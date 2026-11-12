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
#include <array>
#include <math.h>
#include <string.h>

int g_level3d = 1;   // option level3d: 0 = the game's own level renderer
int g_n3_parts = 7;  // option n3_parts (debug): native 1 level, 2 effect meshes, 4 objects

extern float g_ws_scale, g_fov_mult;

static inline u32 rd32(u32 a) { u32 v; memcpy(&v, g_ram + (a & 0x1FFFFF), 4); return v; }
static inline u16 rd16(u32 a) { u16 v; memcpy(&v, g_ram + (a & 0x1FFFFF), 2); return v; }
static inline s16 rds16(u32 a) { return (s16)rd16(a); }
static inline bool ptr_ok(u32 p) { return (p >> 24) == 0x80 && (p & 0x1FFFFF) < RAM_SIZE; }

struct LvVtx { float x, y, z; float u, v; u32 rgb; int g = -1; };   // g < 0: world space (level), else model space of group g
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
    g.count = (u32)cap.tris.size() - g.first;   // kept even if empty: other groups' triangles may use its vertices
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
    // every group's transform for this picture (blended with last frame's for the in-between frame)
    static std::vector<std::array<float, 12>> gm;
    gm.resize(cap->groups.size());
    size_t li = 0;   // last frame's groups come mostly in the same order: search on from the last hit
    for (size_t gi = 0; gi < cap->groups.size(); gi++) {
        const Group& g = cap->groups[gi];
        float* M = gm[gi].data();
        memcpy(M, g.M, sizeof g.M);
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
    }
    for (const LvTri& t : cap->tris) {
        N3Vtx o[3];
        for (int i = 0; i < 3; i++) {
            const LvVtx& lv = t.v[i];
            float p[3] = {lv.x, lv.y, lv.z}, w[3], q[3];
            if (lv.g < 0) view_of(cam, p, q);
            else { xform(gm[lv.g].data(), p, w); view_of(cam, w, q); }
            o[i] = N3Vtx{q[0], q[1], q[2], lv.u, lv.v, lv.rgb};
        }
        n3_tri(o, t.m);
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
        extern int g_dbg_frame;
        if (g_dbg_frame >= 0 && g_fx_native + g_fx_mixed + g_fx_ps1) logf("meshfx frames %d..: native %d, mixed %d, PS1 %d\n", g_fx_frame * 250, g_fx_native, g_fx_mixed, g_fx_ps1);
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
    if (!scene3d_enabled() || !(g_n3_parts & 2)) return 0;
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
            L[i] = LvVtx{P[i][0], P[i][1], P[i][2], (float)(uv & 0xff), (float)(uv >> 8), rgb, (int)cap.groups.size() - 1};
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

// ---- PT_MODEL objects (800b8cb4; see re/notes_models_other.md) -------------------------------
// The hero, enemies, weapons, doors, crates, props: rigid parts, each with its own world matrix (built
// by the game in 800a2fd0), drawn by small per-polygon-type drivers that project, cull, light (GTE
// NCDS/NCDT: BK + LCM * light directions in part space, fogged by the depth-cue curve) and sort.
// Here the same records are read and lit on the CPU; each part becomes one group keyed (model, part)
// so the in-between frame blends every part's own motion. Instances with a private sub-ordering-table,
// vertex morphing (MIME) or a polygon type not handled here go through the game's drivers instead.
u32 mem_r32_slow(Ctx* c, u32 a);
static int g_pm_native, g_pm_ps1, g_pm_frame, g_pm_why;

struct PmLight {
    float L[9];          // light directions (rows), world space
    float LLM0[9];       // the GTE's LLM as found (groups that do not move the lights into part space)
    float LCM[9];        // light colours (columns per light)
    float BK[3];         // ambient, 1.0 = 4096
    float FC[3];         // fog colour (0..255)
    float dqa, dqb, h;   // depth cue: IR0 = DQB + DQA * H / z
    float flat[3];       // flat colour (flat / unlit mode)
};

static inline float s16f(u32 w, int hi) { return (float)(s16)(hi ? (w >> 16) : (w & 0xffff)); }

static void read_mat3(Ctx* c, int r0, float o[9]) {   // GTE control registers r0..r0+4: 3x3 s16, packed
    u32 w[5];
    for (int i = 0; i < 5; i++) w[i] = gte_cfc2(c, r0 + i);
    float v[9] = {s16f(w[0], 0), s16f(w[0], 1), s16f(w[1], 0), s16f(w[1], 1), s16f(w[2], 0), s16f(w[2], 1), s16f(w[3], 0), s16f(w[3], 1), s16f(w[4], 0)};
    for (int i = 0; i < 9; i++) o[i] = v[i] / 4096.0f;
}

// colour of one vertex: RGBC * (BK + LCM * clamp0(LLM * n)) (or * flat colour), then toward the fog colour
static u32 pm_colour(const PmLight& lt, u32 rgbc, const float* LLM, const float n[3], bool flat, float z) {
    float ir[3];
    if (flat) { ir[0] = lt.flat[0]; ir[1] = lt.flat[1]; ir[2] = lt.flat[2]; }
    else {
        float l[3];
        for (int i = 0; i < 3; i++) l[i] = fmaxf(0.0f, LLM[i * 3] * n[0] + LLM[i * 3 + 1] * n[1] + LLM[i * 3 + 2] * n[2]);
        for (int i = 0; i < 3; i++) ir[i] = fminf(fmaxf(lt.BK[i] + lt.LCM[i * 3] * l[0] + lt.LCM[i * 3 + 1] * l[1] + lt.LCM[i * 3 + 2] * l[2], 0.0f), 7.999f);
    }
    float t = 0.0f;
    if (z > 0.0f) t = (lt.dqb + lt.dqa * fminf(lt.h * 65536.0f / z, 131071.0f)) / 16777216.0f;   // IR0 / 4096
    t = fminf(fmaxf(t, 0.0f), 1.0f);
    u32 o = 0;
    for (int ch = 0; ch < 3; ch++) {
        float cc = (float)((rgbc >> (8 * ch)) & 0xff) * ir[ch];
        cc = cc + (lt.FC[ch] - cc) * t;
        int iv = (int)cc;
        iv = iv < 0 ? 0 : iv > 255 ? 255 : iv;
        o |= (u32)iv << (8 * ch);
    }
    return o;
}

static int ptmodel_do(Ctx* c);
bool ptmodel_capture(Ctx* c) {
    int r = ptmodel_do(c);
    if (scene3d_enabled()) {
        if (g_frame / 250 != g_pm_frame) {
            extern int g_dbg_frame;
            if (g_dbg_frame >= 0 && g_pm_native + g_pm_ps1) logf("models frames %d..: native %d, PS1 %d\n", g_pm_frame * 250, g_pm_native, g_pm_ps1);
            g_pm_frame = g_frame / 250;
            g_pm_native = g_pm_ps1 = 0;
        }
        (r ? g_pm_native : g_pm_ps1)++;
        extern int g_dbg_frame;
        if (!r && g_pm_ps1 == 1 && g_dbg_frame >= 0) logf("models: PS1 fallback, reason %d (model %08x)\n", g_pm_why, c->r[4]);
    }
    return r != 0;
}

// Hook body for 800b8cb4 (a0 = model, a1 = instance, a2 = view, a3 = cull mode). 0 = the original must run.
static int ptmodel_do(Ctx* c) {
    if (!scene3d_enabled() || !(g_n3_parts & 4)) { g_pm_why = 1; return 0; }
    u32 model = c->r[4], inst = c->r[5], view = c->r[6];
    if (!ptr_ok(model) || !ptr_ok(inst) || view != rd32(0x800efde0)) { g_pm_why = 2; return 0; }
    if (rd16(inst + 0x34) != 0) { g_pm_why = 3; return 0; }                       // private sub-ordering-table
    u32 src = rd32(model + 0x28), pstate = rd32(model + 0x20), coords = rd32(model + 0x24);
    if (!ptr_ok(src) || !ptr_ok(pstate) || !ptr_ok(coords) || rd32(src) != 0x33305453) { g_pm_why = 4; return 0; }   // 'ST03'
    u32 sparts = rd32(src + 8);
    int nparts = rd16(model + 0x1c), ncoords = rd16(model + 0x1e);
    if (!ptr_ok(sparts) || nparts <= 0 || nparts > 64) { g_pm_why = 5; return 0; }
    u32 mflags = rd32(model + 0xc), iflags = rd32(inst + 0xc);
    u16 srcflags = rd16(src + 4);

    PmLight lt;
    for (int i = 0; i < 9; i++) lt.L[i] = (float)(s16)(mem_r32_slow(c, 0x1f800020 + (i & ~1) * 2) >> ((i & 1) * 16)) / 4096.0f;
    read_mat3(c, 8, lt.LLM0);
    read_mat3(c, 16, lt.LCM);
    for (int i = 0; i < 3; i++) lt.BK[i] = (float)(s32)gte_cfc2(c, 13 + i) / 4096.0f;
    for (int i = 0; i < 3; i++) lt.FC[i] = (float)(s32)gte_cfc2(c, 21 + i) / 16.0f;
    lt.h = (float)(gte_cfc2(c, 26) & 0xffff);
    lt.dqa = (float)(s16)gte_cfc2(c, 27);
    lt.dqb = (float)(s32)gte_cfc2(c, 28);
    u32 f88 = mem_r32_slow(c, 0x1f800088), f8c = mem_r32_slow(c, 0x1f80008c);
    lt.flat[0] = s16f(f88, 0) / 4096.0f; lt.flat[1] = s16f(f88, 1) / 4096.0f; lt.flat[2] = s16f(f8c, 0) / 4096.0f;

    Capture* cp = scene_cap();
    if (!cp) { g_pm_why = 6; return 0; }
    Capture& cap = *cp;
    const Camera& cam = cap.cam;
    size_t tri0 = cap.tris.size(), grp0 = cap.groups.size();
    auto give_up = [&]() { cap.tris.resize(tri0); cap.groups.resize(grp0); return 0; };
    float zlim = (float)rd16(view + 0x6c) * 8.0f;   // the game keeps slots 2..len-1 (z >> 3)

    struct Slot { float p[3]; int g; };
    static Slot slot[256];       // the drivers' vertex scratch: position in the space of the part that loaded it
    for (auto& sl : slot) sl.g = -2;
    float LLM[9];
    memcpy(LLM, lt.LLM0, sizeof LLM);
    for (int pi = 0; pi < nparts; pi++) {
        u32 s = pstate + 0x10u * pi, p = sparts + 0x14u * pi;
        u16 sflags = rd16(s);
        if (sflags & 2) continue;                                // hidden part
        int ngroups = rd16(p + 4), gi = rd16(s + 2);
        if (gi >= ngroups) continue;
        u32 groups = rd32(p + 0x10);
        if (!ptr_ok(groups)) continue;
        u32 g = groups + 0x1cu * gi;
        int ci = g_ram[(p + 6) & 0x1FFFFF];
        if (ci >= ncoords) continue;
        if ((mflags & 0x80) && (sflags & 1) && rd16(s + 6) != 0) return (g_pm_why = __LINE__, give_up());   // vertex morph (MIME)
        u32 C = coords + 0x70u * ci;
        float M[12];
        s32 t[3];
        rd_matrix(C + 0x30, M, t);
        for (int i = 0; i < 3; i++) M[9 + i] = (float)t[i];
        u16 gflags = rd16(g);
        bool part_space_lights = (gflags & 1) && !((iflags | sflags) & 0x20) && !(srcflags & 0x40);
        // light directions into part space (L x C.rot); other groups keep whatever the GTE holds, i.e.
        // the last lit part's
        if (part_space_lights)
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) LLM[i * 3 + j] = lt.L[i * 3] * M[j] + lt.L[i * 3 + 1] * M[3 + j] + lt.L[i * 3 + 2] * M[6 + j];
        u32 arrA = rd32(g + 0xc), arrB = rd32(g + 0x10), ibuf = rd32(s + 0xc);
        u32 d8 = (u32)rd16(p + 8) + rd16(p + 0xa);
        u32 nbase = arrB;
        uint64_t key = (1ull << 63) | ((uint64_t)model << 8) | (u32)pi;
        group_begin(cap, key, M);
        u32 blk = rd32(g + 0x14);
        int nblocks = rd16(g + 8);
        for (int b = 0; b < nblocks && ptr_ok(blk); b++) {
            int id = rd16(blk), cnt = rd16(blk + 2);
            u16 bflags = rd16(blk + 4), bsize = rd16(blk + 6);
            u32 fl = iflags | sflags | bflags;
            bool flat = (fl & 0x20) || (srcflags & 0x40);
            u32 r = blk + 8;
            // record sizes and the game's drivers (9 / 0xa, per-vertex colour without texture: not handled)
            static const int rsize[13] = {8, 0xc, 0xc, 0x18, 0x18, 0x10, 0x10, 0x1c, 0x20, 0x18, 0x1c, 0x24, 0x2c};
            static const u32 drv[13] = {0x800b9320, 0x800aa160, 0x800aa57c, 0x800a8d74, 0x800a91c0, 0x800aa350, 0x800aa7a4, 0x800a8f7c, 0x800a9408, 0, 0, 0x800a8738, 0x800a8a0c};
            u32 dt = rd32(inst + 0x30), de = (ptr_ok(dt) && id < 13) ? rd32(dt + 4u * id) : 0;
            if (id >= 13 || !drv[id] || !ptr_ok(de) || rd32(de + 4) != drv[id]) { group_end(cap); return (g_pm_why = 1000 + id, give_up()); }
            for (int k = 0; k < cnt; k++, r += rsize[id]) {
                u32 w0 = rd32(r), w1 = rd32(r + 4);
                if (id == 0) {   // vertices into the scratch slots
                    int type = w0 & 0xffff, si = w0 >> 16, dst = w1 & 0xffff, n = w1 >> 16;
                    u32 a;
                    switch (type & 3) {
                    case 0: a = arrA; break;
                    case 1: a = ibuf; break;
                    case 2: a = ibuf + d8 * 8; break;
                    default: a = arrB; break;
                    }
                    if (type >= 4) { nbase = a; continue; }
                    if (!ptr_ok(a)) { group_end(cap); return (g_pm_why = __LINE__, give_up()); }
                    for (int v = 0; v < n && dst + v < 256; v++) {
                        u32 va = a + 8u * (si + v);
                        Slot& sl = slot[dst + v];
                        sl.p[0] = rds16(va); sl.p[1] = rds16(va + 2); sl.p[2] = rds16(va + 4);
                        sl.g = (int)cap.groups.size() - 1;
                    }
                    continue;
                }
                bool quad = (id & 1) == 0, tex = id == 3 || id == 4 || id == 7 || id == 8 || id == 11 || id == 12, gour = id >= 5;
                u32 rgbcv[4] = {w1 & 0xffffff, w1 & 0xffffff, w1 & 0xffffff, w1 & 0xffffff};   // 0xb / 0xc: one per vertex
                if (id >= 11) for (int i = 0; i < 4; i++) rgbcv[i] = rd32(r + 4 + 4 * i) & 0xffffff;
                int nv = quad ? 4 : 3;
                int vi[4] = {(int)(w0 & 0xff), (int)((w0 >> 8) & 0xff), (int)((w0 >> 16) & 0xff), (int)(w0 >> 24)};
                // normal slot per vertex
                int ni[4] = {0, 0, 0, 0};
                u32 w2 = rd32(r + 8), w3 = rd32(r + 12);
                switch (id) {
                case 1: case 2: ni[0] = ni[1] = ni[2] = ni[3] = w2 & 0xffff; break;
                case 3: ni[0] = ni[1] = ni[2] = rd32(r + 20) & 0xffff; break;
                case 4: ni[0] = ni[1] = ni[2] = ni[3] = rd16(r + 0x16); break;
                case 5: ni[0] = w2 & 0xffff; ni[1] = w2 >> 16; ni[2] = w3 & 0xffff; break;
                case 6: ni[0] = w2 & 0xffff; ni[1] = w2 >> 16; ni[2] = w3 & 0xffff; ni[3] = w3 >> 16; break;
                case 7: { u32 w5 = rd32(r + 20), w6 = rd32(r + 24); ni[0] = w5 & 0xffff; ni[1] = w5 >> 16; ni[2] = w6 & 0xffff; break; }
                case 8: { u32 w5 = rd32(r + 20), w6 = rd32(r + 24), w7 = rd32(r + 28); ni[0] = w5 >> 16; ni[1] = w6 & 0xffff; ni[2] = w6 >> 16; ni[3] = w7 & 0xffff; break; }
                case 11: ni[0] = rd16(r + 0x1c); ni[1] = rd16(r + 0x1e); ni[2] = rd16(r + 0x20); break;
                case 12: ni[0] = rd16(r + 0x22); ni[1] = rd16(r + 0x24); ni[2] = rd16(r + 0x26); ni[3] = rd16(r + 0x28); break;
                }
                (void)gour;
                float P[4][3], V[4][3];
                int pg[4];
                bool unset = false;
                for (int i = 0; i < nv; i++) {
                    const Slot& sl = slot[vi[i]];
                    if (sl.g < 0) { unset = true; break; }
                    memcpy(P[i], sl.p, sizeof P[i]);
                    pg[i] = sl.g;   // a joint's vertices may come from the previous part (its own matrix)
                    float w[3];
                    xform(cap.groups[sl.g].M, P[i], w);
                    view_of(cam, w, V[i]);
                }
                if (unset) continue;
                // the game's depth range (average z, as AVSZ3/AVSZ4)
                float zs = 0;
                for (int i = 0; i < nv; i++) zs += V[i][2];
                zs /= nv;
                if (zs >= zlim) continue;
                float zmax = V[0][2];
                for (int i = 1; i < nv; i++) zmax = fmaxf(zmax, V[i][2]);
                if (zmax <= 0.0f) continue;
                if (!(fl & 8)) {   // back faces (NCLIP: a quad is kept if either half faces front)
                    auto front = [&](int a, int bb, int d) {
                        const float *A = V[a], *B = V[bb], *D = V[d];
                        return A[0] * (B[1] * D[2] - B[2] * D[1]) - A[1] * (B[0] * D[2] - B[2] * D[0]) + A[2] * (B[0] * D[1] - B[1] * D[0]) >= 0;
                    };
                    if (quad ? !(front(1, 2, 0) || front(2, 0, 3)) : !front(0, 1, 2)) continue;
                }
                N3Mat m = {};
                m.textured = tex;
                m.semi = ((w1 >> 24) & 2) || (fl & 0x10);
                float uv[4][2] = {};
                if (tex) {
                    int uo = id == 11 ? 0x10 : id == 12 ? 0x14 : 8;   // uv0|clut, uv1|tpage, uv2 (, uv3)
                    u32 u0 = rd32(r + uo), u1 = rd32(r + uo + 4), u2 = rd32(r + uo + 8);
                    u16 clut = u0 >> 16, tpage = u1 >> 16;
                    u16 uvs[4] = {(u16)u0, (u16)u1, (u16)u2, 0};
                    if (id == 4) uvs[3] = (u16)(u2 >> 16);
                    if (id == 8) uvs[3] = (u16)rd32(r + 20);
                    if (id == 12) uvs[3] = rd16(r + 0x20);
                    for (int i = 0; i < nv; i++) { uv[i][0] = (float)(uvs[i] & 0xff); uv[i][1] = (float)(uvs[i] >> 8); }
                    m.blend = (tpage >> 5) & 3;
                    m.depth = (tpage >> 7) & 3;
                    if (m.depth > 2) m.depth = 2;
                    m.tpx = (tpage & 15) * 64;
                    m.tpy = ((tpage >> 4) & 1) * 256;
                    m.clutx = (clut & 63) * 16;
                    m.cluty = (clut >> 6) & 0x1ff;
                }
                LvVtx L[4];
                u32 rgbc = w1 & 0xffffff;
                for (int i = 0; i < nv; i++) {
                    u32 na = nbase + 8u * ni[i];
                    float n[3] = {rds16(na) / 4096.0f, rds16(na + 2) / 4096.0f, rds16(na + 4) / 4096.0f};
                    u32 col = pm_colour(lt, rgbcv[i], LLM, n, flat, V[i][2]);
                    L[i] = LvVtx{P[i][0], P[i][1], P[i][2], uv[i][0], uv[i][1], col, pg[i]};
                }
                if (!gour && !flat) for (int i = 1; i < nv; i++) L[i].rgb = L[0].rgb;   // flat-shaded types: one colour
                { extern int g_dbg_frame; if (g_dbg_frame == g_frame) logf("pm part %d blk %d id %d fl %x rgbc %06x v %d,%d,%d,%d n %d,%d,%d,%d col %06x %06x %06x z %.0f vy %.0f tex %d semi %d tp %d,%d clut %d,%d uv %.0f,%.0f %.0f,%.0f %.0f,%.0f\n", pi, b, id, fl, rgbc, vi[0], vi[1], vi[2], vi[3], ni[0], ni[1], ni[2], ni[3], L[0].rgb, L[1].rgb, L[2].rgb, zs, V[0][1], m.textured, m.semi, m.tpx, m.tpy, m.clutx, m.cluty, uv[0][0], uv[0][1], uv[1][0], uv[1][1], uv[2][0], uv[2][1]); }
                cap.tris.push_back(LvTri{{L[0], L[1], L[2]}, m});
                if (quad) cap.tris.push_back(LvTri{{L[1], L[3], L[2]}, m});
            }
            blk += bsize ? bsize : (8 + cnt * rsize[id]);
        }
        group_end(cap);
    }
    g_ram[(inst + 0xc) & 0x1FFFFF] |= 4;   // "drawn this frame", as the original does
    return 1;
}
