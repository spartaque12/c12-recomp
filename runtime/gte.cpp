// GTE (COP2) with PGXP precise-vertex side channel and widescreen projection scaling.
#include "psx.h"
#include <math.h>

#define D c->gd
#define C c->gc
#define VX(n) ((s16)D[(n) * 2])
#define VY(n) ((s16)(D[(n) * 2] >> 16))
#define VZ(n) ((s16)D[(n) * 2 + 1])
#define IR0 D[8]
#define IR1 D[9]
#define IR2 D[10]
#define IR3 D[11]
#define MAC0 D[24]
#define MAC1 D[25]
#define MAC2 D[26]
#define MAC3 D[27]
#define FLAG C[31]
#define RGBC ((u32)D[6])

static inline s16 mtx(Ctx* c, int base, int i) {  // i = 0..8
    u32 w = (u32)C[base + (i >> 1)];
    return (s16)((i & 1) ? (w >> 16) : w);
}

static u8 unr_table[0x101];
static bool unr_init;

static inline s64 chkA(Ctx* c, int n, s64 v) {  // 44-bit accumulate overflow
    if (v > 0x7FFFFFFFFFFll) FLAG |= 1u << (31 - n);
    else if (v < -0x80000000000ll) FLAG |= 1u << (28 - n);
    return (s64)((u64)v << 20) >> 20;
}
static inline s32 lmB(Ctx* c, int n, s32 v, int lm) {
    s32 lo = lm ? 0 : -0x8000;
    if (v > 0x7FFF) { FLAG |= 1u << (25 - n); return 0x7FFF; }
    if (v < lo) { FLAG |= 1u << (25 - n); return lo; }
    return v;
}
static inline s32 lmC(Ctx* c, int n, s32 v) {
    if (v > 255) { FLAG |= 1u << (22 - n); return 255; }
    if (v < 0) { FLAG |= 1u << (22 - n); return 0; }
    return v;
}
static inline s32 lmD(Ctx* c, s32 v) {
    if (v > 0xFFFF) { FLAG |= 1u << 18; return 0xFFFF; }
    if (v < 0) { FLAG |= 1u << 18; return 0; }
    return v;
}
static inline s32 lmG(Ctx* c, int n, s64 v) {
    if (v > 0x3FF) { FLAG |= 1u << (15 - n); return 0x3FF; }
    if (v < -0x400) { FLAG |= 1u << (15 - n); return -0x400; }
    return (s32)v;
}
static inline s64 chkF(Ctx* c, s64 v) {
    if (v > 0x7FFFFFFFll) FLAG |= 1u << 16;
    else if (v < -0x80000000ll) FLAG |= 1u << 15;
    return v;
}
static inline s32 lmH(Ctx* c, s64 v) {
    if (v > 0x1000) { FLAG |= 1u << 12; return 0x1000; }
    if (v < 0) { FLAG |= 1u << 12; return 0; }
    return (s32)v;
}

static u32 gte_divide(Ctx* c, u32 h, u32 sz3) {
    if (h < sz3 * 2) {
        int z = 0;
        for (u32 t = sz3; !(t & 0x8000); t <<= 1) z++;
        u64 n = (u64)h << z;
        u32 d = sz3 << z;
        u32 u = unr_table[(d - 0x7FC0) >> 7] + 0x101;
        d = (0x2000080 - d * u) >> 8;
        d = (0x80 + d * u) >> 8;
        n = (n * d + 0x8000) >> 16;
        return n > 0x1FFFF ? 0x1FFFF : (u32)n;
    }
    FLAG |= 1u << 17;
    return 0x1FFFF;
}

static inline void push_color(Ctx* c) {
    s32 r = lmC(c, 1, MAC1 >> 4), g = lmC(c, 2, MAC2 >> 4), b = lmC(c, 3, MAC3 >> 4);
    D[20] = D[21];
    D[21] = D[22];
    D[22] = (s32)((u32)r | ((u32)g << 8) | ((u32)b << 16) | (RGBC & 0xFF000000u));
}
static inline void set_ir(Ctx* c, int lm) {
    IR1 = lmB(c, 1, MAC1, lm);
    IR2 = lmB(c, 2, MAC2, lm);
    IR3 = lmB(c, 3, MAC3, lm);
}

