// GPU front-end: GP0/GP1 decoding, DMA, video timing. Rendering is delegated to a GpuBackend.
#include "gpu.h"
#include "ui.h"
#include <algorithm>

static GpuBackend* g_be = &g_sw_backend;
void gpu_set_backend(GpuBackend* b) { g_be = b; }

int g_frame;
int g_shot_from;
int g_ramdump_every;   // debug: dump guest RAM every N frames   // headless: first frame to capture
int g_test_classic[4], g_test_menu;   // test hooks: frames at which to press Tab / open the menu
int g_flips;
static u32 g_stat = 0x14802000;
static GpuState g_st;
static s32 g_off_x, g_off_y;
static bool g_tex_disable_allowed;
static u32 g_e1, g_e2, g_e3, g_e4, g_e5, g_e6;
static int g_disp_x, g_disp_y, g_h1 = 0x260, g_h2 = 0xC60, g_v1 = 0x23, g_v2 = 0x123;
static u32 g_gpuread;
static u32 g_fifo[16];
static u32 g_fifo_addr[16];
static int g_fifo_n, g_fifo_need;
static u32 g_src_addr;       // RAM address of the word being fed (DMA linked list), 0 otherwise
// image transfer
static int g_xfer_mode;      // 0 none, 1 cpu->vram, 2 vram->cpu
static int g_xfer_x, g_xfer_y, g_xfer_w, g_xfer_h, g_xfer_pos;
static std::vector<u16> g_xfer_buf;
// polyline
static bool g_poly_line;
static GpuVtx g_pl_prev;
static bool g_pl_gouraud, g_pl_first, g_pl_expect_color;
static u32 g_pl_color;
// timing
static int g_line, g_field;
static s64 g_line_frac;
extern int g_dot_div;
void timers_dotclock_changed();
extern float g_ws_scale;
int g_ws_2d_fix = 1;

int g_dbg_gpu = 0;
int g_dbg_frame = -1;      // dump every primitive of this frame to prims.txt
static FILE* g_dbg_file;
int g_ot_slots;
int g_ot_index = -1;       // ordering-table slot of the primitive being drawn (0 = farthest), -1 unknown

static inline s32 sext11(u32 v) { return (s32)(v << 21) >> 21; }

static void set_texpage(u32 v) {
    g_st.tpx = (v & 0xF) * 64;
    g_st.tpy = ((v >> 4) & 1) * 256;
    g_st.blend = (v >> 5) & 3;
    g_st.depth = (v >> 7) & 3;
    if (g_st.depth == 3) g_st.depth = 2;
    g_stat = (g_stat & ~0x1FFu) | (v & 0x1FF);
}
static void set_clut(u32 v) {
    g_st.clutx = (v & 0x3F) * 16;
    g_st.cluty = (v >> 6) & 0x1FF;
}

bool pgxp_cache_get(u32 xy, PgxpVal* out);
void pgxp_cache_next_frame();

static GpuVtx make_vtx(u32 xy, u32 rgb, u32 uv, u32 addr) {
    GpuVtx v;
    v.x = sext11(xy & 0x7FF) + g_off_x;
    v.y = sext11((xy >> 16) & 0x7FF) + g_off_y;
    v.rgb = rgb & 0xFFFFFF;
    v.u = uv & 0xFF;
    v.v = (uv >> 8) & 0xFF;
    v.px = (float)v.x;
    v.py = (float)v.y;
    v.pw = 0;
    v.ox = v.oy = 0;
    v.has_old = 0;
    if (addr && (g_cfg.pgxp || g_ws_scale != 1.0f)) {
        const PgxpVal& p = g_pgxp_mem[(addr & (RAM_SIZE - 1)) >> 2];
        if (p.val == xy && p.w > 0) {
            // only trust it when it agrees with the integer coordinate (guards against stale/aliased entries)
            float ix = (float)sext11(xy & 0x7FF), iy = (float)sext11((xy >> 16) & 0x7FF);
            if (fabsf(p.x - ix) < 2.0f && fabsf(p.y - iy) < 2.0f) {
                v.px = p.x + g_off_x;
                v.py = p.y + g_off_y;
                v.pw = p.w;
                if (p.has_old) { v.ox = p.ox + g_off_x; v.oy = p.oy + g_off_y; v.has_old = 1; }
            }
        }
    }
    return v;
}

// ---- widescreen 2D layout ---------------------------------------------------------------------
// With the 16:9 projection the 4:3 frame is stretched, which would fatten everything 2D. HUD/menu
// primitives are grouped into visual elements (a text line, a bar, a panel) and each element is
// squeezed back to its real proportions around a sensible anchor: left edge, right edge or centre.
// Anything spanning the full width (fades, letterbox bars, tiled backgrounds) stays stretched.
int g_hud_fix = 1;
float g_near_cull = 0.0f;
int g_near_plane_cull = 1;
struct Box { int x0, y0, x1, y1; };
struct Tf { float a, k, dy; };
static bool g_collect;                 // pass 1: record boxes, draw nothing
static std::vector<Box> g_boxes;
static std::vector<Tf> g_tf;
static size_t g_tf_i;
static bool g_tf_on;
static int g_clip_l, g_clip_r;
static bool g_collect_3d;              // the list being examined contains real 3D polygons
static int g_last_pgxp_frame = -100;   // last frame that drew real 3D geometry

static std::vector<u8> g_box3d;        // per collected primitive: it is projected 3D geometry, leave it alone
static bool g_world_layout;            // the list being laid out is the world list (3D plus maybe a 2D panel)
static int g_clip_t, g_clip_b;
static bool g_sub_window;              // the list being laid out draws into a window narrower than the screen
struct Placed { Box b; Tf t; int frame; };
static std::vector<Placed> g_placed;    // where the elements of the lists drawn so far ended up
static int g_ot_slots_cur;             // ordering-table size of the list being walked
int g_dbg_skip;   // debug: skip classes of primitives (1 small lists, 2 world ot>=990, 4 world ot<50, 8 world ot 900..989, 16 lists drawn through a window)
static bool prim_2d(int x0, int y0, int x1, int y1, int is3d = 0) {
    if (g_dbg_skip && !g_collect) {
        if ((g_dbg_skip & 1) && g_ot_slots_cur <= 64) return true;
        if ((g_dbg_skip & 2) && g_ot_slots_cur >= 1024 && g_ot_index >= 990) return true;
        if ((g_dbg_skip & 4) && g_ot_slots_cur >= 1024 && g_ot_index < 50) return true;
        if ((g_dbg_skip & 8) && g_ot_slots_cur >= 1024 && g_ot_index >= 900 && g_ot_index < 990) return true;
        if ((g_dbg_skip & 16) && g_st.cx2 - g_st.cx1 < 450) return true;
        if ((g_dbg_skip & 32) && g_ot_slots_cur >= 1024 && g_ot_index >= 990 && x1 - x0 <= 2) return true;
        if ((g_dbg_skip & 64) && g_ot_slots_cur >= 1024 && g_ot_index >= 990 && g_ot_index < 1015) return true;
        if ((g_dbg_skip & 128) && g_ot_slots_cur >= 1024 && g_ot_index >= 1015 && x1 - x0 == 256) return true;
        if ((g_dbg_skip & 256) && g_ot_slots_cur >= 1024 && g_ot_index >= 1015 && x1 - x0 >= 500) return true;
    }
    if (g_collect) {
        g_box3d.push_back((u8)is3d);
        if (g_boxes.empty()) { g_clip_l = g_st.cx1; g_clip_r = g_st.cx2 + 1; g_clip_t = g_st.cy1; g_clip_b = g_st.cy2 + 1; }   // the list sets its own drawing area
        g_boxes.push_back({x0, y0, x1, y1});
        return true;
    }
    if (g_tf_on) {
        Tf t = g_tf_i < g_tf.size() ? g_tf[g_tf_i] : Tf{0, 1};
        g_tf_i++;
        g_st.xs_a = t.a;
        g_st.xs_k = t.k;
        g_st.ys = t.dy;
        g_st.xs_clip = g_sub_window;
    }
    return false;
}

