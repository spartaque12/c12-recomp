// Native level geometry.
//
// The game draws its level (walls, floors) in 8006737c, inside the render function 8003836c: it walks
// the cells found visible this frame (list at 0x800f8b50, made by 800681a0), projects each face with
// the GTE to integer screen coordinates, splits big ones in screen space, fogs the vertex colours,
// drops faces near or behind the eye and sorts them into the ordering table. That is where the
// wobbling textures, seams and missing floors come from.
//
// Here the same faces are read straight from the level data and drawn as real 3D triangles through
// the renderer's native path (gpu3d.h): per-pixel perspective, a depth buffer, hardware clipping at
// the near plane. 8006737c itself does not run (no PS1 packets for the level).
//
// Timing: the list built in frame N is drawn by the GPU at the start of frame N+1. A capture is
// therefore tied to the ordering table it belongs to and drawn when the front-end walks that table,
// right after its farthest slot (the sky dome lives there), before everything else in the world list.
// The in-between frame (interpolation) gets the same faces seen from a camera halfway between the
// previous and this frame's camera: the level is static, so that is exact.
//
// Data (see re/notes_level_geometry.md, re/notes_frame.md):
//   view struct *0x800efde0: OFX/OFY +0x68/+0x6a (u16), OT size +0x6c, OT base +0x7c, world->view
//   rotation +0xa4 (s16 3x3, 4096 = 1, includes the game's 512x256 stretch), eye +0xb8 (s32 x3),
//   H +0xc4 (u16), fog colour +0xc8 (s16 x3, <<4).
//   POLY chunk (a0): faces +0x10 (16 bytes), vertices +0x14 (8 bytes: s16 x,y,z world, u16 RGB555),
//   explicit UVs +0x18 (8 bytes: 4 x u16 u|v<<8).
//   face: u16 v[4]; u16 mat; u16 uvref; u32 flags (bit0 quad, bit4 semi, bits2-3 sort depth,
//   0xC00 disabled). Materials: *(u32*)0x800f00cc -> 16-byte records (CLUT +6, TPAGE +0xa, corner
//   UVs selected by 3-bit selectors).
//   Fog: depth-cue table u16 *(0x800f95bc) and CLUT-row table u8 *(0x800f95c0), indexed by view z>>6;
//   zone flags 0x800f89a0 (bit0: fogged CLUT rows), fog end 0x800f89f0 (16.16).
#include "psx.h"
#include "gpu.h"
#include "gpu3d.h"
#include "savestate.h"
#include <vector>
#include <math.h>
#include <string.h>

int g_level3d = 1;   // option level3d: 0 = the game's own level renderer

extern float g_ws_scale, g_fov_mult;

static inline u32 rd32(u32 a) { u32 v; memcpy(&v, g_ram + (a & 0x1FFFFF), 4); return v; }
static inline u16 rd16(u32 a) { u16 v; memcpy(&v, g_ram + (a & 0x1FFFFF), 2); return v; }
static inline s16 rds16(u32 a) { return (s16)rd16(a); }
static inline bool ptr_ok(u32 p) { return (p >> 24) == 0x80 && (p & 0x1FFFFF) < RAM_SIZE; }

struct LvVtx { float x, y, z; float u, v; u32 rgb; };   // world space
struct LvTri { LvVtx v[3]; N3Mat m; };
struct Camera { float R[9]; float eye[3]; float h, ofx, ofy; };
struct Capture {
    bool valid = false;
    u32 ot_lo = 0, ot_hi = 0;    // address range of its ordering table
    Camera cam;
    std::vector<LvTri> tris;
};
static Capture g_cap[2];
static int g_cap_next;
static Camera g_last_cam;        // camera of the last main (non-interpolated) emission
static bool g_last_ok;

static void view_of(const Camera& c, const float p[3], float o[3]) {
    float d[3] = {p[0] - c.eye[0], p[1] - c.eye[1], p[2] - c.eye[2]};
    for (int i = 0; i < 3; i++) o[i] = (c.R[i * 3] * d[0] + c.R[i * 3 + 1] * d[1] + c.R[i * 3 + 2] * d[2]);
}