// MAC = (T<<12 + M*V) >> sh
static void mat_mul(Ctx* c, int mbase, int tbase, s32 v1, s32 v2, s32 v3, int sh, int lm) {
    for (int i = 0; i < 3; i++) {
        s64 t = tbase >= 0 ? ((s64)C[tbase + i] << 12) : 0;
        t = chkA(c, i + 1, t + (s64)mtx(c, mbase, i * 3) * v1);
        t = chkA(c, i + 1, t + (s64)mtx(c, mbase, i * 3 + 1) * v2);
        t = chkA(c, i + 1, t + (s64)mtx(c, mbase, i * 3 + 2) * v3);
        D[25 + i] = (s32)(t >> sh);
    }
    set_ir(c, lm);
}

float g_ws_scale = 1.0f;
int g_ws_flags = 1;  // saturation flags as in the original 4:3 projection
float g_gte_h = 1.0f;   // current projection distance
int g_ws_view = 0;   // experimental: expose squeezed view-space X to the game
int g_pgxp_cull = 1; // winding test from precise coordinates
int g_dbg_near = 0;  // experiment: never report a polygon as nearer than the game's near limit

// PGXP vertex cache: precise result of every projection of the current frame, looked up by the
// packed integer SXY word. Catches vertices that reach a primitive through code paths the
// load/store tracking cannot follow (the game's own clipping / subdivision of near polygons).
#define VC_SIZE 65536
#define VC_PROBE 8
static PgxpVal g_vc[VC_SIZE];
static u32 g_vc_gen[VC_SIZE];
static u32 g_vc_cur = 1;
static inline u32 vc_slot(u32 xy) { return (xy * 2654435761u) >> 16; }

// ---- frame interpolation: previous-frame position of every projected model vertex --------------
// A vertex is identified by the RAM address its model-space coordinates were loaded from plus the
// n-th time that address is projected within the frame (instances of a shared mesh).
#define MV_SIZE 131072
#define MV_PROBE 8
struct MvEnt { u64 key; float x, y; u32 gen; u32 cnt; };
static MvEnt g_mv[2][MV_SIZE];
static int g_mv_cur;                 // table being filled this frame; the other one is the previous frame
static u32 g_mv_gen[2] = {1, 1};
int g_mv_matched, g_mv_far;          // per-frame statistics (camera-cut detection)
int g_mv_prev_matched, g_mv_prev_far;
static inline u32 mv_hash(u64 k) { return (u32)((k * 0x9E3779B97F4A7C15ull) >> 47); }
static MvEnt* mv_find(int t, u64 key, bool create) {
    u32 h = mv_hash(key);
    for (int i = 0; i < MV_PROBE; i++) {
        MvEnt& e = g_mv[t][(h + i) & (MV_SIZE - 1)];
        if (e.gen != g_mv_gen[t]) {
            if (!create) return nullptr;
            e.gen = g_mv_gen[t]; e.key = key; e.cnt = 0;
            return &e;
        }
        if (e.key == key) return &e;
    }
    return nullptr;
}
void pgxp_cache_next_frame() {
    g_vc_cur++;
    g_mv_prev_matched = g_mv_matched;
    g_mv_prev_far = g_mv_far;
    g_mv_matched = g_mv_far = 0;
    g_mv_cur ^= 1;
    g_mv_gen[g_mv_cur]++;            // empties the table that now becomes "current"
}
// record this projection and return last frame's position of the same vertex
static void mv_track(u32 src, PgxpVal& pv) {
    pv.has_old = 0;
    if (!src) return;
    MvEnt* cnt = mv_find(g_mv_cur, (u64)src, true);          // occurrence counter lives under key (src, -)
    if (!cnt) return;
    u64 key = ((u64)(++cnt->cnt) << 32) | src;
    MvEnt* e = mv_find(g_mv_cur, key, true);
    if (e) { e->x = pv.x; e->y = pv.y; }
    MvEnt* o = mv_find(g_mv_cur ^ 1, key, false);
    if (!o) return;
    pv.ox = o->x;
    pv.oy = o->y;
    pv.has_old = 1;
    g_mv_matched++;
    if (fabsf(pv.x - o->x) > 160.0f || fabsf(pv.y - o->y) > 160.0f) g_mv_far++;
}
static void vc_put(const PgxpVal& pv) {
    u32 h = vc_slot(pv.val);
    for (int i = 0; i < VC_PROBE; i++) {
        u32 s = (h + i) & (VC_SIZE - 1);
        if (g_vc_gen[s] != g_vc_cur || g_vc[s].val == pv.val) { g_vc[s] = pv; g_vc_gen[s] = g_vc_cur; return; }
    }
    g_vc[h] = pv;
    g_vc_gen[h] = g_vc_cur;
}
bool pgxp_cache_get(u32 xy, PgxpVal* out) {
    u32 h = vc_slot(xy);
    for (int i = 0; i < VC_PROBE; i++) {
        u32 s = (h + i) & (VC_SIZE - 1);
        if (g_vc_gen[s] != g_vc_cur) return false;
        if (g_vc[s].val == xy) { *out = g_vc[s]; return true; }
    }
    return false;
}  // horizontal projection scale (0.75 for 16:9)