static int g_scope_seen = -100;   // last frame that drew the scope's scan-line strips
int g_scope_overlay = 1;          // 0 = mods asked for the scope view without its overlay
static void layout_2d(float k) {
    size_t n = g_boxes.size();
    g_tf.assign(n, Tf{0, 1});
    if (n == 0 || n > 3000) return;
    for (size_t i = 0; i < n; i++) if (g_box3d[i] == 2) g_box3d[i] = g_world_layout ? 1 : 0;   // deep slots: scene only in a real world list
    // a list that clips itself to a small window (the scope's info line) is laid out against the
    // screen area the previous lists used
    static int scr_l = 0, scr_r = 0, scr_t = 0;
    // (a window is anything clearly narrower than the screen area seen so far)
    g_sub_window = scr_r - scr_l >= 200 && (g_clip_r - g_clip_l) * 10 < (scr_r - scr_l) * 9;
    if (!g_sub_window && g_clip_r - g_clip_l >= 200) { scr_l = g_clip_l; scr_r = g_clip_r; scr_t = g_clip_t; }
    const int L = scr_l, R = scr_r, W = R - L;
    if (W < 200) return;
    if (g_sub_window) {
        // A list clipped to a small window is the contents of some box drawn by an earlier list
        // (the scope's info line in its frame, the objectives list in the menu panel): everything
        // in it moves exactly like that box. The backend moves the clip window the same way.
        int cxm = (g_clip_l + g_clip_r) / 2;
        Tf t = {g_clip_r < L + 0.45f * W ? (float)L : g_clip_l > L + 0.55f * W ? (float)R : (L + R) * 0.5f, k};
        long best = -1;
        for (const Placed& p : g_placed) {
            if (g_frame - p.frame > 4 || cxm < p.b.x0 || cxm > p.b.x1 || p.b.y1 <= g_clip_t || p.b.y0 >= g_clip_b) continue;
            long area = (long)(p.b.x1 - p.b.x0) * (p.b.y1 - p.b.y0);
            if (best < 0 || area < best) { best = area; t = p.t; }
        }
        for (size_t i = 0; i < n; i++) if (!g_box3d[i]) g_tf[i] = t;
        return;
    }
    std::vector<int> par(n);
    for (size_t i = 0; i < n; i++) par[i] = (int)i;
    auto find = [&](int i) { while (par[i] != i) i = par[i] = par[par[i]]; return i; };
    auto big = [&](const Box& b) { return b.x1 - b.x0 >= 160; };
    for (size_t i = 0; i < n; i++) {
        const Box& a = g_boxes[i];
        if (g_box3d[i]) continue;
        for (size_t j = i + 1; j < n; j++) {
            const Box& b = g_boxes[j];
            if (g_box3d[j]) continue;
            if (big(a) != big(b)) continue;
            int ox = std::min(a.x1, b.x1) - std::max(a.x0, b.x0), oy = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
            bool join;
            if (big(a)) {  // tiles of one image: touching edges, not stacked on top of each other
                int mw = std::min(a.x1 - a.x0, b.x1 - b.x0), mh = std::min(a.y1 - a.y0, b.y1 - b.y0);
                join = (ox >= -2 && ox <= 2 && oy * 2 > mh) || (oy >= -2 && oy <= 2 && ox * 2 > mw);
            } else join = ox >= -16 && oy >= -4;   // glyphs / parts of one widget
            if (join) par[find((int)i)] = find((int)j);
        }
    }
    std::vector<Box> gb(n, Box{1 << 20, 1 << 20, -(1 << 20), -(1 << 20)});
    for (size_t i = 0; i < n; i++) {
        Box& g = gb[find((int)i)];
        const Box& b = g_boxes[i];
        g.x0 = std::min(g.x0, b.x0); g.y0 = std::min(g.y0, b.y0); g.x1 = std::max(g.x1, b.x1); g.y1 = std::max(g.y1, b.y1);
    }
    std::vector<Tf> gt(n, Tf{0, 1});
    auto place = [&](const Box& g) {
        if (g.x0 <= L + 16 && g.x1 >= R - 16) return Tf{0, 1};
        // edge-anchored only when the whole element lives on that side; everything else is centred
        float a = g.x1 < L + 0.45f * W ? (float)L : g.x0 > L + 0.55f * W ? (float)R : (L + R) * 0.5f;
        // Small elements away from the screen edges are markers the game puts on things in the 3D
        // scene (the lock-on bracket on an enemy): they keep their proportions around their own
        // centre, so they stay on their target. Real HUD sits within ~30 px of an edge and keeps its
        // edge anchor; in between the anchor moves over smoothly, so nothing jumps.
        if (g.x1 - g.x0 <= 120 && g.y1 - g.y0 <= 60) {
            float dl = (float)(g.x0 - L), dr = (float)(R - g.x1);
            float edge = dl < dr ? (float)L : (float)R, dist = dl < dr ? dl : dr;
            float t = (dist - 30.0f) / 50.0f;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float own = (g.x0 + g.x1) * 0.5f;
            if (t > 0) a = edge + (own - edge) * t;
        }
        return Tf{a, k};
    };
    for (size_t i = 0; i < n; i++) if (find((int)i) == (int)i && !g_box3d[i]) gt[i] = place(gb[i]);
    // The world list is only touched while a 2D panel is drawn into it (inventory, map): without one,
    // its 2D pieces are effects tied to the 3D scene and stay as they are.
    if (g_world_layout) {
        bool panel = false;
        for (size_t i = 0; i < n; i++)
            if (find((int)i) == (int)i && !g_box3d[i] && gt[i].k != 1.0f && gb[i].x1 - gb[i].x0 >= W / 2 && gb[i].y1 - gb[i].y0 >= 100) panel = true;
        if (!panel) return;
    }
    // small elements sitting on a panel move with the panel
    for (size_t i = 0; i < n; i++) {
        if (find((int)i) != (int)i || big(g_boxes[i])) continue;
        int cx = (gb[i].x0 + gb[i].x1) / 2, cy = (gb[i].y0 + gb[i].y1) / 2;
        for (size_t j = 0; j < n; j++) {
            if (find((int)j) != (int)j || !big(g_boxes[j]) || gt[j].k == 1.0f) continue;
            if (cx >= gb[j].x0 && cx <= gb[j].x1 && cy >= gb[j].y0 && cy <= gb[j].y1) { gt[i] = gt[j]; break; }
        }
    }
    // A small element lying inside a larger one is part of that picture (a detached room of the map,
    // an icon inside a text block), not a free-standing marker: it moves with what surrounds it.
    auto is_small = [&](const Box& b) { return b.x1 - b.x0 <= 120 && b.y1 - b.y0 <= 60; };
    for (size_t i = 0; i < n; i++) {
        if (find((int)i) != (int)i || !is_small(gb[i])) continue;
        int cx = (gb[i].x0 + gb[i].x1) / 2, cy = (gb[i].y0 + gb[i].y1) / 2;
        long best = -1;
        for (size_t j = 0; j < n; j++) {
            if (j == i || find((int)j) != (int)j || is_small(gb[j]) || gt[j].k == 1.0f) continue;
            if (cx < gb[j].x0 || cx > gb[j].x1 || cy < gb[j].y0 || cy > gb[j].y1) continue;
            long area = (long)(gb[j].x1 - gb[j].x0) * (gb[j].y1 - gb[j].y0);
            if (best < 0 || area < best) { best = area; gt[i] = gt[j]; }
        }
    }
    // The scope view draws the HUD inside its letterbox: health bar 18 lower, weapon box 16 higher, its
    // info box at the top right just under the bar. Without the letterbox (scope_overlay = 0) they go
    // back to their normal places and the info box up to the health bar's height.
    if (!g_scope_overlay && g_frame - g_scope_seen <= 4) {
        for (size_t i = 0; i < n; i++) {
            if (find((int)i) != (int)i || g_box3d[i]) continue;
            int y0 = gb[i].y0 - scr_t, y1 = gb[i].y1 - scr_t;
            if (gb[i].x1 - gb[i].x0 >= W - 32) continue;          // full-width pieces stay
            if (y1 <= 70) gt[i].dy = -18.0f;
            else if (y0 >= 160) gt[i].dy = 16.0f;
        }
    }
    for (size_t i = 0; i < n; i++) g_tf[i] = g_box3d[i] ? Tf{0, 1} : gt[find((int)i)];
    g_placed.erase(std::remove_if(g_placed.begin(), g_placed.end(), [](const Placed& p) { return g_frame - p.frame > 4; }), g_placed.end());
    for (size_t i = 0; i < n; i++)
        if (find((int)i) == (int)i && !g_box3d[i] && gt[i].k != 1.0f && g_placed.size() < 600) g_placed.push_back(Placed{gb[i], gt[i], g_frame});
    if (g_log_level >= 3 && (g_frame % 100) < 2)
        for (size_t i = 0; i < n; i++) if (find((int)i) == (int)i)
            logf("  2D group %d..%d x %d..%d -> anchor %.0f k %.2f (clip %d..%d)\n", gb[i].x0, gb[i].x1, gb[i].y0, gb[i].y1, gt[i].a, gt[i].k, L, R);
}

