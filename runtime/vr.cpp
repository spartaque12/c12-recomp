// VR: the runtime side of the mod API's VR section (c12_mod_api.h version 4). See vr.h.
//
// A VR mod (mods\vr.dll) runs the headset session. It takes over the frame pacing (set_pacer): while
// the game waits for its next frame, the mod waits for the headset and asks for one picture per
// headset frame (vr_render). The runtime then draws the last world frame once per eye - the native
// scene from the eye's pose, blended between the last two game frames - plus the HUD and a flat
// copy of the window's picture for menus and loading screens.
//
// Eye poses come in the headset's tracking space (OpenXR: x right, y up, -z ahead, metres). The game
// camera of the frame was built from `basis` (the head orientation a first-person mod used for it,
// identity otherwise): an eye is placed relative to the game camera by basis^-1 * eye pose.
#include "psx.h"
#include "gpu.h"
#include "vr.h"
#include "c12_mod_api.h"
#include <math.h>
#include <string.h>

extern float g_ws_scale, g_fov_mult;
GpuDisplay gpu_display();
bool scene3d_enabled();
double plat_time();
extern GpuBackend g_hw_backend;
struct ID3D11Device;
ID3D11Device* hw_device();   // gpu_d3d11.cpp (null without the D3D11 renderer)

void (*g_vr_pacer)(double until);
static C12VrPose g_head;
static bool g_head_ok;

static void q_mul(const float a[4], const float b[4], float o[4]) {   // x, y, z, w
    float r[4] = {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                  a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                  a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                  a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    memcpy(o, r, sizeof r);
}
static void q_conj(const float a[4], float o[4]) { o[0] = -a[0]; o[1] = -a[1]; o[2] = -a[2]; o[3] = a[3]; }
static void q_mat(const float q[4], float m[9]) {   // row-major, v' = m v
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w);     m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w);     m[7] = 2 * (y * z + x * w);     m[8] = 1 - 2 * (x * x + y * y);
}

static bool make_eye(const C12VrView& v, const float basis[4], float upm, int ax, int ay, int aw, int ah, int ox, int oy, const VrCamInfo& ci, VrEyeXf& xf) {
    float ib[4], rel[4], m[9];
    q_conj(basis, ib);
    q_mul(ib, v.eye.q, rel);
    q_mat(rel, m);
    // OpenXR axes (y up, -z ahead) -> the game's camera axes (y down, z ahead): C m C, C = diag(1, -1, -1).
    // That maps eye -> camera; the eye transform is its inverse (transpose).
    float g[9] = {m[0], -m[1], -m[2], -m[3], m[4], m[5], -m[6], m[7], m[8]};
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) xf.R[i * 3 + j] = g[j * 3 + i];
    float pq[4] = {v.eye.p[0], v.eye.p[1], v.eye.p[2], 0}, t[4], pr[4];
    q_mul(ib, pq, t);
    q_mul(t, basis, pr);   // basis^-1 * p * basis
    xf.pos[0] = pr[0] * upm; xf.pos[1] = -pr[1] * upm; xf.pos[2] = -pr[2] * upm;
    float tl = v.tan_left, tr = v.tan_right, tu = v.tan_up, td = v.tan_down;
    if (tr - tl < 0.05f || tu - td < 0.05f) return false;
    xf.hx = aw / (tr - tl);
    xf.cx = ax - tl * xf.hx;
    xf.hy = ah / (tu - td);
    xf.cy = ay + tu * xf.hy;
    float hg = ci.h / (g_fov_mult > 0.0f ? g_fov_mult : 1.0f);
    xf.gx0 = ox + ci.ofx;
    xf.gy0 = oy + ci.ofy;
    xf.ghx = ci.sx * hg * g_ws_scale;
    xf.ghy = ci.sy * hg;
    return xf.ghx > 1.0f && xf.ghy > 1.0f;
}