static void rtp(Ctx* c, int v, int sh, int lm, bool last) {
    s32 vx = VX(v), vy = VY(v), vz = VZ(v);
    s64 m[3];
    for (int i = 0; i < 3; i++) {
        s64 t = (s64)C[5 + i] << 12;
        t = chkA(c, i + 1, t + (s64)mtx(c, 0, i * 3) * vx);
        t = chkA(c, i + 1, t + (s64)mtx(c, 0, i * 3 + 1) * vy);
        t = chkA(c, i + 1, t + (s64)mtx(c, 0, i * 3 + 2) * vz);
        m[i] = t;
        D[25 + i] = (s32)(t >> sh);
    }
    IR1 = lmB(c, 1, MAC1, lm);
    IR2 = lmB(c, 2, MAC2, lm);
    // IR3 flag quirk: saturation flag tested against the >>12 value
    {
        s32 z12 = (s32)(m[2] >> 12);
        if (z12 > 0x7FFF || z12 < -0x8000) FLAG |= 1u << 22;
        s32 lo = lm ? 0 : -0x8000;
        IR3 = MAC3 > 0x7FFF ? 0x7FFF : MAC3 < lo ? lo : MAC3;
    }
    D[16] = D[17];
    D[17] = D[18];
    D[18] = D[19];
    D[19] = lmD(c, (s32)(m[2] >> 12));
    u32 h = (u16)C[26];
    s64 q = gte_divide(c, h, (u32)D[19]);
    s32 ir1 = IR1;
    if (g_ws_scale != 1.0f) {
        ir1 = (s32)lrintf(ir1 * g_ws_scale);
        // also report the squeezed view-space X, so game code that culls on it keeps the wider view
        if (g_ws_view) { IR1 = ir1; MAC1 = (s32)lrintf(MAC1 * g_ws_scale); }
    }
    s64 sx = chkF(c, (s64)C[24] + ir1 * q) >> 16;
    s64 sy = chkF(c, (s64)C[25] + IR2 * q) >> 16;
    s32 isx, isy = lmG(c, 2, sy);
    if (ir1 != IR1 && g_ws_flags) {
        // Widescreen: the game uses the X-saturation flag to throw away polygons that reach far
        // off-screen (e.g. the hero's head while the camera sits inside it). Raise the flag exactly
        // when the original 4:3 projection would have, but output the squeezed coordinate.
        lmG(c, 1, ((s64)C[24] + (s64)IR1 * q) >> 16);
        isx = sx > 0x3FF ? 0x3FF : sx < -0x400 ? -0x400 : (s32)sx;
    } else isx = lmG(c, 1, sx);
    D[12] = D[13];
    D[13] = D[14];
    D[14] = (s32)(((u32)isy << 16) | ((u32)isx & 0xFFFF));
    // PGXP
    {
        double fx = (double)m[0] / 4096.0, fy = (double)m[1] / 4096.0, fz = (double)m[2] / 4096.0;
        double zc = fz < h * 0.5 ? h * 0.5 : fz;
        if (zc < 1.0) zc = 1.0;
        double pq = (double)h / zc;
        PgxpVal pv;
        pv.x = (float)(C[24] / 65536.0 + fx * pq * g_ws_scale);
        pv.y = (float)(C[25] / 65536.0 + fy * pq);
        pv.w = (float)zc;
        pv.pad = fz < h * 0.5 ? 1 : 0;   // behind the near plane: the console's divide overflows, the position is garbage
        g_gte_h = (float)h;
        pv.val = (u32)D[14];
        mv_track(c->vsrc[v], pv);
        c->pgte[0] = c->pgte[1];
        c->pgte[1] = c->pgte[2];
        c->pgte[2] = pv;
        vc_put(pv);
    }
    if (last) {
        s64 d = chkF(c, (s64)C[28] + (s64)C[27] * q);
        MAC0 = (s32)d;
        IR0 = lmH(c, d >> 12);
    }
}