// recently resolved vertices of screen-space-subdivided polygons (parents for midpoint reconstruction)
struct SubV { int x, y, u, v; float px, py, w, ox, oy; int has_old; };
static SubV g_sub[64];
static int g_sub_n;
static bool g_interp_pass;  // drawing the in-between frame: vertices go halfway back to last frame
static bool g_noside;       // suppress uploads/copies/read-backs (list is being replayed)
static bool g_in_list;
static bool g_list_had_3d;  // the list being replayed contained tracked 3D polygons
static bool g_list_3d;     // the display list being drawn is the world (large ordering table)

static void draw_polygon() {
    u32 cmd = g_fifo[0] >> 24;
    bool gouraud = cmd & 0x10, quad = cmd & 0x08, tex = cmd & 0x04;
    int n = quad ? 4 : 3;
    GpuVtx v[4];
    u32 vxy[4];
    int i = 0;
    u32 color = g_fifo[0];
    for (int k = 0; k < n; k++) {
        if (gouraud && k > 0) color = g_fifo[i++]; else if (k == 0) i++;
        u32 xy = g_fifo[i];
        vxy[k] = xy;
        u32 addr = g_fifo_addr[i];
        i++;
        u32 uv = 0;
        if (tex) {
            uv = g_fifo[i++];
            if (k == 0) set_clut(uv >> 16);
            if (k == 1) {
                u32 tp = uv >> 16;
                set_texpage((g_stat & 0x1FF & ~0x1FFu) | (tp & 0x1FF));
                if (g_tex_disable_allowed) g_stat = (g_stat & ~0x8000u) | ((tp & 0x800) << 4);
            }
        }
        v[k] = make_vtx(xy, color, uv, addr);
    }
    g_st.textured = tex;
    g_st.raw = tex && (cmd & 1);
    g_st.semi = cmd & 2;
    g_st.gouraud = gouraud;
    g_st.dither = (g_stat & 0x200) && (gouraud || (tex && !g_st.raw));
    // PGXP: perspective needs tracked data on every vertex. Otherwise keep whatever precise
    // positions exist (tracked or from the frame's vertex cache) so shared edges still line up,
    // and texture the primitive affinely.
    bool all = true;
    for (int k = 0; k < n; k++) if (v[k].pw <= 0) all = false;
    int pg_kind = all ? 1 : 0;   // 1 tracked, 2 completed from the cache, 3 partial, 0 none
    if (!all && (g_cfg.pgxp || g_ws_scale != 1.0f) && g_list_3d) {   // never for HUD/menu lists: their pixels only coincide with 3D vertices
        int got = 0;
        for (int k = 0; k < n; k++) {
            u32 xy = vxy[k];
            PgxpVal p;
            if (v[k].pw > 0) { got++; continue; }
            if (pgxp_cache_get(xy, &p)) {
                float ix = (float)sext11(xy & 0x7FF), iy = (float)sext11((xy >> 16) & 0x7FF);
                if (fabsf(p.x - ix) < 1.5f && fabsf(p.y - iy) < 1.5f) {
                    v[k].px = p.x + g_off_x; v[k].py = p.y + g_off_y; v[k].pw = p.w; got++;
                    if (p.has_old) { v[k].ox = p.ox + g_off_x; v[k].oy = p.oy + g_off_y; v[k].has_old = 1; }
                }
            }
        }
        // The game splits near polygons in screen space (2D midpoints of edges, averaged UVs),
        // which is affine by construction. Rebuild each midpoint as the projection of the true 3D
        // midpoint of its two parents: position weighted by depth, depth averaged. The pieces then
        // render exactly like the unsplit, perspective-correct polygon.
        // Not for flat-drawn 2D shapes that merely lie in the world list (the map's scroll arrows):
        // untextured and without a single projected vertex, their corners would be "rebuilt" from
        // whatever 3D edge happens to pass under them and be dragged along it.
        if (got < n && (tex || got > 0)) {
            auto remember = [&](int k) {
                for (int a = 0; a < g_sub_n; a++)
                    if (g_sub[a].x == v[k].x && g_sub[a].y == v[k].y && g_sub[a].u == v[k].u && g_sub[a].v == v[k].v) return;
                if (g_sub_n == 64) { memmove(g_sub, g_sub + 16, 48 * sizeof(SubV)); g_sub_n = 48; }
                g_sub[g_sub_n++] = SubV{v[k].x, v[k].y, v[k].u, v[k].v, v[k].px, v[k].py, v[k].pw, v[k].ox, v[k].oy, v[k].has_old};
            };
            for (int k = 0; k < n; k++) if (v[k].pw > 0) remember(k);   // this piece's own corners are parents too
            for (int pass = 0; pass < 2 && got < n; pass++)             // second pass: centre = midpoint of two midpoints
                for (int k = 0; k < n; k++) {
                    if (v[k].pw > 0) continue;
                    for (int a = 0; a < g_sub_n && v[k].pw <= 0; a++)
                        for (int b = a + 1; b < g_sub_n; b++) {
                            const SubV &A = g_sub[a], &B = g_sub[b];
                            if (tex && (abs(((A.u + B.u) >> 1) - v[k].u) > 1 || abs(((A.v + B.v) >> 1) - v[k].v) > 1)) continue;
                            // The piece's new vertex must sit on the parent edge A-B, roughly in the middle
                            // (the game places it with its own, coarser maths). It is replaced by the exact
                            // projection of the 3D midpoint, which lies on the same edge, so the mesh stays closed.
                            float ex = B.px - A.px, ey = B.py - A.py, el = ex * ex + ey * ey;
                            if (el < 4.0f) continue;
                            float t = ((v[k].px - A.px) * ex + (v[k].py - A.py) * ey) / el;
                            float dist = fabsf((v[k].px - A.px) * ey - (v[k].py - A.py) * ex) / sqrtf(el);
                            if (t < 0.25f || t > 0.75f || dist > 2.0f) continue;
                            float ws = A.w + B.w;
                            float mx = (A.px * A.w + B.px * B.w) / ws, my = (A.py * A.w + B.py * B.w) / ws;
                            v[k].px = mx;
                            v[k].py = my;
                            v[k].pw = ws * 0.5f;
                            v[k].has_old = A.has_old && B.has_old;
                            if (v[k].has_old) { v[k].ox = (A.ox * A.w + B.ox * B.w) / ws; v[k].oy = (A.oy * A.w + B.oy * B.w) / ws; }
                            got++;
                            remember(k);
                            break;
                        }
                }
        }
        // every vertex has a depth now: perspective-correct. Otherwise keep the precise positions
        // (shared edges still line up) and texture affinely.
        if (got == n) { all = true; pg_kind = 2; }
        else { pg_kind = got ? 3 : 0; for (int k = 0; k < n; k++) v[k].pw = 0; }
    } else if (!all) for (int k = 0; k < n; k++) { v[k].px = (float)v[k].x; v[k].py = (float)v[k].y; v[k].pw = 0; }
    if (all) { g_last_pgxp_frame = g_frame; g_list_had_3d = true; g_collect_3d = true; }
    // A world polygon whose every vertex lies behind the near plane has no valid projection (the
    // console's divide overflows and the coordinates saturate). The game sends such polygons for one
    // frame when leaving first-person view: the hero's own body, seen from inside his head.
    if (g_near_plane_cull && g_list_3d) {
        int behind = 0;
        for (int k = 0; k < n; k++) { PgxpVal p; if (pgxp_cache_get(vxy[k], &p) && p.pad) behind++; }
        if (behind == n) return;
    }
    // Near cull: geometry closer to the eye than this multiple of the projection distance is
    // dropped (the hero's own body when the camera travels through his head leaving aim mode).
    if (all && g_near_cull > 0.0f) {
        extern float g_gte_h;
        for (int k = 0; k < n; k++) if (v[k].pw < g_gte_h * g_near_cull) return;
    }
    // In-between frame: every vertex that was tracked from the previous game frame goes halfway
    // back. Per vertex, not per polygon: shared vertices then move identically in all polygons that
    // use them, so no cracks open between moved and unmoved neighbours.
    if (g_interp_pass) {
        for (int k = 0; k < n; k++)
            if (v[k].has_old && fabsf(v[k].px - v[k].ox) < 256.0f && fabsf(v[k].py - v[k].oy) < 200.0f) {
                v[k].px = (v[k].px + v[k].ox) * 0.5f;
                v[k].py = (v[k].py + v[k].oy) * 0.5f;
            }
    }
    if (!g_cfg.pgxp) {   // PGXP off: whole-pixel positions and affine texturing, as on the console
        for (int k = 0; k < n; k++) { v[k].px = floorf(v[k].px + 0.5f); v[k].py = floorf(v[k].py + 0.5f); v[k].pw = 0; }
    }
    {
        int x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
        for (int k = 1; k < n; k++) { x0 = std::min(x0, v[k].x); x1 = std::max(x1, v[k].x); y0 = std::min(y0, v[k].y); y1 = std::max(y1, v[k].y); }
        // in the world list only the front-most slots can hold a 2D panel; everything behind is the scene
        if (prim_2d(x0, y0, x1, y1, pg_kind != 0 ? 1 : (g_list_3d && g_ot_index < g_ot_slots_cur - 8) ? 2 : 0)) return;
        if (g_st.ys != 0.0f) for (int k = 0; k < n; k++) { v[k].y += (int)g_st.ys; v[k].py += g_st.ys; }
    }
    if (g_dbg_file) {
        fprintf(g_dbg_file, "poly %02x ot %d pgxp %d tp %d,%d semi %d:", cmd, g_ot_index, pg_kind, g_st.tpx, g_st.tpy, (int)g_st.semi);
        for (int k = 0; k < n; k++) fprintf(g_dbg_file, " (%d,%d|%.2f,%.2f|%.0f|uv %d,%d|%06x)", v[k].x, v[k].y, v[k].px, v[k].py, v[k].pw, v[k].u, v[k].v, v[k].rgb);
        if (pg_kind == 3) {
            fprintf(g_dbg_file, "  W:");
            for (int k = 0; k < n; k++) { PgxpVal p; if (pgxp_cache_get(vxy[k], &p)) fprintf(g_dbg_file, " %.1f", p.w); else fprintf(g_dbg_file, " ?"); }
        }
        fputc(10, g_dbg_file);
    }
    // The console silently drops triangles larger than 1023x511. Games rely on that (this one sends
    // over-sized helper polygons with its scope effect), so only tracked 3D geometry is exempt.
    auto oversize = [](const GpuVtx* t) {
        int x0 = std::min({t[0].x, t[1].x, t[2].x}), x1 = std::max({t[0].x, t[1].x, t[2].x});
        int y0 = std::min({t[0].y, t[1].y, t[2].y}), y1 = std::max({t[0].y, t[1].y, t[2].y});
        return x1 - x0 > 1023 || y1 - y0 > 511;
    };
    bool keep_big = pg_kind == 1 || pg_kind == 2;
    if (keep_big || !oversize(v)) g_be->tri(v, g_st);
    if (quad && (keep_big || !oversize(v + 1))) g_be->tri(v + 1, g_st);
}

