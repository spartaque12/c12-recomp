// VR support inside the runtime (see runtime/vr.cpp and the VR section of c12_mod_api.h).
//
// A VR mod drives the headset; the runtime draws what the headset shows: the last world frame once
// per eye and the HUD on its own, at the headset's rate, between the game's own frames.
//   * The world display list of every frame is recorded on the host (gpu.cpp) together with a copy of
//     its native 3D scene and of the one before (level3d.cpp), so it can be drawn again later.
//   * Per eye, the native scene is projected from the eye (pose relative to the game camera, the
//     eye's own field of view), blended between the two scenes by the time since the frame arrived.
//     The remaining flat primitives of the world list (sprites, glows, lines) are moved from the
//     game's projection to the eye's at the depth of their sort slot.
//   * The HUD (the small HUD lists and the world list's front-most slots) is drawn into two targets,
//     over black and over white, from which the mod gets colour and coverage.
#pragma once
#include "psx.h"

// Projection of one eye, and of the game, for the world list being drawn per eye.
struct VrEyeXf {
    float R[9], pos[3];          // eye space = R * (true camera space - pos); game units, x right, y down, z ahead
    float hx, hy, cx, cy;        // eye projection into VRAM pixels of the display area: sx = cx + x / z * hx
    float gx0, gy0, ghx, ghy;    // the game's projection of flat primitives: sx = gx0 + x / z * ghx (true camera space)
};

extern int g_vr_pass;            // 0 normal drawing, 1 an eye, 2 the HUD
extern const VrEyeXf* g_vr_xf;   // the eye being drawn (pass 1)
extern bool g_vr_active;         // a VR mod renders frames: record display lists and scenes

// gpu.cpp
bool gpu_vr_have_world();
// Draw the recorded world list into the bound target as eye `xf` (pass 1), or the recorded HUD lists
// (pass 2). Returns the display area (VRAM rectangle) the world list draws into.
bool gpu_vr_world_area(int& x, int& y, int& w, int& h, int& off_x, int& off_y);
void gpu_vr_replay(int pass, const VrEyeXf* xf);

// level3d.cpp
struct VrCamInfo { float h, ofx, ofy, sx, sy; float basis[4]; };   // game projection, stretch, head basis used
bool level3d_vr_snapshot(u32 slot_addr);                 // the world list for this ordering table arrived
bool level3d_vr_cam(VrCamInfo& out);                    // camera of the newest snapshot
void level3d_vr_emit(float t, const VrEyeXf& xf, int clip_x1, int clip_y1, int clip_x2, int clip_y2);
void level3d_vr_basis(const float q[4]);                // head orientation the game camera was built from (this frame)