// tail shared by depth-cue colour ops: interpolate MAC toward far colour by IR0
static void interp_fc(Ctx* c, s64 in1, s64 in2, s64 in3, int sh, int lm) {
    s64 in[3] = {in1, in2, in3};
    s32 t[3];
    for (int i = 0; i < 3; i++) {
        s64 d = chkA(c, i + 1, ((s64)C[21 + i] << 12) - in[i]);
        t[i] = lmB(c, i + 1, (s32)(d >> sh), 0);
    }
    for (int i = 0; i < 3; i++)
        D[25 + i] = (s32)(chkA(c, i + 1, in[i] + (s64)IR0 * t[i]) >> sh);
    set_ir(c, lm);
}

static void nc(Ctx* c, int v, int sh, int lm, int mode) {  // mode 0 NCS, 1 NCC, 2 NCD
    mat_mul(c, 8, -1, VX(v), VY(v), VZ(v), sh, lm);
    mat_mul(c, 16, 13, IR1, IR2, IR3, sh, lm);
    u32 rgb = RGBC;
    s32 r = (rgb & 0xFF) << 4, g = ((rgb >> 8) & 0xFF) << 4, b = ((rgb >> 16) & 0xFF) << 4;
    if (mode == 1) {
        MAC1 = (r * IR1) >> sh;
        MAC2 = (g * IR2) >> sh;
        MAC3 = (b * IR3) >> sh;
        set_ir(c, lm);
    } else if (mode == 2) {
        interp_fc(c, (s64)r * IR1, (s64)g * IR2, (s64)b * IR3, sh, lm);
    }
    push_color(c);
}