static void draw_rect() {
    u32 cmd = g_fifo[0] >> 24;
    bool tex = cmd & 4;
    int size = (cmd >> 3) & 3;
    int i = 1;
    u32 xy = g_fifo[i++];
    u32 uv = 0;
    if (tex) { uv = g_fifo[i++]; set_clut(uv >> 16); }
    int w, h;
    if (size == 0) { w = g_fifo[i] & 0x3FF; h = (g_fifo[i] >> 16) & 0x1FF; }
    else if (size == 1) w = h = 1;
    else if (size == 2) w = h = 8;
    else w = h = 16;
    int x = sext11(xy & 0x7FF) + g_off_x, y = sext11((xy >> 16) & 0x7FF) + g_off_y;
    g_st.textured = tex;
    g_st.raw = tex && (cmd & 1);
    g_st.semi = cmd & 2;
    g_st.gouraud = false;
    g_st.dither = false;
    if (prim_2d(x, y, x + w, y + h)) return;
    y += (int)g_st.ys;
    if (g_dbg_file) { fprintf(g_dbg_file, "rect %02x ot %d: %d,%d %dx%d uv %d,%d tp %d,%d", cmd, g_ot_index, x, y, w, h, uv & 0xFF, (uv >> 8) & 0xFF, g_st.tpx, g_st.tpy); fputc(10, g_dbg_file); }
    if (!g_scope_overlay && g_list_3d) {
        // The scope view's scan lines are 256x1 strips that re-draw every other line from the frame
        // buffer itself; its green tint is one translucent flat rectangle over the whole picture.
        // The black bars above and below are two opaque 512x32 rectangles.
        if (tex && h == 1 && w == 256 && g_st.depth == 2) { g_scope_seen = g_frame; return; }
        if (!tex && (cmd & 2) && w >= 500 && h >= 150 && g_frame - g_scope_seen <= 4) return;
        if (!tex && !(cmd & 2) && w >= 500 && h == 32 && g_frame - g_scope_seen <= 4) return;
    }
    g_be->rect(x, y, w, h, uv & 0xFF, (uv >> 8) & 0xFF, g_fifo[0] & 0xFFFFFF, g_st);
}


