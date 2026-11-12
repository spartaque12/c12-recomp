// GPU front-end <-> renderer back-end interface.
#pragma once
#include "psx.h"

struct GpuVtx {
    s32 x, y;          // integer screen coords, drawing offset applied
    u32 rgb;           // 0x00BBGGRR
    s32 u, v;
    float px, py, pw;  // position as drawn (offset and widescreen layout applied); pw unused (0)
    float ox, oy;      // position of the same vertex in the previous game frame (front-end only)
    int has_old;
};

struct GpuState {
    bool textured, raw, semi, gouraud, dither;
    int blend;               // 0 avg, 1 add, 2 sub, 3 add-quarter
    int depth;               // 0 = 4bpp, 1 = 8bpp, 2 = 15bpp
    int tpx, tpy;            // texture page origin in VRAM
    int clutx, cluty;
    u8 tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
    u8 xs_clip = 0;   // the clip window is part of a laid-out 2D element and moves with xs_a/xs_k
    int cx1, cy1, cx2, cy2;  // clip rectangle, inclusive
    bool set_mask, check_mask;
    // 2D layout correction (widescreen HUD): x2 = xs_a + (x - xs_a) * xs_k
    float xs_a = 0.0f, xs_k = 1.0f;
    float xs_dx = 0.0f;   // horizontal nudge of a laid-out 2D element, before xs_a/xs_k (applied like ys)
    float ys = 0.0f;   // vertical shift of a laid-out 2D element (already applied to its vertices; the clip window follows when xs_clip)
};

struct GpuDisplay {
    int x, y, w, h;          // VRAM source rectangle
    bool rgb24, interlace, blank;
    bool interp;             // show the interpolated in-between frame instead of VRAM
    int hres, vres;          // nominal mode size
};

struct GpuBackend {
    void (*tri)(const GpuVtx* v, const GpuState& s);
    void (*rect)(int x, int y, int w, int h, int u, int v, u32 rgb, const GpuState& s);
    void (*line)(const GpuVtx& a, const GpuVtx& b, const GpuState& s);
    void (*fill)(int x, int y, int w, int h, u32 rgb);
    void (*copy)(int sx, int sy, int dx, int dy, int w, int h, const GpuState& s);
    void (*upload)(int x, int y, int w, int h, const u16* data, const GpuState& s);
    void (*download)(int x, int y, int w, int h, u16* out);
    void (*present)(const GpuDisplay& d);
    bool (*screenshot)(const GpuDisplay& d, const char* path);
    void (*target)(int which);   // 0 = VRAM, 1 = in-between frame layer (null if unsupported)
};

extern GpuBackend g_sw_backend;
extern GpuBackend g_hw_backend;
bool hw_backend_init();
extern u16 g_vram[1024 * 512];
bool write_bmp(const char* path, const u32* bgra, int w, int h);

// platform presentation of a CPU-side BGRA image (software backend)
void plat_present_sw(const u32* bgra, int w, int h);
