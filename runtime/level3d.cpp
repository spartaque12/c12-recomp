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

struct LvVtx { float x, y, z; float u, v; u32 rgb; };   // world space (level) or the group's model space
struct LvTri { LvVtx v[3]; N3Mat m; };
struct Camera { float R[9]; float eye[3]; float h, ofx, ofy; };
// A group of triangles sharing one model->world transform (the level: identity). Moving objects are
// keyed (object address, part) so the in-between frame can find last frame's transform and blend.
struct Group { uint64_t key; float M[12]; u32 first, count; };   // M: R row-major 3x3, then t
struct Capture {
    bool valid = false, drawn = false;
    u32 seq = 0;
    u32 ot_lo = 0, ot_hi = 0;    // address range of its ordering table
    Camera cam;
    std::vector<LvTri> tris;
    std::vector<Group> groups;
};
static Capture g_cap[2];
static int g_cap_cur;
static u32 g_cap_seq;
static Camera g_last_cam;        // camera of the last main (non-interpolated) emission
static bool g_last_ok;
static std::vector<Group> g_last_groups;   // transforms of the last main emission (in-between frames)

static void view_of(const Camera& c, const float p[3], float o[3]) {
    float d[3] = {p[0] - c.eye[0], p[1] - c.eye[1], p[2] - c.eye[2]};
    for (int i = 0; i < 3; i++) o[i] = (c.R[i * 3] * d[0] + c.R[i * 3 + 1] * d[1] + c.R[i * 3 + 2] * d[2]);
}
static void xform(const float M[12], const float p[3], float o[3]) {
    for (int i = 0; i < 3; i++) o[i] = M[i * 3] * p[0] + M[i * 3 + 1] * p[1] + M[i * 3 + 2] * p[2] + M[9 + i];
}

bool settings_is_classic();
bool scene3d_enabled() { return g_level3d && n3_available() && !settings_is_classic(); }