static int g_dbg_lines, g_dbg_thin;
static void draw_line_seg(const GpuVtx& a, const GpuVtx& b, bool gouraud, bool semi) {
    g_dbg_lines++;
    if (prim_2d(std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x) + 1, std::max(a.y, b.y) + 1)) return;
    if (g_dbg_gpu & 1) return;
    // The scope view's rounded corners are drawn as lines in the front-most slots of the world list.
    if (!g_scope_overlay && g_list_3d && g_ot_index >= 1015 && g_frame - g_scope_seen <= 4) return;
    g_st.textured = false;
    g_st.raw = false;
    g_st.semi = semi;
    g_st.gouraud = gouraud;
    g_st.dither = (g_stat & 0x200) != 0;
    if (g_st.ys != 0.0f) {
        GpuVtx a2 = a, b2 = b;
        a2.y += (int)g_st.ys; a2.py += g_st.ys; b2.y += (int)g_st.ys; b2.py += g_st.ys;
        g_be->line(a2, b2, g_st);
        return;
    }
    g_be->line(a, b, g_st);
}

static int cmd_len(u32 cmd) {
    if (cmd >= 0x20 && cmd < 0x40) {
        bool gouraud = cmd & 0x10, quad = cmd & 8, tex = cmd & 4;
        int n = quad ? 4 : 3;
        return 1 + n * (1 + (tex ? 1 : 0)) + (gouraud ? n - 1 : 0);
    }
    if (cmd >= 0x40 && cmd < 0x60) return (cmd & 0x10) ? 4 : 3;
    if (cmd >= 0x60 && cmd < 0x80) return 2 + ((cmd & 4) ? 1 : 0) + (((cmd >> 3) & 3) == 0 ? 1 : 0);
    if (cmd >= 0x80 && cmd < 0xA0) return 4;
    if (cmd >= 0xA0 && cmd < 0xE0) return 3;
    if (cmd == 0x02) return 3;
    return 1;
}

static void exec_cmd() {
    u32 w = g_fifo[0], cmd = w >> 24;
    if (cmd >= 0x20 && cmd < 0x40) { draw_polygon(); return; }
    if (cmd >= 0x40 && cmd < 0x60) {
        bool gouraud = cmd & 0x10;
        GpuVtx a = make_vtx(g_fifo[1], g_fifo[0], 0, 0);
        GpuVtx b = gouraud ? make_vtx(g_fifo[3], g_fifo[2], 0, 0) : make_vtx(g_fifo[2], g_fifo[0], 0, 0);
        draw_line_seg(a, b, gouraud, cmd & 2);
        if (cmd & 8) {
            g_poly_line = true;
            g_pl_prev = b;
            g_pl_gouraud = gouraud;
            g_pl_color = g_fifo[0];
            g_pl_expect_color = gouraud;
            g_st.semi = cmd & 2;
        }
        return;
    }
    if (cmd >= 0x60 && cmd < 0x80) { draw_rect(); return; }
    if (cmd >= 0x80 && cmd < 0xA0) {
        int sx = g_fifo[1] & 0x3FF, sy = (g_fifo[1] >> 16) & 0x1FF, dx = g_fifo[2] & 0x3FF, dy = (g_fifo[2] >> 16) & 0x1FF;
        int cw = ((g_fifo[3] & 0xFFFF) - 1 & 0x3FF) + 1, ch = (((g_fifo[3] >> 16) & 0xFFFF) - 1 & 0x1FF) + 1;
        if (g_log_level >= 3 && !g_collect && !g_noside && (g_frame % 100) < 2) logf("vram copy %d,%d -> %d,%d %dx%d (frame %d)\n", sx, sy, dx, dy, cw, ch, g_frame);
        if (!g_collect && !g_noside) g_be->copy(sx, sy, dx, dy, cw, ch, g_st);
        return;
    }
    if (cmd >= 0xA0 && cmd < 0xE0) {
        g_xfer_x = g_fifo[1] & 0x3FF;
        g_xfer_y = (g_fifo[1] >> 16) & 0x1FF;
        g_xfer_w = ((g_fifo[2] & 0xFFFF) - 1 & 0x3FF) + 1;
        g_xfer_h = (((g_fifo[2] >> 16) & 0xFFFF) - 1 & 0x1FF) + 1;
        g_xfer_pos = 0;
        g_xfer_buf.assign((size_t)g_xfer_w * g_xfer_h + 1, 0);
        if (cmd < 0xC0) g_xfer_mode = 1;
        else {
            g_xfer_mode = 2;
            if (!g_collect && !g_noside) g_be->download(g_xfer_x, g_xfer_y, g_xfer_w, g_xfer_h, g_xfer_buf.data());
        }
        return;
    }
    switch (cmd) {
    case 0x00: case 0x01: case 0x03: break;
    case 0x02: {
        int x = g_fifo[1] & 0x3F0, y = (g_fifo[1] >> 16) & 0x1FF;
        int fw = ((g_fifo[2] & 0x3FF) + 15) & ~15, fh = (g_fifo[2] >> 16) & 0x1FF;
        if (fw && fh && !g_collect) {
            u32 col = (g_dbg_gpu & 2) ? 0xFF00FF : (w & 0xFFFFFF);
            if (g_cfg.interp && g_be->target && !g_in_list) { g_be->target(1); g_be->fill(x, y, fw, fh, col); g_be->target(0); }   // keep the in-between layer cleared too
            g_be->fill(x, y, fw, fh, col);
        }
        break;
    }
    case 0x1F: if (!g_collect && !g_noside) { g_stat |= 0x01000000; irq_raise(IRQ_GPU); } break;
    case 0xE1:
        g_e1 = w;
        set_texpage(w & 0x1FF);
        g_stat = (g_stat & ~0x7FFu) | (w & 0x7FF);
        if (g_tex_disable_allowed) g_stat = (g_stat & ~0x8000u) | ((w & 0x800) << 4);
        break;
    case 0xE2:
        g_e2 = w;
        g_st.tw_mask_x = (w & 0x1F) * 8;
        g_st.tw_mask_y = ((w >> 5) & 0x1F) * 8;
        g_st.tw_off_x = ((w >> 10) & 0x1F) * 8;
        g_st.tw_off_y = ((w >> 15) & 0x1F) * 8;
        break;
    case 0xE3: g_e3 = w; g_st.cx1 = w & 0x3FF; g_st.cy1 = (w >> 10) & 0x1FF; break;
    case 0xE4: g_e4 = w; g_st.cx2 = w & 0x3FF; g_st.cy2 = (w >> 10) & 0x1FF; break;
    case 0xE5: g_e5 = w; g_off_x = sext11(w & 0x7FF); g_off_y = sext11((w >> 11) & 0x7FF); break;
    case 0xE6:
        g_e6 = w;
        g_st.set_mask = w & 1;
        g_st.check_mask = w & 2;
        g_stat = (g_stat & ~0x1800u) | ((w & 3) << 11);
        break;
    default: break;
    }
}

