// GPU front-end: GP0/GP1 decoding, DMA, video timing. Rendering is delegated to a GpuBackend.
#include "gpu.h"
#include "vr.h"
#include "ui.h"
#include <algorithm>
#include "savestate.h"

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


static bool g_list_3d;     // the display list being drawn is the world (large ordering table)
static GpuVtx make_vtx(u32 xy, u32 rgb, u32 uv) {
    GpuVtx v;
    v.x = sext11(xy & 0x7FF) + g_off_x;
    v.y = sext11((xy >> 16) & 0x7FF) + g_off_y;
    v.rgb = rgb & 0xFFFFFF;
    v.u = uv & 0xFF;
    v.v = (uv >> 8) & 0xFF;
    v.px = (float)v.x;
    v.py = (float)v.y;
    v.pw = 0;
    extern bool prec_get(u32 word, float& x, float& y, float& dx, float& w);
    float fx, fy, fdx = 0.0f, fw = 0.0f;
    extern int g_prec_hits, g_prec_miss;
    bool got = g_list_3d && prec_get(xy, fx, fy, fdx, fw);
    if (got && !(fabsf(fx - sext11(xy & 0x7FF)) < 8.0f && fabsf(fy - sext11((xy >> 16) & 0x7FF)) < 8.0f)) g_prec_miss++;
    if (got && fabsf(fx - sext11(xy & 0x7FF)) < 8.0f && fabsf(fy - sext11((xy >> 16) & 0x7FF)) < 8.0f) {
        g_prec_hits++;
        v.pw = fw > 0.0f ? fw : 1.0f;   // its depth: perspective-correct texturing (and the layout leaves it alone)
        v.px = fx + fdx + g_off_x;
        v.x = (int)floorf(v.px + 0.5f);
        v.py = fy + g_off_y;
    }
    return v;
}

// ---- widescreen 2D layout ---------------------------------------------------------------------
// With the 16:9 projection the 4:3 frame is stretched, which would fatten everything 2D. HUD/menu
// primitives are grouped into visual elements (a text line, a bar, a panel) and each element is
// squeezed back to its real proportions around a sensible anchor: left edge, right edge or centre.
// Anything spanning the full width (fades, letterbox bars, tiled backgrounds) stays stretched.
int g_hud_fix = 1;
struct Box { int x0, y0, x1, y1; };
struct Tf { float a, k, dy, dx; };
static bool g_collect;                 // pass 1: record boxes, draw nothing
static std::vector<Box> g_boxes;
static std::vector<Tf> g_tf;
static size_t g_tf_i;
static bool g_tf_on;
static int g_clip_l, g_clip_r;
static bool g_collect_3d;              // the list being examined contains real 3D polygons
static int g_last_3d_frame = -100;     // last frame that drew 3D geometry (native scene or world polygons)

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
        g_st.xs_dx = t.dx;
        g_st.xs_clip = g_sub_window;
    }
    return false;
}