static int api_vr_render(const C12VrView* views, float upm, int w, int h, C12VrImages* out) {
    memset(out, 0, sizeof *out);
    if (!hw_device() || !views || w <= 0 || h <= 0) return 0;
    g_vr_active = true;
    // the window's picture (menus, loading screens, films)
    GpuDisplay d = gpu_display();
    int cw = 1280, ch = g_cfg.widescreen ? 720 : 960;
    if (!d.blank && hw_xtarget_make(6, cw, ch)) { hw_xtarget_copy_display(6, d); out->cinema = hw_xtarget_texture(6); out->cinema_w = cw; out->cinema_h = ch; }
    int ax, ay, aw, ah, ox, oy;
    VrCamInfo ci;
    if (!scene3d_enabled() || !gpu_vr_have_world() || !gpu_vr_world_area(ax, ay, aw, ah, ox, oy) || !level3d_vr_cam(ci)) return 0;
    const float black[4] = {0, 0, 0, 1}, clear0[4] = {0, 0, 0, 0}, white[4] = {1, 1, 1, 0};
    for (int e = 0; e < 2; e++) {
        VrEyeXf xf;
        if (!make_eye(views[e], ci.basis, upm, ax, ay, aw, ah, ox, oy, ci, xf)) return 0;
        if (!hw_xtarget_make(2 + e, w, h)) return 0;
        hw_xtarget_begin(2 + e, ax, ay, aw, ah, black);
        g_hw_backend.target(2 + e);
        gpu_vr_replay(1, &xf);
        g_hw_backend.target(0);
        out->eye[e] = hw_xtarget_texture(2 + e);
    }
    // the HUD, over black and over white: colour = over black, coverage = 1 - (white - black)
    int hw = aw * 3, hh = ah * 3;
    if (hw_xtarget_make(4, hw, hh) && hw_xtarget_make(5, hw, hh)) {
        hw_xtarget_begin(4, ax, ay, aw, ah, clear0);
        g_hw_backend.target(4);
        gpu_vr_replay(2, nullptr);
        hw_xtarget_begin(5, ax, ay, aw, ah, white);
        g_hw_backend.target(5);
        gpu_vr_replay(2, nullptr);
        g_hw_backend.target(0);
        out->hud_black = hw_xtarget_texture(4);
        out->hud_white = hw_xtarget_texture(5);
        out->hud_w = hw; out->hud_h = hh;
    }
    out->eye_w = w; out->eye_h = h;
    out->scene = 1;
    return 1;
}

static void* api_d3d11_device() { return (void*)hw_device(); }
static void api_set_pacer(void (*fn)(double)) { g_vr_pacer = fn; if (!fn) g_vr_active = false; }
static double api_time_now() { return plat_time(); }
static void api_vr_set_head(const C12VrPose* p) { g_head_ok = p != nullptr; if (p) g_head = *p; }
static int api_vr_get_head(C12VrPose* p) { if (g_head_ok && p) *p = g_head; return g_head_ok ? 1 : 0; }
static void api_vr_set_basis(const float q[4]) { level3d_vr_basis(q); }

// test without a headset (--vr_test 1): with every test screenshot, both eyes (90 degree lens, 64 mm
// apart, looking straight along the game camera), the HUD and the cinema picture as BMPs
int g_vr_test;
bool hw_xtarget_save(int which, const char* path);
void vr_test_shot(const char* dir, int frame) {
    if (!g_vr_test) return;
    g_vr_active = true;
    C12VrView v[2] = {};
    for (int e = 0; e < 2; e++) {
        v[e].eye.q[3] = 1.0f;
        v[e].eye.p[0] = e ? 0.032f : -0.032f;
        v[e].tan_left = -1.0f; v[e].tan_right = 1.0f; v[e].tan_up = 1.0f; v[e].tan_down = -1.0f;
    }
    C12VrImages img;
    int r = api_vr_render(v, 183.0f, 800, 800, &img);
    char path[600];
    const char* name[5] = {"eyeL", "eyeR", "hudB", "hudW", "cinema"};
    for (int i = 0; i < 5; i++) {
        if (i < 4 && !r) continue;
        snprintf(path, sizeof path, "%s/%s%06d.bmp", dir, name[i], frame);
        hw_xtarget_save(2 + i, path);
    }
}

void vr_fill_api(C12ModApi& api) {
    api.d3d11_device = api_d3d11_device;
    api.set_pacer = api_set_pacer;
    api.time_now = api_time_now;
    api.vr_render = api_vr_render;
    api.vr_set_head = api_vr_set_head;
    api.vr_get_head = api_vr_get_head;
    api.vr_set_basis = api_vr_set_basis;
}