void gpu_write_gp0(u32 v) {
    if (g_xfer_mode == 1) {
        size_t total = (size_t)g_xfer_w * g_xfer_h;
        g_xfer_buf[g_xfer_pos++] = (u16)v;
        if ((size_t)g_xfer_pos < total) g_xfer_buf[g_xfer_pos++] = (u16)(v >> 16);
        if ((size_t)g_xfer_pos >= total) {
            g_xfer_mode = 0;
            if (g_log_level >= 3 && g_xfer_x < 512 && (g_frame % 100) < 2) logf("upload into frame buffer: %d,%d %dx%d (frame %d)\n", g_xfer_x, g_xfer_y, g_xfer_w, g_xfer_h, g_frame);
            if (!g_collect && !g_noside) g_be->upload(g_xfer_x, g_xfer_y, g_xfer_w, g_xfer_h, g_xfer_buf.data(), g_st);
        }
        return;
    }
    if (g_poly_line) {
        if ((v & 0xF000F000) == 0x50005000) { g_poly_line = false; return; }
        if (g_pl_expect_color) { g_pl_color = v; g_pl_expect_color = false; return; }
        GpuVtx b = make_vtx(v, g_pl_color, 0, 0);
        bool semi = g_st.semi;
        draw_line_seg(g_pl_prev, b, g_pl_gouraud, semi);
        g_pl_prev = b;
        g_pl_expect_color = g_pl_gouraud;
        return;
    }
    if (g_fifo_n == 0) g_fifo_need = cmd_len(v >> 24);
    g_fifo[g_fifo_n] = v;
    g_fifo_addr[g_fifo_n] = g_src_addr;
    g_fifo_n++;
    if (g_fifo_n >= g_fifo_need) {
        g_fifo_n = 0;
        exec_cmd();
    }
}

u32 gpu_read_data() {
    if (g_xfer_mode == 2) {
        size_t total = (size_t)g_xfer_w * g_xfer_h;
        u32 lo = g_xfer_buf[g_xfer_pos++];
        u32 hi = (size_t)g_xfer_pos < total ? g_xfer_buf[g_xfer_pos++] : 0;
        if ((size_t)g_xfer_pos >= total) g_xfer_mode = 0;
        g_gpuread = lo | (hi << 16);
    }
    return g_gpuread;
}

u32 gpu_read_stat() {
    u32 s = g_stat | 0x1C000000;
    s &= ~0x80000000u;
    bool vbl = g_line < g_v1 || g_line >= g_v2;
    if (g_stat & 0x400000) { if (!vbl && g_field) s |= 0x80000000u; }
    else if (!vbl && (g_line & 1)) s |= 0x80000000u;
    if (g_field) s |= 0x2000; else s &= ~0x2000u;
    if (!(g_stat & 0x400000)) s |= 0x2000;
    // DREQ mirrors the selected DMA direction readiness
    u32 dir = (s >> 29) & 3;
    s &= ~0x02000000u;
    if (dir == 1 || dir == 2 || dir == 3) s |= 0x02000000;
    return s;
}

void gpu_write_gp1(u32 v) {
    u32 cmd = (v >> 24) & 0x3F;
    switch (cmd) {
    case 0x00:
        g_stat = 0x14802000;
        g_fifo_n = 0;
        g_xfer_mode = 0;
        g_poly_line = false;
        g_disp_x = g_disp_y = 0;
        g_h1 = 0x200; g_h2 = 0xC00; g_v1 = 0x10; g_v2 = 0x100;
        g_off_x = g_off_y = 0;
        g_st = GpuState{};
        g_st.cx2 = 1023; g_st.cy2 = 511;
        break;
    case 0x01: g_fifo_n = 0; g_xfer_mode = 0; g_poly_line = false; break;
    case 0x02: g_stat &= ~0x01000000u; break;
    case 0x03: g_stat = (g_stat & ~0x00800000u) | ((v & 1) << 23); break;
    case 0x04: g_stat = (g_stat & ~0x60000000u) | ((v & 3) << 29); break;
    case 0x05: { int nx = v & 0x3FF, ny = (v >> 10) & 0x1FF; if (nx != g_disp_x || ny != g_disp_y) g_flips++; g_disp_x = nx; g_disp_y = ny; break; }
    case 0x06: g_h1 = v & 0xFFF; g_h2 = (v >> 12) & 0xFFF; break;
    case 0x07: g_v1 = v & 0x3FF; g_v2 = (v >> 10) & 0x3FF; break;
    case 0x08: {
        g_stat = (g_stat & ~0x007F4000u) | ((v & 0x3F) << 17) | ((v & 0x40) << 10) | ((v & 0x80) << 7);
        static const int divs[4] = {10, 8, 5, 4};
        g_dot_div = (v & 0x40) ? 7 : divs[v & 3];
        timers_dotclock_changed();
        break;
    }
    case 0x09: g_tex_disable_allowed = v & 1; break;
    default:
        if (cmd >= 0x10 && cmd < 0x20) {
            switch (v & 7) {
            case 2: g_gpuread = g_e2 & 0xFFFFF; break;
            case 3: g_gpuread = g_e3 & 0xFFFFF; break;
            case 4: g_gpuread = g_e4 & 0xFFFFF; break;
            case 5: g_gpuread = g_e5 & 0x3FFFFF; break;
            case 7: g_gpuread = 2; break;
            default: break;
            }
        }
        break;
    }
}