// Hook body for 8006737c (a0 = POLY chunk). Returns false if the game's own routine must run.
bool settings_is_classic();
bool level3d_capture(Ctx* c) {
    if (!g_level3d || !n3_available() || settings_is_classic()) {   // classic view: the game's own renderer
        if (n3_available()) n3_invalidate();                        // PS1 polygons stop testing against stale depth
        for (auto& cp : g_cap) cp.valid = false;
        return false;
    }
    u32 view = rd32(0x800efde0), poly = c->r[4];
    if (!ptr_ok(view) || !ptr_ok(poly)) return false;
    u32 faces = rd32(poly + 0x10), verts = rd32(poly + 0x14), uvtab = rd32(poly + 0x18), mats = rd32(0x800f00cc);
    u16 nverts = rd16(poly + 0x0a);
    if (!ptr_ok(faces) || !ptr_ok(verts) || !ptr_ok(mats)) return false;

    Capture& cap = g_cap[g_cap_next];
    g_cap_next ^= 1;
    cap.tris.clear();
    u32 ot = rd32(view + 0x7c);
    cap.ot_lo = ot & 0x1FFFFF;
    cap.ot_hi = cap.ot_lo + rd16(view + 0x6c) * 4u;
    Camera& cam = cap.cam;
    for (int i = 0; i < 9; i++) cam.R[i] = rds16(view + 0xa4 + i * 2) / 4096.0f;
    for (int i = 0; i < 3; i++) cam.eye[i] = (float)(s32)rd32(view + 0xb8 + i * 4);
    cam.h = (float)rd16(view + 0xc4);
    cam.ofx = (float)rds16(view + 0x68);
    cam.ofy = (float)rds16(view + 0x6a);

    // fog
    u32 dq = rd32(0x800f95bc), fog8 = rd32(0x800f95c0);
    int dqn = rd16(0x800f95c4) + 0x40;
    u16 zflags = rd16(0x800f89a0);
    float far_z = (float)((s32)rd32(0x800f89f0) >> 16);
    float fogc[3];
    for (int i = 0; i < 3; i++) fogc[i] = (float)(rds16(view + 0xc8 + i * 2) >> 4);
    auto dq_at = [&](float z) -> float {
        if (!ptr_ok(dq) || dqn <= 0) return 0.0f;
        int i = z <= 0 ? 0 : (int)z >> 6;
        if (i >= dqn) i = dqn - 1;
        return rd16(dq + i * 2) / 4096.0f;
    };

    int ncell = (int)rd32(0x800f8a2c);
    if (ncell < 0 || ncell > 150) ncell = 0;
    static std::vector<u32> seen;
    seen.clear();
    for (int e = 0; e < ncell; e++) {
        u32 ent = 0x800f8b50 + e * 12;
        int cnt = rd16(ent);
        u32 idx = rd32(ent + 8);
        if (!ptr_ok(idx)) continue;
        for (int k = 0; k < cnt; k++) {
            u32 f = faces + 16u * rd16(idx + 2 * k);
            u32 fl = rd32(f + 12);
            if (fl & 0xC00) continue;
            bool dup = false;
            for (u32 s : seen) if (s == f) { dup = true; break; }
            if (dup) continue;
            seen.push_back(f);
            bool quad = fl & 1;
            int nv = quad ? 4 : 3;
            float P[4][3], V[4][3];
            u32 col[4];
            bool bad = false;
            for (int i = 0; i < nv; i++) {
                u16 vi = rd16(f + 2 * i);
                if (vi >= nverts) { bad = true; break; }
                u32 va = verts + 8u * vi;
                P[i][0] = rds16(va); P[i][1] = rds16(va + 2); P[i][2] = rds16(va + 4);
                col[i] = rd16(va + 6);
                view_of(cam, P[i], V[i]);
            }
            if (bad) continue;
            // the game's draw distance (fog end): nearest vertex beyond it
            float zmin = V[0][2], zmax = V[0][2], zsum = 0;
            for (int i = 0; i < nv; i++) { zmin = fminf(zmin, V[i][2]); zmax = fmaxf(zmax, V[i][2]); zsum += V[i][2]; }
            if (zmax <= 0.0f) continue;                       // entirely behind the eye
            if (far_z > 0 && zmin > far_z) continue;
            // facing, in 3D (front = clockwise on the y-down screen); a quad is kept if either half faces
            auto front = [&](int a, int b, int d) {
                const float *A = V[a], *B = V[b], *D = V[d];
                float det = A[0] * (B[1] * D[2] - B[2] * D[1]) - A[1] * (B[0] * D[2] - B[2] * D[0]) + A[2] * (B[0] * D[1] - B[1] * D[0]);
                return det > 0;
            };
            if (quad ? !(front(0, 1, 2) || front(1, 3, 2)) : !front(0, 1, 2)) continue;
            // material, UVs
            u32 mat = rd32(mats + 4u * rd16(f + 8));
            if (!ptr_ok(mat)) continue;
            u16 tpage = rd16(mat + 10), clut = rd16(mat + 6);
            float sortz = ((fl >> 2) & 3) == 1 ? zmin : ((fl >> 2) & 3) == 2 ? zmax : zsum / nv;
            if ((zflags & 1) && ptr_ok(fog8)) {
                int i = sortz <= 0 ? 0 : (int)sortz >> 6;
                if (i >= dqn) i = dqn - 1;
                clut = (u16)(clut + g_ram[(fog8 + i) & 0x1FFFFF] * 0x40);
            }
            u16 uvref = rd16(f + 10), uv[4];
            for (int i = 0; i < nv; i++) {
                if (uvref & 0x8000) uv[i] = rd16(uvtab + 8u * (uvref & 0x7fff) + 2 * i);
                else uv[i] = rd16(mat + 2 * ((uvref >> (3 * i)) & 7));
            }
            N3Mat m = {};
            m.textured = 1;
            m.semi = (fl & 0x10) ? 1 : 0;
            m.blend = (tpage >> 5) & 3;
            m.depth = (tpage >> 7) & 3;
            if (m.depth > 2) m.depth = 2;
            m.tpx = (tpage & 15) * 64;
            m.tpy = ((tpage >> 4) & 1) * 256;
            m.clutx = (clut & 63) * 16;
            m.cluty = (clut >> 6) & 0x1ff;
            // vertex colour: RGB555 * 248/32, depth-cued toward the fog colour (GTE DCPL, as the game)
            LvVtx L[4];
            for (int i = 0; i < nv; i++) {
                float t = dq_at(V[i][2]);
                u32 rgb = 0;
                for (int ch = 0; ch < 3; ch++) {
                    float base = ((col[i] >> (5 * ch)) & 31) * 7.75f;
                    float v = base + (fogc[ch] - base) * t;
                    int iv = (int)(v + 0.5f);
                    iv = iv < 0 ? 0 : iv > 255 ? 255 : iv;
                    rgb |= (u32)iv << (8 * ch);
                }
                L[i] = LvVtx{P[i][0], P[i][1], P[i][2], (float)(uv[i] & 0xff), (float)(uv[i] >> 8), rgb};
            }
            cap.tris.push_back(LvTri{{L[0], L[1], L[2]}, m});
            if (quad) cap.tris.push_back(LvTri{{L[1], L[3], L[2]}, m});
        }
    }
    cap.valid = true;
    return true;
}