void gte_op(Ctx* c, u32 op) {
    if (!unr_init) {
        for (int i = 0; i < 0x101; i++) {
            int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
            unr_table[i] = (u8)(v < 0 ? 0 : v);
        }
        unr_init = true;
    }
    int sh = (op & 0x80000) ? 12 : 0;
    int lm = (op >> 10) & 1;
    FLAG = 0;
    switch (op & 0x3F) {
    case 0x01: rtp(c, 0, sh, lm, true); break;
    case 0x30: rtp(c, 0, sh, lm, false); rtp(c, 1, sh, lm, false); rtp(c, 2, sh, lm, true); break;
    case 0x06: {
        s32 x0 = (s16)D[12], y0 = (s16)(D[12] >> 16), x1 = (s16)D[13], y1 = (s16)(D[13] >> 16), x2 = (s16)D[14], y2 = (s16)(D[14] >> 16);
        MAC0 = (s32)chkF(c, (s64)x0 * y1 + (s64)x1 * y2 + (s64)x2 * y0 - (s64)x0 * y2 - (s64)x1 * y0 - (s64)x2 * y1);
        // Precise winding test: the integer screen coordinates saturate at +-1024 for vertices far
        // off-screen (near the camera at wide angles), which can flip the sign and make the game
        // drop a visible polygon. Decide from the unsaturated precise coordinates instead.
        if (g_pgxp_cull && c->pgte[0].val == (u32)D[12] && c->pgte[1].val == (u32)D[13] && c->pgte[2].val == (u32)D[14]) {
            const PgxpVal &a = c->pgte[0], &b = c->pgte[1], &d = c->pgte[2];
            double n = (double)a.x * b.y + (double)b.x * d.y + (double)d.x * a.y - (double)a.x * d.y - (double)b.x * a.y - (double)d.x * b.y;
            if (n > 2147483647.0) n = 2147483647.0;
            if (n < -2147483647.0) n = -2147483647.0;
            s32 pn = (s32)llrint(n);
            if (pn == 0 && n != 0.0) pn = n > 0 ? 1 : -1;
            if ((pn > 0) != (MAC0 > 0) || (pn < 0) != (MAC0 < 0)) MAC0 = pn;
        }
        break;
    }
    case 0x0C: {
        s32 d1 = (s16)C[0], d2 = (s16)C[2], d3 = (s16)C[4];
        s32 a = IR1, b = IR2, e = IR3;
        MAC1 = (s32)(chkA(c, 1, (s64)d2 * e - (s64)d3 * b) >> sh);
        MAC2 = (s32)(chkA(c, 2, (s64)d3 * a - (s64)d1 * e) >> sh);
        MAC3 = (s32)(chkA(c, 3, (s64)d1 * b - (s64)d2 * a) >> sh);
        set_ir(c, lm);
        break;
    }
    case 0x10: {
        u32 rgb = RGBC;
        interp_fc(c, (s64)(rgb & 0xFF) << 16, (s64)((rgb >> 8) & 0xFF) << 16, (s64)((rgb >> 16) & 0xFF) << 16, sh, lm);
        push_color(c);
        break;
    }
    case 0x2A:
        for (int i = 0; i < 3; i++) {
            u32 rgb = (u32)D[20];
            interp_fc(c, (s64)(rgb & 0xFF) << 16, (s64)((rgb >> 8) & 0xFF) << 16, (s64)((rgb >> 16) & 0xFF) << 16, sh, lm);
            push_color(c);
        }
        break;
    case 0x11:
        interp_fc(c, (s64)IR1 << 12, (s64)IR2 << 12, (s64)IR3 << 12, sh, lm);
        push_color(c);
        break;
    case 0x12: {
        int mx = (op >> 17) & 3, vv = (op >> 15) & 3, cv = (op >> 13) & 3;
        s32 v1, v2, v3;
        if (vv == 3) { v1 = (s16)IR1; v2 = (s16)IR2; v3 = (s16)IR3; }
        else { v1 = VX(vv); v2 = VY(vv); v3 = VZ(vv); }
        s16 m[9];
        if (mx == 3) {
            s16 r4 = (s16)((RGBC & 0xFF) << 4);
            m[0] = -r4; m[1] = r4; m[2] = (s16)IR0;
            m[3] = m[4] = m[5] = mtx(c, 0, 2);
            m[6] = m[7] = m[8] = mtx(c, 0, 4);
        } else {
            int base = mx * 8;
            for (int i = 0; i < 9; i++) m[i] = mtx(c, base, i);
        }
        static const int tb[4] = {5, 13, 21, -1};
        int t = tb[cv];
        if (cv == 2) {  // far-colour translation bug
            for (int i = 0; i < 3; i++) {
                s64 a = chkA(c, i + 1, ((s64)C[21 + i] << 12) + (s64)m[i * 3] * v1);
                lmB(c, i + 1, (s32)(a >> sh), 0);
                s64 b = chkA(c, i + 1, (s64)m[i * 3 + 1] * v2);
                b = chkA(c, i + 1, b + (s64)m[i * 3 + 2] * v3);
                D[25 + i] = (s32)(b >> sh);
            }
        } else {
            for (int i = 0; i < 3; i++) {
                s64 a = t >= 0 ? ((s64)C[t + i] << 12) : 0;
                a = chkA(c, i + 1, a + (s64)m[i * 3] * v1);
                a = chkA(c, i + 1, a + (s64)m[i * 3 + 1] * v2);
                a = chkA(c, i + 1, a + (s64)m[i * 3 + 2] * v3);
                D[25 + i] = (s32)(a >> sh);
            }
        }
        set_ir(c, lm);
        break;
    }
    case 0x13: nc(c, 0, sh, lm, 2); break;
    case 0x16: nc(c, 0, sh, lm, 2); nc(c, 1, sh, lm, 2); nc(c, 2, sh, lm, 2); break;
    case 0x1B: nc(c, 0, sh, lm, 1); break;
    case 0x3F: nc(c, 0, sh, lm, 1); nc(c, 1, sh, lm, 1); nc(c, 2, sh, lm, 1); break;
    case 0x1E: nc(c, 0, sh, lm, 0); break;
    case 0x20: nc(c, 0, sh, lm, 0); nc(c, 1, sh, lm, 0); nc(c, 2, sh, lm, 0); break;
    case 0x14: {  // CDP
        mat_mul(c, 16, 13, IR1, IR2, IR3, sh, lm);
        u32 rgb = RGBC;
        interp_fc(c, (s64)((rgb & 0xFF) << 4) * IR1, (s64)(((rgb >> 8) & 0xFF) << 4) * IR2, (s64)(((rgb >> 16) & 0xFF) << 4) * IR3, sh, lm);
        push_color(c);
        break;
    }
    case 0x1C: {  // CC
        mat_mul(c, 16, 13, IR1, IR2, IR3, sh, lm);
        u32 rgb = RGBC;
        MAC1 = (s32)(((rgb & 0xFF) << 4) * IR1) >> sh;
        MAC2 = (s32)((((rgb >> 8) & 0xFF) << 4) * IR2) >> sh;
        MAC3 = (s32)((((rgb >> 16) & 0xFF) << 4) * IR3) >> sh;
        set_ir(c, lm);
        push_color(c);
        break;
    }
    case 0x28:
        MAC1 = (IR1 * IR1) >> sh;
        MAC2 = (IR2 * IR2) >> sh;
        MAC3 = (IR3 * IR3) >> sh;
        set_ir(c, lm);
        break;
    case 0x29: {  // DCPL
        u32 rgb = RGBC;
        interp_fc(c, (s64)((rgb & 0xFF) << 4) * IR1, (s64)(((rgb >> 8) & 0xFF) << 4) * IR2, (s64)(((rgb >> 16) & 0xFF) << 4) * IR3, sh, lm);
        push_color(c);
        break;
    }
    case 0x2D: {
        s64 t = chkF(c, (s64)(s16)C[29] * ((u32)(u16)D[17] + (u16)D[18] + (u16)D[19]));
        MAC0 = (s32)t;
        D[7] = lmD(c, (s32)(t >> 12)); if (g_dbg_near && D[7] < 4) D[7] = 4;
        break;
    }
    case 0x2E: {
        s64 t = chkF(c, (s64)(s16)C[30] * ((u32)(u16)D[16] + (u16)D[17] + (u16)D[18] + (u16)D[19]));
        MAC0 = (s32)t;
        D[7] = lmD(c, (s32)(t >> 12)); if (g_dbg_near && D[7] < 4) D[7] = 4;
        break;
    }
    case 0x3D:
        MAC1 = (IR0 * IR1) >> sh;
        MAC2 = (IR0 * IR2) >> sh;
        MAC3 = (IR0 * IR3) >> sh;
        set_ir(c, lm);
        push_color(c);
        break;
    case 0x3E:
        MAC1 = (s32)(chkA(c, 1, ((s64)MAC1 << sh) + (s64)IR0 * IR1) >> sh);
        MAC2 = (s32)(chkA(c, 2, ((s64)MAC2 << sh) + (s64)IR0 * IR2) >> sh);
        MAC3 = (s32)(chkA(c, 3, ((s64)MAC3 << sh) + (s64)IR0 * IR3) >> sh);
        set_ir(c, lm);
        push_color(c);
        break;
    default:
        logf("GTE: unknown op %07x\n", op);
        break;
    }
    if (FLAG & 0x7F87E000) FLAG |= 0x80000000;
}