// Walk one display list. Returns the number of ordering-table slots it contained.
static int walk_list(u32 madr, bool draw) {
    u32 a = madr & 0x1FFFFC;
    int guard = 0;
    // The list starts at the far end of the ordering table; empty nodes that walk down in
    // steps of 4 bytes are table slots, everything else is a primitive hanging off a slot.
    u32 ot_top = a, ot_last = a;
    g_ot_index = -1;
    int ot_slots = 0;
    for (;;) {
        u32 hdr;
        memcpy(&hdr, g_ram + a, 4);
        u32 n = hdr >> 24;
        if (n == 0 && a <= ot_top && ot_top - a < 0x10000 && (a == ot_top || a + 4 == ot_last || a == ot_last)) { ot_last = a; ot_slots++; g_ot_index = (int)((ot_top - a) >> 2); }
        if (draw)
            for (u32 i = 1; i <= n; i++) {
                u32 wa = (a + i * 4) & 0x1FFFFC, w;
                memcpy(&w, g_ram + wa, 4);
                g_src_addr = wa | 0x80000000u;
                gpu_write_gp0(w);
            }
        g_src_addr = 0;
        if (hdr & 0x800000) break;
        a = hdr & 0x1FFFFC;
        if (++guard > 0x40000) { if (draw) logf("GPU: runaway display list\n"); break; }
    }
    g_ot_index = -1;
    return ot_slots;
}

// frame buffers (by top-left corner) whose in-between layer holds a complete interpolated frame
static struct { int x, y; bool ok; } g_ireg[4];
static void interp_mark(int x, int y) {
    int slot = 0;
    for (int i = 0; i < 4; i++) if (g_ireg[i].x == x && g_ireg[i].y == y) { slot = i; goto found; }
    for (int i = 0; i < 4; i++) if (!g_ireg[i].ok) { slot = i; break; }
found:
    g_ireg[slot].x = x; g_ireg[slot].y = y; g_ireg[slot].ok = true;
}

struct FrontState {
    u32 stat; GpuState st; s32 offx, offy; u32 e[6]; int fifo_n, fifo_need; bool poly_line; int xfer_mode;
};

void gpu_dma(u32 madr, u32 bcr, u32 chcr) {
    u32 mode = (chcr >> 9) & 3;
    if (mode == 2) {
        if (g_dbg_frame == g_frame && !g_dbg_file) g_dbg_file = fopen("prims.txt", "w");
        int slots = walk_list(madr, false);
        g_list_3d = slots >= 1024;
        g_ot_slots_cur = slots;
        g_sub_n = 0;
        // 2D layers: the HUD's own small table, or any list while no 3D is on screen (menus, loading)
        bool is2d = g_hud_fix && g_cfg.widescreen && g_ws_scale != 1.0f && slots > 0 &&
                    (slots <= 64 || g_frame - g_last_pgxp_frame > 6 || slots >= 1024);
        if (is2d) {
            FrontState fs = {g_stat, g_st, g_off_x, g_off_y, {g_e1, g_e2, g_e3, g_e4, g_e5, g_e6}, g_fifo_n, g_fifo_need, g_poly_line, g_xfer_mode};
            g_boxes.clear();
            g_box3d.clear();
            bool in_game = g_frame - g_last_pgxp_frame <= 6;
            g_collect_3d = false;
            g_collect = true;
            walk_list(madr, true);
            g_collect = false;
            // a big list during gameplay is 2D only when it holds no 3D at all (the game's second,
            // nearly empty list carries HUD text such as the scope's info line)
            if (g_log_level >= 3 && (g_frame % 100) < 2) logf("  collect: %d boxes, 3d=%d in_game=%d clip %d..%d\n", (int)g_boxes.size(), (int)g_collect_3d, (int)in_game, g_clip_l, g_clip_r);
            g_world_layout = slots > 64 && in_game && g_collect_3d;
            g_sub_n = 0;
            layout_2d(g_ws_scale);
            g_stat = fs.stat; g_st = fs.st; g_off_x = fs.offx; g_off_y = fs.offy;
            g_e1 = fs.e[0]; g_e2 = fs.e[1]; g_e3 = fs.e[2]; g_e4 = fs.e[3]; g_e5 = fs.e[4]; g_e6 = fs.e[5];
            g_fifo_n = fs.fifo_n; g_fifo_need = fs.fifo_need; g_poly_line = fs.poly_line; g_xfer_mode = fs.xfer_mode;
            g_tf_i = 0;
            g_tf_on = true;
        }
        // Frame interpolation: replay the list into the in-between layer first, with every tracked
        // polygon moved halfway back to where it was in the previous game frame.
        if (g_cfg.interp && g_be->target) {
            extern int g_mv_matched, g_mv_far;
            bool cut = g_mv_matched > 40 && g_mv_far * 3 > g_mv_matched;   // camera cut: nothing to blend
            if (g_log_level >= 3 && slots >= 1024 && (g_frame % 20) < 2)
                logf("interp frame %d: %d vertices matched to last frame, %d moved far, cut=%d\n", g_frame, g_mv_matched, g_mv_far, (int)cut);
            FrontState f2 = {g_stat, g_st, g_off_x, g_off_y, {g_e1, g_e2, g_e3, g_e4, g_e5, g_e6}, g_fifo_n, g_fifo_need, g_poly_line, g_xfer_mode};
            g_be->target(1);
            g_list_had_3d = false;
            g_interp_pass = !cut;
            g_noside = true;
            g_in_list = true;
            g_tf_i = 0;
            g_sub_n = 0;
            walk_list(madr, true);
            g_interp_pass = false;
            g_noside = false;
            g_be->target(0);
            if (slots >= 1024 && g_list_had_3d) interp_mark(g_off_x, g_off_y);   // frame buffer = where the drawing offset points
            g_stat = f2.stat; g_st = f2.st; g_off_x = f2.offx; g_off_y = f2.offy;
            g_e1 = f2.e[0]; g_e2 = f2.e[1]; g_e3 = f2.e[2]; g_e4 = f2.e[3]; g_e5 = f2.e[4]; g_e6 = f2.e[5];
            g_fifo_n = f2.fifo_n; g_fifo_need = f2.fifo_need; g_poly_line = f2.poly_line; g_xfer_mode = f2.xfer_mode;
            g_tf_i = 0;
            g_sub_n = 0;
        }
        g_in_list = true;
        g_list_had_3d = false;
        walk_list(madr, true);
        g_in_list = false;
        // one "frame" of vertex history per world list (the game also sends an empty second big list)
        if (slots >= 1024 && g_list_had_3d) pgxp_cache_next_frame();
        g_tf_on = false;
        g_st.xs_a = 0;
        g_st.xs_k = 1;
        g_st.ys = 0;
        g_st.xs_clip = 0;
        g_ot_slots = slots;
        if (g_log_level >= 3 && (g_frame % 100) < 2) logf("frame %d list @%06x: %d slots%s\n", g_frame, madr & 0x1FFFFF, slots, is2d ? " (2D)" : "");
        if (g_log_level >= 4) { extern float g_gte_h; logf("frame %d list @%06x: %d slots, H %.0f, off %d,%d clip %d..%d x %d..%d\n", g_frame, madr & 0x1FFFFF, slots, g_gte_h, g_off_x, g_off_y, g_st.cx1, g_st.cx2, g_st.cy1, g_st.cy2); }
        if (g_dbg_file && g_dbg_frame != g_frame) { fclose(g_dbg_file); g_dbg_file = nullptr; }
        return;
    }
    u32 words = (bcr & 0xFFFF) * (mode == 1 ? (bcr >> 16) : 1);
    u32 a = madr & 0x1FFFFC;
    if (chcr & 1) {
        for (u32 i = 0; i < words; i++) {
            u32 w;
            memcpy(&w, g_ram + ((a + i * 4) & 0x1FFFFC), 4);
            gpu_write_gp0(w);
        }
    } else {
        for (u32 i = 0; i < words; i++) {
            u32 w = gpu_read_data();
            memcpy(g_ram + ((a + i * 4) & 0x1FFFFC), &w, 4);
        }
    }
}