static void lerp_cam(const Camera& a, const Camera& b, float t, Camera& o) {
    for (int i = 0; i < 9; i++) o.R[i] = a.R[i] + (b.R[i] - a.R[i]) * t;
    for (int i = 0; i < 3; i++) o.eye[i] = a.eye[i] + (b.eye[i] - a.eye[i]) * t;
    o.h = a.h + (b.h - a.h) * t;
    o.ofx = b.ofx; o.ofy = b.ofy;
}

// Called by the front-end while walking a display list, once its farthest ordering-table slot has been
// drawn. slot_addr: RAM address of a table slot of this list; interp: drawing the in-between frame
// (blend = move halfway back to the previous frame's camera).
void level3d_emit(u32 slot_addr, bool interp, bool blend, int off_x, int off_y, int cx1, int cy1, int cx2, int cy2) {
    slot_addr &= 0x1FFFFF;
    Capture* cap = nullptr;
    for (auto& c : g_cap) if (c.valid && slot_addr >= c.ot_lo && slot_addr < c.ot_hi) cap = &c;
    if (!cap || !n3_available()) return;
    Camera cam = cap->cam;
    if (interp && blend && g_last_ok) {
        float d = fabsf(cam.eye[0] - g_last_cam.eye[0]) + fabsf(cam.eye[1] - g_last_cam.eye[1]) + fabsf(cam.eye[2] - g_last_cam.eye[2]);
        if (d < 2000.0f) lerp_cam(g_last_cam, cap->cam, 0.5f, cam);   // further = a camera cut: no blend
    }
    N3View v = {};
    v.h = cam.h / g_fov_mult;     // mods widen the field of view the same way as the GTE does
    v.ofx = cam.ofx;
    v.ofy = cam.ofy;
    v.off_x = off_x;
    v.off_y = off_y;
    v.clip_x1 = cx1; v.clip_y1 = cy1; v.clip_x2 = cx2; v.clip_y2 = cy2;
    v.ws_scale = g_ws_scale;
    v.znear = 4.0f;
    v.zfar = 0.0f;
    n3_begin(v);
    for (const LvTri& t : cap->tris) {
        N3Vtx o[3];
        for (int i = 0; i < 3; i++) {
            float p[3] = {t.v[i].x, t.v[i].y, t.v[i].z}, q[3];
            view_of(cam, p, q);
            o[i] = N3Vtx{q[0], q[1], q[2], t.v[i].u, t.v[i].v, t.v[i].rgb};
        }
        n3_tri(o, t.m);
    }
    n3_end();
    if (!interp) { g_last_cam = cap->cam; g_last_ok = true; }
}

// a save state was loaded: captures from before belong to another game
void level3d_reset() {
    for (auto& c : g_cap) c.valid = false;
    g_last_ok = false;
}