u32 gte_mfc2(Ctx* c, int reg) {
    switch (reg) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11:
        return (u32)(s32)(s16)D[reg];
    case 7: case 16: case 17: case 18: case 19:
        return (u16)D[reg];
    case 15: return (u32)D[14];
    case 28: case 29: {
        s32 r = IR1 >> 7, g = IR2 >> 7, b = IR3 >> 7;
        r = r < 0 ? 0 : r > 31 ? 31 : r;
        g = g < 0 ? 0 : g > 31 ? 31 : g;
        b = b < 0 ? 0 : b > 31 ? 31 : b;
        return (u32)(r | (g << 5) | (b << 10));
    }
    default: return (u32)D[reg];
    }
}

void gte_mtc2(Ctx* c, int reg, u32 v) {
    switch (reg) {
    case 15:
        D[12] = D[13];
        D[13] = D[14];
        D[14] = (s32)v;
        break;
    case 28:
        D[28] = v & 0x7FFF;
        IR1 = (v & 0x1F) << 7;
        IR2 = ((v >> 5) & 0x1F) << 7;
        IR3 = ((v >> 10) & 0x1F) << 7;
        break;
    case 30: {
        D[30] = (s32)v;
        u32 t = (v & 0x80000000u) ? ~v : v;
        int n = 0;
        while (n < 32 && !(t & 0x80000000u)) { t <<= 1; n++; }
        D[31] = n;
        break;
    }
    case 29: case 31: break;
    default: D[reg] = (s32)v; break;
    }
}