// ---- display / timing --------------------------------------------------------
static GpuDisplay cur_display() {
    GpuDisplay d;
    static const int widths[4] = {256, 320, 512, 640};
    d.hres = (g_stat & 0x10000) ? 368 : widths[(g_stat >> 17) & 3];
    bool pal = g_stat & 0x100000;
    d.interlace = (g_stat & 0x400000) && (g_stat & 0x80000);
    int lines = g_v2 - g_v1;
    int maxl = pal ? 288 : 240;
    if (lines <= 0 || lines > maxl) lines = pal ? 256 : 240;
    d.vres = d.interlace ? lines * 2 : lines;
    d.x = g_disp_x;
    d.y = g_disp_y;
    d.w = d.hres;
    d.h = d.vres;
    d.rgb24 = g_stat & 0x200000;
    d.blank = g_stat & 0x800000;
    d.interp = false;
    return d;
}

GpuDisplay gpu_display() { return cur_display(); }

static double g_next_frame_time;
static u32 g_last_shot_frame;
void spu_frame();

void gpu_vblank() {
    g_frame++;
    g_field ^= 1;
    { extern void mods_frame(); mods_frame(); }
    GpuDisplay d = cur_display();
    // The first refresh after the game flips to a new frame shows its in-between version, the next
    // one the frame itself: 25 game frames become 50 evenly spaced pictures per second.
    {
        static int ldx = -1, ldy = -1;
        bool flipped = d.x != ldx || d.y != ldy;
        ldx = d.x; ldy = d.y;
        d.interp = false;
        if (flipped && g_log_level >= 3) logf("flip to %d,%d  regions: %d,%d:%d %d,%d:%d\n", d.x, d.y, g_ireg[0].x, g_ireg[0].y, (int)g_ireg[0].ok, g_ireg[1].x, g_ireg[1].y, (int)g_ireg[1].ok);
        if (flipped && g_cfg.interp)
            for (auto& r : g_ireg)
                if (r.ok && r.x >= d.x && r.x < d.x + d.w && r.y >= d.y && r.y < d.y + d.h) { d.interp = true; r.ok = false; }   // drawn inside the buffer now shown
    }
    spu_frame();
    if ((g_frame % 500) == 0) { static int lf; LOGV("frame %d: %d flips in last 500 vblanks\n", g_frame, g_flips - lf); lf = g_flips; }
    if (g_ramdump_every && (g_frame % g_ramdump_every) == 0 && g_frame >= g_shot_from) {
        char path[64];
        snprintf(path, sizeof path, "vid/ram%06d.bin", g_frame);
        FILE* f = fopen(path, "wb");
        if (f) { fwrite(g_ram, 1, RAM_SIZE, f); fclose(f); }
    }
    if (g_cfg.headless) {
        if (g_frame >= g_shot_from && g_cfg.shot_every && (g_frame % g_cfg.shot_every) == 0 && !g_cfg.shot_dir.empty()) {
            char path[512];
            snprintf(path, sizeof path, "%s/f%06d.bmp", g_cfg.shot_dir.c_str(), g_frame);
            g_be->screenshot(d, path);
        }
        if (g_cfg.max_frames && g_frame >= g_cfg.max_frames) {
            extern u32 g_peek_ptr, g_peek_off;
            if (g_peek_ptr) {
                logf("stack at exit:");
                for (int i = g_cs_n - 1, k = 0; i >= 0 && k < 20; i--, k++) logf(" %08x", g_cs[i & 4095]);
                logf("\n");
                u32 base = mem_r32(&g_ctx, g_peek_ptr) + g_peek_off;
                for (int i = 0; i < 8; i++)
                    logf("peek %08x: %08x %08x %08x %08x\n", base + i * 16, mem_r32(&g_ctx, base + i * 16), mem_r32(&g_ctx, base + i * 16 + 4),
                         mem_r32(&g_ctx, base + i * 16 + 8), mem_r32(&g_ctx, base + i * 16 + 12));
            }
            logf("headless: reached frame %d, exiting\n", g_frame);
            exit(0);
        }
        return;
    }
    g_be->present(d);
    if (g_cfg.max_frames && g_frame >= g_cfg.max_frames) { logf("reached frame %d (audio queue %d ms)\n", g_frame, plat_audio_queued_ms()); exit(0); }
    if (!g_cfg.shot_dir.empty() && g_cfg.shot_every && (g_frame % g_cfg.shot_every) == 0) {
        extern char g_bb_shot[512];
        snprintf(g_bb_shot, sizeof g_bb_shot, "%s/w%06d.bmp", g_cfg.shot_dir.c_str(), g_frame);
    }
    for (int i = 0; i < 4; i++) if (g_test_classic[i] == g_frame) settings_toggle_classic();
    if (g_test_menu == g_frame) ui_toggle_menu();
    plat_pump();
    while (ui_menu_open() && g_cfg.pause_in_menu && !g_quit) {   // frozen frame, live menu
        g_be->present(d);
        plat_pump();
    }
    if (g_quit) exit(0);
}

static void line_event(int, s64 late) {
    bool pal = g_stat & 0x100000;
    int total = pal ? 314 : 263;
    g_line++;
    if (g_line >= total) g_line = 0;
    int vbl = g_v2;
    if (vbl < 200 || vbl >= total) vbl = pal ? 291 : 256;
    if (g_line == vbl) {
        irq_raise(IRQ_VBLANK);
        gpu_vblank();
    }
    // 3406 (PAL) / 3413 (NTSC) video clocks per line, 7/11 sysclk each
    s64 num = (pal ? 3406 : 3413) * 7 + g_line_frac;
    s64 ticks = num / 11;
    g_line_frac = num % 11;
    ticks -= late;
    if (ticks < 1) ticks = 1;
    sched_set(EV_GPU_LINE, ticks, line_event);
}

void gpu_init() {
    gpu_write_gp1(0);
    g_stat |= 0x100000;  // PAL console
    sched_set(EV_GPU_LINE, 2168, line_event);
}