// The capture that native geometry of the main view's current ordering table goes to. A new one starts
// when the game builds into another table (they alternate) or the current one has been drawn.
static Capture* scene_cap() {
    u32 view = rd32(0x800efde0);
    if (!ptr_ok(view)) return nullptr;
    u32 ot = rd32(view + 0x7c) & 0x1FFFFF;
    Capture& c = g_cap[g_cap_cur];
    if (c.valid && !c.drawn && c.ot_lo == ot) return &c;
    g_cap_cur ^= 1;
    Capture& n = g_cap[g_cap_cur];
    n.valid = true; n.drawn = false; n.seq = ++g_cap_seq;
    n.tris.clear(); n.groups.clear();
    n.ot_lo = ot;
    n.ot_hi = ot + rd16(view + 0x6c) * 4u;
    Camera& cam = n.cam;
    for (int i = 0; i < 9; i++) cam.R[i] = rds16(view + 0xa4 + i * 2) / 4096.0f;
    for (int i = 0; i < 3; i++) cam.eye[i] = (float)(s32)rd32(view + 0xb8 + i * 4);
    cam.h = (float)rd16(view + 0xc4);
    cam.ofx = (float)rds16(view + 0x68);
    cam.ofy = (float)rds16(view + 0x6a);
    return &n;
}
static const float k_ident[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
static Group& group_begin(Capture& cap, uint64_t key, const float M[12]) {
    Group g;
    g.key = key;
    memcpy(g.M, M, sizeof g.M);
    g.first = (u32)cap.tris.size();
    g.count = 0;
    cap.groups.push_back(g);
    return cap.groups.back();
}
static void group_end(Capture& cap) {
    Group& g = cap.groups.back();
    g.count = (u32)cap.tris.size() - g.first;
    if (!g.count) cap.groups.pop_back();
}

// Hook body for 8006737c (a0 = POLY chunk). Returns false if the game's own routine must run.
bool level3d_capture(Ctx* c) {
    if (!scene3d_enabled()) {   // classic view / switched off: the game's own renderer
        if (n3_available()) n3_invalidate();                        // PS1 polygons stop testing against stale depth
        for (auto& cp : g_cap) cp.valid = false;
        return false;
    }
    u32 view = rd32(0x800efde0), poly = c->r[4];
    if (!ptr_ok(view) || !ptr_ok(poly)) return false;
    u32 faces = rd32(poly + 0x10), verts = rd32(poly + 0x14), uvtab = rd32(poly + 0x18), mats = rd32(0x800f00cc);
    u16 nverts = rd16(poly + 0x0a);
    if (!ptr_ok(faces) || !ptr_ok(verts) || !ptr_ok(mats)) return false;
    Capture* cp = scene_cap();
    if (!cp) return false;
    Capture& cap = *cp;
    const Camera& cam = cap.cam;
    group_begin(cap, 0, k_ident);

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
    group_end(cap);
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
// (blend = move halfway back to the previous frame: camera and every object found in both frames).
void level3d_emit(u32 slot_addr, bool interp, bool blend, int off_x, int off_y, int cx1, int cy1, int cx2, int cy2) {
    slot_addr &= 0x1FFFFF;
    Capture* cap = nullptr;
    for (auto& c : g_cap)
        if (c.valid && slot_addr >= c.ot_lo && slot_addr < c.ot_hi && (!cap || c.seq > cap->seq)) cap = &c;
    if (!cap || !n3_available()) return;
    Camera cam = cap->cam;
    bool mix = interp && blend && g_last_ok;
    if (mix) {
        float d = fabsf(cam.eye[0] - g_last_cam.eye[0]) + fabsf(cam.eye[1] - g_last_cam.eye[1]) + fabsf(cam.eye[2] - g_last_cam.eye[2]);
        if (d < 2000.0f) lerp_cam(g_last_cam, cap->cam, 0.5f, cam);   // further = a camera cut: no blend
        else mix = false;
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
    size_t li = 0;   // last frame's groups come mostly in the same order: search on from the last hit
    for (const Group& g : cap->groups) {
        float M[12];
        memcpy(M, g.M, sizeof M);
        if (mix && g.key) {
            const Group* p = nullptr;
            size_t n = g_last_groups.size();
            for (size_t k = 0; k < n && !p; k++) {
                const Group& q = g_last_groups[(li + k) % n];
                if (q.key == g.key) { p = &q; li = (li + k + 1) % n; }
            }
            if (p && fabsf(p->M[9] - M[9]) + fabsf(p->M[10] - M[10]) + fabsf(p->M[11] - M[11]) < 1500.0f)
                for (int i = 0; i < 12; i++) M[i] = (M[i] + p->M[i]) * 0.5f;
        }
        bool ident = g.key == 0;
        for (u32 ti = g.first; ti < g.first + g.count; ti++) {
            const LvTri& t = cap->tris[ti];
            N3Vtx o[3];
            for (int i = 0; i < 3; i++) {
                float p[3] = {t.v[i].x, t.v[i].y, t.v[i].z}, w[3], q[3];
                if (ident) view_of(cam, p, q);
                else { xform(M, p, w); view_of(cam, w, q); }
                o[i] = N3Vtx{q[0], q[1], q[2], t.v[i].u, t.v[i].v, t.v[i].rgb};
            }
            n3_tri(o, t.m);
        }
    }
    n3_end();
    if (!interp) { g_last_cam = cap->cam; g_last_ok = true; g_last_groups = cap->groups; cap->drawn = true; }
}

// a save state was loaded: captures from before belong to another game
void level3d_reset() {
    for (auto& c : g_cap) c.valid = false;
    g_last_ok = false;
    g_last_groups.clear();
}

// ---- MeshFX effect meshes (80086810; see re/notes_models.md) ---------------------------------
// Shockwave rings, explosion shells, beams, flashes: small meshes with a colour palette, no lighting.
// Captured in model space with the instance's model->world matrix. Lines and screen-mapped (heat
// haze) faces, screen-space/anchored, fixed-slot and fading instances stay on the PS1 path.
static void rd_matrix(u32 a, float R[9], s32 t[3]) {
    for (int i = 0; i < 9; i++) R[i] = rds16(a + i * 2) / 4096.0f;
    for (int i = 0; i < 3; i++) t[i] = (s32)rd32(a + 0x14 + i * 4);
}
static u32 dpc(u32 c, int ir0, const s16 fc[3]) {   // GTE DPCS: c + IR0 * (FC - c) (sf=1, lm=0)
    u32 o = 0;
    for (int ch = 0; ch < 3; ch++) {
        s32 ci = (c >> (8 * ch)) & 0xff;
        s32 ir = fc[ch] - (ci << 4);
        if (ir > 32767) ir = 32767;
        if (ir < -32768) ir = -32768;
        s32 r = ((ci << 16) + ir0 * ir) >> 16;
        r = r < 0 ? 0 : r > 255 ? 255 : r;
        o |= (u32)r << (8 * ch);
    }
    return o;
}

// Hook body for 80086810 (a0 = instance, a1 = view). Returns false if the original must run unchanged.
static int g_fx_native, g_fx_mixed, g_fx_ps1, g_fx_frame;
static void fx_count(int* k) {
    if (g_frame / 250 != g_fx_frame) {
        if (g_fx_native + g_fx_mixed + g_fx_ps1) logf("meshfx frames %d..: native %d, mixed %d, PS1 %d\n", g_fx_frame * 250, g_fx_native, g_fx_mixed, g_fx_ps1);
        g_fx_frame = g_frame / 250; g_fx_native = g_fx_mixed = g_fx_ps1 = 0;
    }
    (*k)++;
}
static int meshfx_do(Ctx* c, RecompFn orig);
bool meshfx_capture(Ctx* c, RecompFn orig) {
    int r = meshfx_do(c, orig);
    if (scene3d_enabled()) fx_count(r == 0 ? &g_fx_ps1 : r == 1 ? &g_fx_native : &g_fx_mixed);
    return r != 0;
}
// 0 = not handled (the original runs), 1 = drawn natively, 2 = native + the rest through the original
static int meshfx_do(Ctx* c, RecompFn orig) {
    if (!scene3d_enabled()) return 0;
    u32 obj = c->r[4], view = c->r[5];
    if (!ptr_ok(obj) || view != rd32(0x800efde0)) return 0;
    u32 fl = rd32(obj + 0xc);
    if (fl & (0x2 | 0x4 | 0x8 | 0x20000 | 0x800000)) return 0;
    if (rd32(obj + 0x38) != 0 || g_ram[(obj + 0x4a) & 0x1FFFFF] != 3) return 0;
    u32 lp = rd32(obj + 0x14), pp = rd32(obj + 0x20), verts = rd32(obj + 0x2c), faces = rd32(obj + 0x30), pal = rd32(obj + 0x34);
    int nv = rd16(obj + 0x24), nf = rd16(obj + 0x26);
    if (!ptr_ok(lp) || !ptr_ok(verts) || !ptr_ok(faces) || !ptr_ok(pal) || nf <= 0 || nf > 2000 || nv <= 0 || nv > 4000) return 0;
    // model -> world
    float LR[9], M[12];
    s32 Lt[3];
    rd_matrix(lp, LR, Lt);
    if (ptr_ok(pp)) {
        float PR[9];
        s32 Pt[3];
        rd_matrix(pp, PR, Pt);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) M[i * 3 + j] = PR[i * 3] * LR[j] + PR[i * 3 + 1] * LR[3 + j] + PR[i * 3 + 2] * LR[6 + j];
        float lt[3] = {(float)(s16)Lt[0], (float)(s16)Lt[1], (float)(s16)Lt[2]};   // enters the GTE as s16
        for (int i = 0; i < 3; i++) M[9 + i] = Pt[i] + PR[i * 3] * lt[0] + PR[i * 3 + 1] * lt[1] + PR[i * 3 + 2] * lt[2];
    } else {
        memcpy(M, LR, sizeof LR);
        for (int i = 0; i < 3; i++) M[9 + i] = (float)Lt[i];
    }
    Capture* cp = scene_cap();
    if (!cp) return 0;
    Capture& cap = *cp;
    const Camera& cam = cap.cam;
    float zlim = (float)rd16(view + 0x72);
    if (fl & 0x10) {   // per-instance sort: dropped if the origin is beyond the draw distance
        float o[3];
        view_of(cam, M + 9, o);
        if (o[2] > zlim) return 1;
    }
    int ir0 = rds16(obj + 0x46);
    if (fl & 0x200000) {
        u32 h = rd32(0x800efdb8);
        if (ptr_ok(h)) { u32 hb = rd32(h + 0xb0); if (ptr_ok(hb) && (rd32(hb + 0x10) & 0x8000000)) ir0 = 0; }
    }
    s16 fc[3];
    for (int i = 0; i < 3; i++) fc[i] = rds16(view + 0xc8 + i * 2);
    int poff = rds16(obj + 0x48);
    auto pal_at = [&](s32 ci) { return rd32(pal + 4u * (u32)(ci <= 0 ? -ci : ci + poff)); };

    group_begin(cap, (uint64_t)obj, M);
    static std::vector<u32> mine;   // faces drawn natively (switched off for the original)
    mine.clear();
    int left = 0, base = 0;
    for (int fi = 0; fi < nf; fi++) {
        u32 f = faces + 0x20u * fi;
        u8 type = g_ram[f & 0x1FFFFF], b1 = g_ram[(f + 1) & 0x1FFFFF];
        if (b1 & 0x20) base = rd16(f + 0x18);
        if (b1 & 0x40) continue;
        int n = type & 7;
        if (n < 3 || n > 4 || (type & 0x50)) { left++; continue; }   // lines, screen-mapped: PS1 path
        float P[4][3], V[4][3];
        u32 col[4];
        bool bad = false;
        for (int i = 0; i < n; i++) {
            int vi = base + rd16(f + 8 + 2 * i);
            if (vi >= nv) { bad = true; break; }
            u32 va = verts + 8u * vi;
            P[i][0] = rds16(va); P[i][1] = rds16(va + 2); P[i][2] = rds16(va + 4);
            float w[3];
            xform(M, P[i], w);
            view_of(cam, w, V[i]);
            s32 ci = (fl & 0x40) ? rds16(va + 6) : rds16(f + 0x10 + 2 * ((type & 8) ? i : 0));
            col[i] = pal_at(ci);
        }
        if (bad) { left++; continue; }
        mine.push_back(f);
        if (!(fl & 0x10)) {   // per-polygon sort: the game's far limit (min/max ignore a quad's 4th vertex)
            int m3 = n > 3 ? 3 : n;
            float z = 0;
            if (b1 & 2) { z = V[0][2]; for (int i = 1; i < m3; i++) z = fminf(z, V[i][2]); }
            else if (b1 & 4) { z = V[0][2]; for (int i = 1; i < m3; i++) z = fmaxf(z, V[i][2]); }
            else { for (int i = 0; i < n; i++) z += V[i][2]; z /= n; }
            if (z > zlim) continue;
        }
        float zmax = V[0][2];
        for (int i = 1; i < n; i++) zmax = fmaxf(zmax, V[i][2]);
        if (zmax <= 0.0f) continue;
        if (fl & 0x80) {   // back-face culling only on request (NCLIP on v0..v2)
            const float *A = V[0], *B = V[1], *D = V[2];
            float det = A[0] * (B[1] * D[2] - B[2] * D[1]) - A[1] * (B[0] * D[2] - B[2] * D[0]) + A[2] * (B[0] * D[1] - B[1] * D[0]);
            if (det <= 0) continue;
        }
        u8 cmd = g_ram[(f + 6) & 0x1FFFFF], b7 = g_ram[(f + 7) & 0x1FFFFF];
        N3Mat m = {};
        m.textured = (type & 0x20) ? 1 : 0;
        m.semi = (cmd & 2) ? 1 : 0;
        m.blend = (b7 >> 5) & 3;
        m.depth = 0;
        m.tpx = (b7 & 15) * 64;
        m.tpy = ((b7 >> 4) & 1) * 256;
        u16 clut = rd16(f + 2);
        m.clutx = (clut & 63) * 16;
        m.cluty = (clut >> 6) & 0x1ff;
        LvVtx L[4];
        for (int i = 0; i < n; i++) {
            u16 uv = rd16(f + 0x18 + 2 * i);
            u32 rgb = dpc(col[(type & 8) ? i : 0], ir0, fc);
            L[i] = LvVtx{P[i][0], P[i][1], P[i][2], (float)(uv & 0xff), (float)(uv >> 8), rgb};
        }
        cap.tris.push_back(LvTri{{L[0], L[1], L[2]}, m});
        if (n == 4) cap.tris.push_back(LvTri{{L[1], L[3], L[2]}, m});
    }
    group_end(cap);
    if (left) {   // the rest through the game's own code, with the native faces switched off
        for (u32 f : mine) g_ram[(f + 1) & 0x1FFFFF] |= 0x40;
        orig(c);
        for (u32 f : mine) g_ram[(f + 1) & 0x1FFFFF] &= ~0x40;
        return 2;
    }
    return 1;
}