u32 gte_cfc2(Ctx* c, int reg) {
    switch (reg) {
    case 4: case 12: case 20: case 26: case 27: case 29: case 30:
        return (u32)(s32)(s16)C[reg];
    default: return (u32)C[reg];
    }
}

void gte_ctc2(Ctx* c, int reg, u32 v) {
    if (reg == 31) {
        v &= 0x7FFFF000;
        if (v & 0x7F87E000) v |= 0x80000000;
    }
    C[reg] = (s32)v;
}

// Screen positions the game reads back into a CPU register (instead of storing them straight into a
// polygon): that is how it places 2D markers on 3D things - the lock-on bracket, pickup labels. The
// widescreen HUD layout keeps elements sitting on such a point where they are (gpu.cpp layout_2d).
SxyMark g_sxy_marks[128];
int g_sxy_mark_n;

// ---- PGXP register plumbing -------------------------------------------
extern "C" {
void pgxp_mfc2(Ctx* c, int rt, int reg) {
    if (reg >= 12 && reg <= 15) {
        u32 v = c->r[rt];
        SxyMark& m = g_sxy_marks[g_sxy_mark_n++ & 127];
        m.x = (s16)(v & 0xFFFF); m.y = (s16)(v >> 16); m.frame = g_frame;
    }
    if (reg >= 12 && reg <= 15) c->preg[rt] = c->pgte[reg == 15 ? 2 : reg - 12];
    else c->preg[rt].val = ~c->r[rt];
}
void pgxp_mtc2(Ctx* c, int rt, int reg) {
    if (reg == 0 || reg == 2 || reg == 4) c->vsrc[reg >> 1] = c->rsrc_val[rt] == c->r[rt] ? c->rsrc[rt] : 0;
    if (reg >= 12 && reg <= 14) c->pgte[reg - 12] = c->preg[rt];
    else if (reg == 15) { c->pgte[0] = c->pgte[1]; c->pgte[1] = c->pgte[2]; c->pgte[2] = c->preg[rt]; }
}
void pgxp_lwc2(Ctx* c, int reg, u32 a) {
    if (reg == 0 || reg == 2 || reg == 4) c->vsrc[reg >> 1] = a;
    PgxpVal* s = pgxp_slot(a);
    if (!s) return;
    if (reg >= 12 && reg <= 14) c->pgte[reg - 12] = *s;
    else if (reg == 15) { c->pgte[0] = c->pgte[1]; c->pgte[1] = c->pgte[2]; c->pgte[2] = *s; }
}
void pgxp_swc2(Ctx* c, int reg, u32 a) {
    PgxpVal* s = pgxp_slot(a);
    if (!s) return;
    if (reg >= 12 && reg <= 15) *s = c->pgte[reg == 15 ? 2 : reg - 12];
    else s->val = ~(u32)D[reg];
}
}
