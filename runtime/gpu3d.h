// Native 3D triangle path for the D3D11 renderer.
//
// Game-specific code can draw view-space triangles straight into the emulated VRAM frame buffer, with
// real perspective (clip space built from the GTE projection, hardware near clipping, perspective-
// correct texturing) and a depth buffer. Texturing and colour go through exactly the same shader as
// the PS1 polygons (texpage/CLUT/texture window/semi-transparency/raw/mask, HD replacements).
//
// Frame usage (see the comment block at n3_begin in gpu_d3d11.cpp for details):
//   n3_begin(view);           // binds the view, clears the depth buffer of the current target
//   n3_tri(...) x N;          // opaque triangles draw at once (depth test + write),
//                             // semi-transparent ones are queued
//   n3_end();                 // queued semi-transparent triangles, back to front (depth test, no write)
//   ... the PS1 display list is drawn afterwards: the primitives of the world list are depth-tested
//   against the native geometry at the depth of their sort slot when g_n3_depth_ps1 is set.
// Call the same sequence again after the front-end has switched to the in-between frame layer
// (GpuBackend::target(1)) to draw the interpolated frame; each target has its own depth buffer.
//
// Only the D3D11 back-end implements this: n3_available() is false otherwise and the calls do nothing.
#pragma once
#include <stdint.h>

// View-space vertex: x right, y down, z forward, in GTE units (the same space the GTE projects:
// screen = OFX + x * H / z). u/v are texel coordinates inside the texture page (0..255, may be
// fractional). rgb is a PS1 colour 0x00BBGGRR; for textured triangles 0x80 = 1.0 (as on the console).
struct N3Vtx {
    float x, y, z;
    float u, v;
    uint32_t rgb;
};

// Material: the same meaning as the PS1 polygon state (GpuState).
struct N3Mat {
    int textured;      // sample the texture page (otherwise flat/gouraud colour only)
    int raw;           // textured without colour modulation
    int semi;          // semi-transparent (textured: only texels with the STP bit, as on the console)
    int blend;         // 0 avg, 1 add, 2 sub, 3 add-quarter
    int depth;         // 0 4bpp, 1 8bpp, 2 15bpp
    int tpx, tpy;      // texture page origin in VRAM (x multiple of 64, y 0 or 256)
    int clutx, cluty;  // CLUT position in VRAM (4/8bpp)
    uint8_t tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;   // texture window (as in GpuState, in texels)
    int dither;        // accepted for completeness; the D3D11 path does not dither (same as PS1 polygons)
    int set_mask;      // force the mask bit on written pixels
};

// Projection and destination.
//   screen_x = off_x + ofx + x * h / z * ws_scale
//   screen_y = off_y + ofy + y * h / z
// off_x/off_y: GPU drawing offset (GP0 E5), ofx/ofy: GTE screen offset (OFX/OFY in pixels),
// h: GTE projection distance. Pixels outside the drawing area (clip_*, inclusive, GP0 E3/E4) are cut.
// znear: geometry closer than this is clipped by the hardware (must be > 0). zfar: geometry further
// is clipped (0 = no far plane).
struct N3View {
    float h;
    float ofx, ofy;
    int off_x, off_y;
    int clip_x1, clip_y1, clip_x2, clip_y2;
    float ws_scale;
    float znear, zfar;
};

bool n3_available();                          // the D3D11 renderer is active
void n3_begin(const N3View& v);
void n3_tri(const N3Vtx v[3], const N3Mat& m);
void n3_end();
void n3_invalidate();                         // forget the depth buffers (PS1 polygons stop depth testing)

// Depth: reversed, infinite-far mapping. NDC depth = N3_DEPTH_K / z (1 at z = N3_DEPTH_K, 0 at infinity),
// depth buffer cleared to 0, tests are GREATER (native opaque, writes) and GREATER_EQUAL (native
// semi-transparent and PS1 world primitives, no write). PS1 primitives use their sort slot's depth.
#define N3_DEPTH_K 1.0f

extern int g_n3_depth_ps1;   // 1 (default): PS1 world primitives are depth-tested against native geometry
extern int g_n3_selftest;    // debug: draw a test quad pair with n3_* before each frame's 3D polygons
void n3_option(const char* key, const char* value);   // "n3_depth_ps1", "n3_selftest"