// the game's scope flags (800efdfc bit 0x400, or 800f0ca4 == 1), set now or within the last few frames
static int g_scope_flag_frame = -100;
static bool scope_flag_recent() {
    u32 f;
    s16 h;
    memcpy(&f, g_ram + 0xefdfc, 4);
    memcpy(&h, g_ram + 0xf0ca4, 2);
    if ((f & 0x400) || h == 1) g_scope_flag_frame = g_frame;
    return g_frame - g_scope_flag_frame <= 8;
}
static int g_scope_seen = -100;   // last frame that drew the scope's scan-line strips (drawn before the HUD in the same frame, so "<= 1" means "this frame")
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
    if (!g_scope_overlay && g_frame - g_scope_seen <= 1) {
        for (size_t i = 0; i < n; i++) {
            if (find((int)i) != (int)i || g_box3d[i]) continue;
            int y0 = gb[i].y0 - scr_t, y1 = gb[i].y1 - scr_t;
            if (gb[i].x1 - gb[i].x0 >= W - 32) continue;          // full-width pieces stay
            // each piece is snapped to its normal spot (also in the game's exit transition, where the
            // weapon box is drawn part-way between its scope and normal places)
            int x0 = gb[i].x0 - L;
            if (y0 >= 28 && y1 <= 70) gt[i].dy = (float)(24 - y0);                 // health bar, info box: top row
            else if (y0 >= 150 && x0 < 120) { gt[i].dy = (float)(200 - y0); gt[i].dx = (float)(24 - x0); }   // weapon box
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

static bool g_interp_pass;  // drawing the in-between frame (native geometry goes halfway back to last frame)
static bool g_noside;       // suppress uploads/copies/read-backs (list is being replayed)
static bool g_in_list;
static bool g_list_had_3d;  // the list being drawn contained 3D geometry

// View depth of the ordering-table slot being drawn (world list only; 0 = none). The game sorts with
// slot = z >> 3; the front-most slots hold 2D overlays (scope frame), which get none. A little nearer
// than the slot: a flat shadow lying on a native floor must not sink into it.
extern float g_prim_depth;
static float slot_depth() {
    // A menu open in play (inventory, giving an item, save point: 800f1338 = 1, the inventory also sets
    // 800efdfc bit 0x80) shows its item icons as 3D models drawn among the world's primitives, at sort
    // depths behind the walls: no depth test then, as on the console.
    u32 menu, inv;
    memcpy(&menu, g_ram + 0xf1338, 4);
    memcpy(&inv, g_ram + 0xefdfc, 4);
    if (menu == 1 || (inv & 0x80)) return 0.0f;
    int game_slot = g_ot_slots_cur - 1 - g_ot_index;
    return (g_list_3d && g_ot_slots_cur >= 1024 && game_slot >= 8) ? (float)(game_slot * 8) * 0.97f : 0.0f;
}

// ---- VR: what an eye or the HUD pass draws, and flat primitives moved into the eye's view ------------
int g_vr_pass;
const VrEyeXf* g_vr_xf;
bool g_vr_active;
static bool g_vr_list_world;   // the list being replayed is the world list
static bool vr_skip() {
    if (g_vr_pass == 3) return false;
    bool front = g_vr_list_world && g_ot_index >= 0 && g_ot_slots_cur - 1 - g_ot_index < 8;
    if (g_vr_pass == 1) return !g_vr_list_world || front;   // eyes: the world list, without its 2D panels
    return g_vr_list_world && !front;                        // HUD: HUD lists and the world list's 2D panels
}
// game screen position at depth z (the primitive's sort slot) -> the eye's screen; false = behind the eye
static float g_vr_zeye;   // depth in the eye of the last primitive moved (average)
static bool vr_move(float& px, float& py, float z, float* ze = nullptr) {
    const VrEyeXf& e = *g_vr_xf;
    float t[3] = {(px - e.gx0) * z / e.ghx, (py - e.gy0) * z / e.ghy, z};
    float d[3] = {t[0] - e.pos[0], t[1] - e.pos[1], t[2] - e.pos[2]};
    float q[3];
    for (int i = 0; i < 3; i++) q[i] = e.R[i * 3] * d[0] + e.R[i * 3 + 1] * d[1] + e.R[i * 3 + 2] * d[2];
    if (q[2] < 4.0f) return false;
    if (ze) *ze = q[2];
    px = e.cx + q[0] / q[2] * e.hx;
    py = e.cy + q[1] / q[2] * e.hy;
    return true;
}
static bool vr_move_vtx(GpuVtx* v, int n) {
    float z = slot_depth();
    if (z <= 0.0f) return false;
    float sum = 0.0f;
    for (int k = 0; k < n; k++) {
        float ze;
        if (!vr_move(v[k].px, v[k].py, z, &ze)) return false;
        sum += ze;
        v[k].x = (int)floorf(v[k].px + 0.5f);
        v[k].y = (int)floorf(v[k].py + 0.5f);
    }
    g_vr_zeye = sum / n;
    return true;
}
static float prim_depth() { return g_vr_pass == 1 ? g_vr_zeye : slot_depth(); }

bool scene3d_enabled();
static void draw_polygon() {
    u32 cmd = g_fifo[0] >> 24;
    bool gouraud = cmd & 0x10, quad = cmd & 0x08, tex = cmd & 0x04;
    int n = quad ? 4 : 3;
    GpuVtx v[4];
    int i = 0;
    u32 color = g_fifo[0];
    for (int k = 0; k < n; k++) {
        if (gouraud && k > 0) color = g_fifo[i++]; else if (k == 0) i++;
        u32 xy = g_fifo[i];
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
        v[k] = make_vtx(xy, color, uv);
    }
    g_st.textured = tex;
    g_st.raw = tex && (cmd & 1);
    g_st.semi = cmd & 2;
    g_st.gouraud = gouraud;
    g_st.dither = (g_stat & 0x200) && (gouraud || (tex && !g_st.raw));
    if (g_vr_pass && (vr_skip() || (g_vr_pass == 1 && !vr_move_vtx(v, n)))) return;
    // a polygon of the world scene (the front-most slots of the world list may hold a 2D panel)
    bool scene = g_list_3d && g_ot_index < g_ot_slots_cur - 8;
    if (scene && !scene3d_enabled()) { g_last_3d_frame = g_frame; g_list_had_3d = true; g_collect_3d = true; }
    {
        int x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
        for (int k = 1; k < n; k++) { x0 = std::min(x0, v[k].x); x1 = std::max(x1, v[k].x); y0 = std::min(y0, v[k].y); y1 = std::max(y1, v[k].y); }
        bool proj = false;
        for (int k = 0; k < n; k++) if (v[k].pw != 0.0f) proj = true;
        if (prim_2d(x0, y0, x1, y1, proj ? 1 : scene ? 2 : 0)) return;
        if (g_st.ys != 0.0f || g_st.xs_dx != 0.0f)
            for (int k = 0; k < n; k++) { v[k].y += (int)g_st.ys; v[k].py += g_st.ys; v[k].x += (int)g_st.xs_dx; v[k].px += g_st.xs_dx; }
    }
    if (g_dbg_file) {
        fprintf(g_dbg_file, "poly %02x ot %d tp %d,%d semi %d:", cmd, g_ot_index, g_st.tpx, g_st.tpy, (int)g_st.semi);
        for (int k = 0; k < n; k++) fprintf(g_dbg_file, " (%d,%d|uv %d,%d|%06x)", v[k].x, v[k].y, v[k].u, v[k].v, v[k].rgb);
        fputc(10, g_dbg_file);
    }
    // The console silently drops triangles larger than 1023x511. Games rely on that (this one sends
    // over-sized helper polygons with its scope effect).
    auto oversize = [](const GpuVtx* t) {
        int x0 = std::min({t[0].x, t[1].x, t[2].x}), x1 = std::max({t[0].x, t[1].x, t[2].x});
        int y0 = std::min({t[0].y, t[1].y, t[2].y}), y1 = std::max({t[0].y, t[1].y, t[2].y});
        return x1 - x0 > 1023 || y1 - y0 > 511;
    };
    // A primitive of the world list has no depth of its own. On the console the sort order hid it
    // behind nearer geometry; with native geometry drawn first it would show through walls and
    // characters: test it at its slot's depth.
    g_prim_depth = prim_depth();
    if (!oversize(v)) g_be->tri(v, g_st);
    if (quad && !oversize(v + 1)) g_be->tri(v + 1, g_st);
    g_prim_depth = 0.0f;
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
    if (g_vr_pass) {
        if (vr_skip()) return;
        if (g_vr_pass == 1) {   // a sprite: its corners into the eye's view, at its sort depth
            float z = slot_depth(), x0 = (float)x, y0 = (float)y, x1 = (float)(x + w), y1 = (float)(y + h);
            if (z <= 0.0f || !vr_move(x0, y0, z, &g_vr_zeye) || !vr_move(x1, y1, z)) return;
            x = (int)floorf(x0 + 0.5f); y = (int)floorf(y0 + 0.5f);
            w = std::max(1, (int)floorf(x1 + 0.5f) - x); h = std::max(1, (int)floorf(y1 + 0.5f) - y);
        }
    }
    y += (int)g_st.ys;
    x += (int)g_st.xs_dx;
    if (g_dbg_file) { fprintf(g_dbg_file, "rect %02x ot %d: %d,%d %dx%d uv %d,%d tp %d,%d", cmd, g_ot_index, x, y, w, h, uv & 0xFF, (uv >> 8) & 0xFF, g_st.tpx, g_st.tpy); fputc(10, g_dbg_file); }
    if (!g_scope_overlay && g_list_3d) {
        // The scope view's scan lines are 256x1 strips that re-draw every other line from the frame
        // buffer itself; its green tint is one translucent flat rectangle over the whole picture.
        // The black bars above and below are two opaque 512x32 rectangles.
        // The bars come first in the frame, so they mark the frame as a scope frame - also the exit
        // transition frames that still have the letterbox but no scan lines - as long as the game's
        // scope flag is (or was a moment ago) set, which keeps cutscene letterboxes out of it.
        bool scope_now = scope_flag_recent();
        if (tex && h == 1 && w == 256 && g_st.depth == 2) { g_scope_seen = g_frame; return; }
        if (!tex && !(cmd & 2) && w >= 500 && h == 32 && (scope_now || g_frame - g_scope_seen <= 1)) { g_scope_seen = g_frame; return; }
        if (!tex && (cmd & 2) && w >= 500 && h >= 150 && g_frame - g_scope_seen <= 1) return;
    }
    g_prim_depth = prim_depth();
    g_be->rect(x, y, w, h, uv & 0xFF, (uv >> 8) & 0xFF, g_fifo[0] & 0xFFFFFF, g_st);
    g_prim_depth = 0.0f;
}


static int g_dbg_lines, g_dbg_thin;
int g_laser_beam = 1;   // 0 = mods hide the laser sight's beam (lines), keeping its dot
static void draw_line_seg(GpuVtx a, GpuVtx b, bool gouraud, bool semi) {
    g_dbg_lines++;
    if (g_vr_pass) {
        GpuVtx e[2] = {a, b};
        if (vr_skip() || (g_vr_pass == 1 && !vr_move_vtx(e, 2))) return;
        a = e[0]; b = e[1];
    }
    if (prim_2d(std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x) + 1, std::max(a.y, b.y) + 1)) return;
    if (g_dbg_gpu & 1) return;
    // The laser sight's beam: red lines in the 3D scene (its dot is a separate sprite). Mods can hide it.
    if (!g_laser_beam && g_list_3d) {
        u32 col = a.rgb;
        int r = col & 0xFF, gc = (col >> 8) & 0xFF, bc = (col >> 16) & 0xFF;
        if (r > 40 && r > gc * 2 && r > bc * 2) return;
    }
    // The scope view's rounded corners are drawn as lines in the front-most slots of the world list.
    if (!g_scope_overlay && g_list_3d && g_ot_index >= 1015 && g_frame - g_scope_seen <= 1) return;
    g_st.textured = false;
    g_st.raw = false;
    g_st.semi = semi;
    g_st.gouraud = gouraud;
    g_st.dither = (g_stat & 0x200) != 0;
    if (g_st.ys != 0.0f || g_st.xs_dx != 0.0f) {
        GpuVtx a2 = a, b2 = b;
        a2.y += (int)g_st.ys; a2.py += g_st.ys; b2.y += (int)g_st.ys; b2.py += g_st.ys;
        a2.x += (int)g_st.xs_dx; a2.px += g_st.xs_dx; b2.x += (int)g_st.xs_dx; b2.px += g_st.xs_dx;
        g_prim_depth = prim_depth();
        g_be->line(a2, b2, g_st);
        g_prim_depth = 0.0f;
        return;
    }
    g_prim_depth = prim_depth();
    g_be->line(a, b, g_st);
    g_prim_depth = 0.0f;
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
        GpuVtx a = make_vtx(g_fifo[1], g_fifo[0], 0);
        GpuVtx b = gouraud ? make_vtx(g_fifo[3], g_fifo[2], 0) : make_vtx(g_fifo[2], g_fifo[0], 0);
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
        if (fw && fh && !g_collect && g_vr_pass != 2) {
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
        GpuVtx b = make_vtx(v, g_pl_color, 0);
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
void level3d_emit(u32 slot_addr, bool interp, bool blend, int off_x, int off_y, int cx1, int cy1, int cx2, int cy2);
bool level3d_listed(u32 slot_addr);
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
        if (n == 0 && a <= ot_top && ot_top - a < 0x10000 && (a == ot_top || a + 4 == ot_last || a == ot_last)) {
            ot_last = a; ot_slots++; g_ot_index = (int)((ot_top - a) >> 2);
            // the native level geometry goes in right after the farthest slot (the sky dome)
            if (g_ot_index == 1 && draw && level3d_listed(a)) {
                g_last_3d_frame = g_frame; g_list_had_3d = true; g_collect_3d = true;
                if (!g_collect) level3d_emit(a | 0x80000000u, g_noside, g_interp_pass, g_off_x, g_off_y, g_st.cx1, g_st.cy1, g_st.cx2, g_st.cy2);
            }
        }
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

// ---- VR: the display lists of the last frame, kept to be drawn again per eye and for the HUD --------
// A list is stored flat: per node its table slot (or -1), its RAM address and its words.
// seq: order of arrival; native: it has a native scene (snapshot taken); tf / tf_on / sub_window: its
// widescreen 2D layout, applied again when the whole picture is redrawn (pictures at the monitor's rate)
struct RecList { FrontState fs; int slots; bool world; bool native; u32 seq; std::vector<u32> w; std::vector<Tf> tf; bool tf_on, sub_window; };
static u32 g_rec_seq;
static double g_rec_world_time;
static RecList g_rec_world;                       // the newest world list
static std::vector<RecList> g_rec_hud[2];         // [0] being collected, [1] the last frame's (complete)
static bool g_rec_have_world;
static void vr_record(u32 madr, int slots, const FrontState& fs) {
    bool world = false, native = false;
    RecList L;
    L.fs = fs;
    L.slots = slots;
    L.seq = ++g_rec_seq;
    L.tf_on = g_tf_on;
    L.sub_window = g_sub_window;
    if (g_tf_on) L.tf = g_tf;
    u32 a = madr & 0x1FFFFC, ot_top = a, ot_last = a;
    int guard = 0;
    for (;;) {
        u32 hdr;
        memcpy(&hdr, g_ram + a, 4);
        u32 n = hdr >> 24;
        int idx = -1;
        if (n == 0 && a <= ot_top && ot_top - a < 0x10000 && (a == ot_top || a + 4 == ot_last || a == ot_last)) {
            ot_last = a; idx = (int)((ot_top - a) >> 2);
            if (idx == 1 && slots >= 1024 && level3d_listed(a)) {
                native = true;
                level3d_vr_snapshot(a);
                if (level3d_vr_main(a)) world = true;
            }
        }
        if (idx >= 0 || n) {
            L.w.push_back((u32)idx); L.w.push_back(a | 0x80000000u); L.w.push_back(n);
            for (u32 i = 1; i <= n; i++) { u32 w; memcpy(&w, g_ram + ((a + i * 4) & 0x1FFFFC), 4); L.w.push_back(w); }
        }
        if (hdr & 0x800000) break;
        a = hdr & 0x1FFFFC;
        if (++guard > 0x40000) break;
    }
    L.world = world;
    L.native = native;
    if (world) {
        // a new frame: the HUD collected since the last one is complete
        g_rec_hud[1].swap(g_rec_hud[0]);
        g_rec_hud[0].clear();
        g_rec_world = std::move(L);
        g_rec_have_world = true;
        extern double plat_time();
        g_rec_world_time = plat_time();
    } else g_rec_hud[0].push_back(std::move(L));
}
bool gpu_vr_have_world() { return g_rec_have_world; }
// the picture shown now is the newest recorded world frame (a 3D scene, not a film or a menu screen)
bool gpu_desktop_ready(const GpuDisplay& d) {
    extern double plat_time();
    if (!g_rec_have_world || plat_time() - g_rec_world_time > 0.15) return false;
    const GpuState& ws = g_rec_world.fs.st;
    // the game draws the next frame into the hidden buffer (other y) while this one is shown: the newest
    // world frame belongs to the shown picture when it lies in the same columns
    return d.x <= ws.cx1 && ws.cx1 < d.x + d.w && ws.cy2 - ws.cy1 < d.h + 16;
}
bool gpu_vr_world_area(int& x, int& y, int& w, int& h, int& off_x, int& off_y) {
    if (!g_rec_have_world) return false;
    const GpuState& st = g_rec_world.fs.st;
    x = st.cx1; y = st.cy1; w = st.cx2 - st.cx1 + 1; h = st.cy2 - st.cy1 + 1;
    off_x = g_rec_world.fs.offx; off_y = g_rec_world.fs.offy;
    return w > 16 && h > 16;
}
static void vr_replay_list(const RecList& L) {
    if (g_vr_pass == 3) {   // the list's own widescreen layout
        g_tf_on = L.tf_on;
        g_sub_window = L.sub_window;
        if (L.tf_on) g_tf = L.tf;
        g_tf_i = 0;
    }
    g_stat = L.fs.stat; g_st = L.fs.st; g_off_x = L.fs.offx; g_off_y = L.fs.offy;
    g_e1 = L.fs.e[0]; g_e2 = L.fs.e[1]; g_e3 = L.fs.e[2]; g_e4 = L.fs.e[3]; g_e5 = L.fs.e[4]; g_e6 = L.fs.e[5];
    g_fifo_n = 0; g_fifo_need = 0; g_poly_line = false; g_xfer_mode = 0;
    g_ot_slots_cur = L.slots;
    g_list_3d = L.slots >= 1024;
    g_vr_list_world = L.world;
    g_ot_index = -1;
    for (size_t i = 0; i + 2 < L.w.size();) {
        int idx = (int)L.w[i];
        u32 n = L.w[i + 2];
        if (idx >= 0) {
            g_ot_index = idx;
            if (idx == 1 && L.world && g_vr_pass == 1 && g_vr_xf)
                level3d_vr_emit(-1.0f, *g_vr_xf, L.fs.st.cx1, L.fs.st.cy1, L.fs.st.cx2, L.fs.st.cy2);
            if (idx == 1 && L.native && g_vr_pass == 3)   // the whole picture again: its scene blended to now
                level3d_desktop_emit(L.w[i + 1], g_off_x, g_off_y, g_st.cx1, g_st.cy1, g_st.cx2, g_st.cy2);
        }
        for (u32 k = 0; k < n; k++) gpu_write_gp0(L.w[i + 3 + k]);
        i += 3 + n;
    }
    g_ot_index = -1;
}
void gpu_vr_replay(int pass, const VrEyeXf* xf) {
    if (!g_rec_have_world) return;
    FrontState keep = {g_stat, g_st, g_off_x, g_off_y, {g_e1, g_e2, g_e3, g_e4, g_e5, g_e6}, g_fifo_n, g_fifo_need, g_poly_line, g_xfer_mode};
    int keep_slots = g_ot_slots_cur;
    bool keep_3d = g_list_3d, keep_in = g_in_list, keep_tf = g_tf_on;
    g_noside = true;
    g_in_list = true;
    g_tf_on = false;
    g_vr_pass = pass;
    g_vr_xf = xf;
    std::vector<Tf> keep_tf_v;
    bool keep_sub = g_sub_window;
    if (pass == 3) keep_tf_v = g_tf;
    if (pass == 1) vr_replay_list(g_rec_world);
    else if (pass == 3) {
        // the shown picture: the world list, then the lists that drew into the same frame buffer after it
        const GpuState& ws = g_rec_world.fs.st;
        int aw = ws.cx2 - ws.cx1 + 1, ah = ws.cy2 - ws.cy1 + 1;
        auto buf = [&](const RecList& L) { return std::make_pair(aw > 0 ? L.fs.offx / aw : 0, ah > 0 ? L.fs.offy / ah : 0); };
        vr_replay_list(g_rec_world);
        for (const RecList& L : g_rec_hud[0])
            if (L.seq > g_rec_world.seq && buf(L) == buf(g_rec_world)) vr_replay_list(L);
    } else {
        // HUD lists may draw into the other frame buffer of the double buffer: the target is moved onto
        // the buffer each list draws into (same size as the world's)
        const GpuState& ws = g_rec_world.fs.st;
        int aw = ws.cx2 - ws.cx1 + 1, ah = ws.cy2 - ws.cy1 + 1;
        auto buffer_of = [&](const RecList& L) {
            int bx = aw > 0 ? (L.fs.offx / aw) * aw : 0, by = ah > 0 ? (L.fs.offy / ah) * ah : 0;
            hw_xtarget_remap(bx, by, aw, ah);
        };
        for (const RecList& L : g_rec_hud[1]) { buffer_of(L); vr_replay_list(L); }
        buffer_of(g_rec_world);
        vr_replay_list(g_rec_world);
    }
    if (pass == 3) { g_tf = keep_tf_v; g_sub_window = keep_sub; g_tf_i = 0; }
    g_vr_pass = 0;
    g_vr_xf = nullptr;
    g_noside = false;
    g_in_list = keep_in;
    g_tf_on = keep_tf;
    g_ot_slots_cur = keep_slots;
    g_list_3d = keep_3d;
    g_vr_list_world = false;
    g_stat = keep.stat; g_st = keep.st; g_off_x = keep.offx; g_off_y = keep.offy;
    g_e1 = keep.e[0]; g_e2 = keep.e[1]; g_e3 = keep.e[2]; g_e4 = keep.e[3]; g_e5 = keep.e[4]; g_e6 = keep.e[5];
    g_fifo_n = keep.fifo_n; g_fifo_need = keep.fifo_need; g_poly_line = keep.poly_line; g_xfer_mode = keep.xfer_mode;
}

void gpu_dma(u32 madr, u32 bcr, u32 chcr) {
    u32 mode = (chcr >> 9) & 3;
    if (mode == 2) {
        if (g_dbg_frame == g_frame && !g_dbg_file) g_dbg_file = fopen("prims.txt", "w");
        int slots = walk_list(madr, false);
        g_list_3d = slots >= 1024;
        g_ot_slots_cur = slots;
        // 2D layers: the HUD's own small table, or any list while no 3D is on screen (menus, loading)
        bool is2d = g_hud_fix && g_cfg.widescreen && g_ws_scale != 1.0f && slots > 0 &&
                    (slots <= 64 || g_frame - g_last_3d_frame > 6 || slots >= 1024);
        if (is2d) {
            FrontState fs = {g_stat, g_st, g_off_x, g_off_y, {g_e1, g_e2, g_e3, g_e4, g_e5, g_e6}, g_fifo_n, g_fifo_need, g_poly_line, g_xfer_mode};
            g_boxes.clear();
            g_box3d.clear();
            bool in_game = g_frame - g_last_3d_frame <= 6;
            g_collect_3d = false;
            g_collect = true;
            walk_list(madr, true);
            g_collect = false;
            // a big list during gameplay is 2D only when it holds no 3D at all (the game's second,
            // nearly empty list carries HUD text such as the scope's info line)
            if (g_log_level >= 3 && (g_frame % 100) < 2) logf("  collect: %d boxes, 3d=%d in_game=%d clip %d..%d\n", (int)g_boxes.size(), (int)g_collect_3d, (int)in_game, g_clip_l, g_clip_r);
            g_world_layout = slots > 64 && in_game && g_collect_3d;
            layout_2d(g_ws_scale);
            g_stat = fs.stat; g_st = fs.st; g_off_x = fs.offx; g_off_y = fs.offy;
            g_e1 = fs.e[0]; g_e2 = fs.e[1]; g_e3 = fs.e[2]; g_e4 = fs.e[3]; g_e5 = fs.e[4]; g_e6 = fs.e[5];
            g_fifo_n = fs.fifo_n; g_fifo_need = fs.fifo_need; g_poly_line = fs.poly_line; g_xfer_mode = fs.xfer_mode;
            g_tf_i = 0;
            g_tf_on = true;
        }
        // Frame interpolation: replay the list into the in-between layer first; the native scene is
        // drawn there from halfway between the previous and this game frame.
        if (g_cfg.interp && g_be->target) {
            FrontState f2 = {g_stat, g_st, g_off_x, g_off_y, {g_e1, g_e2, g_e3, g_e4, g_e5, g_e6}, g_fifo_n, g_fifo_need, g_poly_line, g_xfer_mode};
            g_be->target(1);
            g_list_had_3d = false;
            g_interp_pass = true;   // camera cuts: the native scene does not blend across them
            g_noside = true;
            g_in_list = true;
            g_tf_i = 0;
            walk_list(madr, true);
            g_interp_pass = false;
            g_noside = false;
            g_be->target(0);
            if (slots >= 1024 && g_list_had_3d) interp_mark(g_off_x, g_off_y);   // frame buffer = where the drawing offset points
            g_stat = f2.stat; g_st = f2.st; g_off_x = f2.offx; g_off_y = f2.offy;
            g_e1 = f2.e[0]; g_e2 = f2.e[1]; g_e3 = f2.e[2]; g_e4 = f2.e[3]; g_e5 = f2.e[4]; g_e6 = f2.e[5];
            g_fifo_n = f2.fifo_n; g_fifo_need = f2.fifo_need; g_poly_line = f2.poly_line; g_xfer_mode = f2.xfer_mode;
            g_tf_i = 0;
        }
        if (g_vr_active) {
            FrontState fr = {g_stat, g_st, g_off_x, g_off_y, {g_e1, g_e2, g_e3, g_e4, g_e5, g_e6}, g_fifo_n, g_fifo_need, g_poly_line, g_xfer_mode};
            vr_record(madr, slots, fr);
        }
        g_in_list = true;
        g_list_had_3d = false;
        walk_list(madr, true);
        g_in_list = false;
        g_tf_on = false;
        g_st.xs_a = 0;
        g_st.xs_k = 1;
        g_st.ys = 0;
        g_st.xs_dx = 0;
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
    { extern int g_prec_puts, g_prec_hits, g_prec_miss; if (g_dbg_frame >= 0 && g_prec_puts) logf("prec frame %d: %d recorded, %d used, %d off\n", g_frame, g_prec_puts, g_prec_hits, g_prec_miss); g_prec_puts = g_prec_hits = g_prec_miss = 0; }
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
            extern void vr_test_shot(const char* dir, int frame);
            vr_test_shot(g_cfg.shot_dir.c_str(), g_frame);
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
    extern bool vr_desktop_owns(const GpuDisplay& d);
    if (!vr_desktop_owns(d)) g_be->present(d);   // else the pictures come at the monitor's rate (vr.cpp)
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

// ---- save states ---------------------------------------------------------------------------
// VRAM is taken from the renderer (it holds what was drawn, not just what was uploaded) together with
// the CPU-side copy of uploads (used to recognise HD textures).
void gpu_state(StateIO& s) {
    s.tag("GPU_");
    s.pod(g_stat); s.pod(g_st); s.pod(g_off_x); s.pod(g_off_y); s.pod(g_tex_disable_allowed);
    s.pod(g_e1); s.pod(g_e2); s.pod(g_e3); s.pod(g_e4); s.pod(g_e5); s.pod(g_e6);
    s.pod(g_disp_x); s.pod(g_disp_y); s.pod(g_h1); s.pod(g_h2); s.pod(g_v1); s.pod(g_v2);
    s.pod(g_gpuread);
    s.pod(g_fifo); s.pod(g_fifo_addr); s.pod(g_fifo_n); s.pod(g_fifo_need); s.pod(g_src_addr);
    s.pod(g_xfer_mode); s.pod(g_xfer_x); s.pod(g_xfer_y); s.pod(g_xfer_w); s.pod(g_xfer_h); s.pod(g_xfer_pos);
    s.vec(g_xfer_buf);
    s.pod(g_poly_line); s.pod(g_pl_prev); s.pod(g_pl_gouraud); s.pod(g_pl_first); s.pod(g_pl_expect_color); s.pod(g_pl_color);
    s.pod(g_line); s.pod(g_field); s.pod(g_line_frac);
    std::vector<u16> vram(1024 * 512);
    if (!s.load) g_be->download(0, 0, 1024, 512, vram.data());
    s.raw(vram.data(), vram.size() * 2);
    std::vector<u16> shadow(g_vram, g_vram + 1024 * 512);
    s.raw(shadow.data(), shadow.size() * 2);
    if (s.load && s.ok) {
        GpuState st = {};
        st.cx2 = 1023;
        st.cy2 = 511;
        g_be->upload(0, 0, 1024, 512, vram.data(), st);
        memcpy(g_vram, shadow.data(), shadow.size() * 2);
        g_scope_flag_frame = g_scope_seen = -100;
        g_last_3d_frame = -100;
    }
}
