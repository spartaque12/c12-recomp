/* C-12 mod: first-person view.
 *
 * The game's own third-person camera keeps running (so movement, lock-on, shooting and every other
 * control work exactly as usual), but the eye is moved from behind the hero to his head. Unlike the
 * game's built-in scope view (R2) there is no scan-line overlay and no scope sound, and the hero can
 * run in any direction. The weapon is drawn as a view model at an adjustable place in front of the eye.
 *
 * What the mod knows about the game (European release, SCES-03364):
 *   80018320(cam)   camera update. cam+0x40 -> view object: camera-to-world rotation (3x3 s16,
 *                   4096 = 1.0, columns = right / down / forward) at +0x0c, eye position (3 x s32)
 *                   at +0x20.
 *   800f1338        1 while the game's own scope view (R2) is active.
 *   80065494        draws the world.
 *   800efdb8        pointer to the hero object. An object carries a world MATRIX at +0x14 (rotation
 *                   3x3 s16, position 3 x s32 at +0x28; the hero's position is at his feet and y
 *                   grows downwards), a parent pointer at +0x50 and a "next attached object" pointer
 *                   at +0x54. The first attached object of the hero is the weapon in his hands.
 *   *(*(obj+0x88))  the object's model instance: draw flags at +0x0c (bit 0 = do not draw; this is
 *                   what the game's scope view sets for the hero) and the matrix that is actually
 *                   drawn at +0x74 (position at +0x88).
 *   80073394        per-frame callback of the weapon object (the object being processed is in the
 *                   global at 800efdc8): attaches the weapon to the hero's hand. The object is drawn
 *                   after its callback, so the mod moves the weapon as soon as the callback returns.
 *
 * Build: mods_src\build_mods.ps1  ->  mods\first_person.dll
 */
#include <stdio.h>
#include <string.h>
#include "c12_mod_api.h"

static const C12ModApi* api;
static int enabled = 1;
static float fov = 1.25f;          /* field-of-view multiplier while the mod is on */
static float eye_up = 300.0f;      /* eye height above the feet, game units (the hero is ~330 tall) */
static float eye_fwd = 0.0f;
static int hide_body = 1;
static int strafe = 1;             /* keep the view direction while moving (camera auto-centre off) */
static int clean_scope = 1;        /* the game's own scope view (R2) without its overlay and sound */
static int viewmodel = 1;          /* draw the weapon at a fixed place in front of the eye */
static float vm_right = 60.0f, vm_down = 70.0f, vm_fwd = 170.0f;
static float vm_yaw = 0.0f, vm_pitch = 0.0f;   /* degrees */

static int was_on;                 /* the mod changed the game's state last frame */
static int saved_autocenter = -1;
static uint32_t g_cam;             /* the camera the game updated this frame */
static int vm_ok;                  /* view basis below is valid for this frame */
static float vm_eye[3], vm_r[3], vm_d[3], vm_f[3];

static int16_t r16(uint32_t a) { int16_t v; memcpy(&v, api->ram + (a & 0x1FFFFF), 2); return v; }
static int32_t r32(uint32_t a) { int32_t v; memcpy(&v, api->ram + (a & 0x1FFFFF), 4); return v; }
static void w16(uint32_t a, int v) { int16_t s = (int16_t)v; memcpy(api->ram + (a & 0x1FFFFF), &s, 2); }
static void w32(uint32_t a, int32_t v) { memcpy(api->ram + (a & 0x1FFFFF), &v, 4); }
static int is_ptr(uint32_t p) { return p >= 0x80010000u && p < 0x80200000u && (p & 3) == 0; }

/* an object's model instance, or 0 */
static uint32_t model_of(uint32_t obj) {
    uint32_t q = (uint32_t)r32(obj + 0x88);
    if (!is_ptr(q)) return 0;
    q = (uint32_t)r32(q);
    return is_ptr(q) ? q : 0;
}

static void set_hidden(uint32_t obj, int hidden) {
    uint32_t m = model_of(obj);
    if (m) w32(m + 0x0c, hidden ? (r32(m + 0x0c) | 1) : (r32(m + 0x0c) & ~1));
}

/* small sine: good to ~0.001, avoids pulling in the math library */
static float fsin(float deg) {
    float x = deg * 0.01745329252f;
    while (x > 3.14159265f) x -= 6.28318531f;
    while (x < -3.14159265f) x += 6.28318531f;
    if (x > 1.57079633f) x = 3.14159265f - x;
    if (x < -1.57079633f) x = -3.14159265f - x;
    float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f)));
}
static float fcos(float deg) { return fsin(deg + 90.0f); }

static void restore(uint32_t hero, int scope) {
    if (!was_on) return;
    if (is_ptr(hero) && !scope) set_hidden(hero, 0);
    api->set_fov(1.0f);
    if (saved_autocenter >= 0) { api->game_option("cam_autocenter", saved_autocenter); saved_autocenter = -1; }
    was_on = 0;
    vm_ok = 0;
}

/* Put the eye at the hero's head, hide his body, set the field of view. Runs after the game's camera
 * update and again right before the world is drawn (the game moves the eye a little in between). */
static void apply(void) {
    uint32_t cam = g_cam;
    if (!is_ptr(cam)) return;
    uint32_t hero = (uint32_t)r32(0x800efdb8);
    uint32_t vo = (uint32_t)r32(cam + 0x40);
    int scope = r32(0x800f1338) == 1;   /* the game's own scope view is active */
    if (!enabled || !is_ptr(hero) || !is_ptr(vo) || scope) { restore(hero, scope); return; }
    if (!was_on) saved_autocenter = -1;
    was_on = 1;
    if (strafe) { int prev = api->game_option("cam_autocenter", 0); if (saved_autocenter < 0) saved_autocenter = prev; }
    else if (saved_autocenter >= 0) { api->game_option("cam_autocenter", saved_autocenter); saved_autocenter = -1; }
    for (int i = 0; i < 3; i++) {
        vm_r[i] = r16(vo + 0x0c + i * 6 + 0) / 4096.0f;
        vm_d[i] = r16(vo + 0x0c + i * 6 + 2) / 4096.0f;
        vm_f[i] = r16(vo + 0x0c + i * 6 + 4) / 4096.0f;
    }
    for (int i = 0; i < 3; i++) vm_eye[i] = (float)r32(hero + 0x28 + i * 4) + vm_f[i] * eye_fwd;
    vm_eye[1] -= eye_up;
    for (int i = 0; i < 3; i++) w32(vo + 0x20 + i * 4, (int)vm_eye[i]);
    set_hidden(hero, hide_body);
    api->set_fov(fov);
    vm_ok = 1;
}

/* Weapon axes = view axes, turned by the adjustable yaw (about "down") and pitch (about "right");
 * position = eye + offset along the view axes. */
static void place_weapon(uint32_t gun) {
    float cy = fcos(vm_yaw), sy = fsin(vm_yaw), cp = fcos(vm_pitch), sp = fsin(vm_pitch);
    float ax[3][3];   /* ax[k] = k-th local axis of the weapon in world space */
    for (int i = 0; i < 3; i++) {
        float r1 = vm_r[i] * cy - vm_f[i] * sy, f1 = vm_r[i] * sy + vm_f[i] * cy;
        ax[0][i] = r1;
        ax[1][i] = vm_d[i] * cp - f1 * sp;
        ax[2][i] = vm_d[i] * sp + f1 * cp;
    }
    uint32_t model = model_of(gun);
    for (int i = 0; i < 3; i++) {
        int t = (int)(vm_eye[i] + vm_r[i] * vm_right + vm_d[i] * vm_down + vm_f[i] * vm_fwd);
        for (int k = 0; k < 3; k++) {
            int v = (int)(ax[k][i] * 4096.0f);
            w16(gun + 0x14 + i * 6 + k * 2, v);
            if (model) w16(model + 0x74 + i * 6 + k * 2, v);
        }
        w32(gun + 0x28 + i * 4, t);
        if (model) w32(model + 0x88 + i * 4, t);
    }
}

static void camera_hook(uint32_t* r, C12OrigFn orig) {
    g_cam = r[4];
    orig(r);
    apply();   /* the weapon's callback, later in the frame, needs this frame's view */
}

static void draw_hook(uint32_t* r, C12OrigFn orig) {
    api->game_option("scope_overlay", enabled && clean_scope ? 0 : 1);
    apply();
    orig(r);
}

static void weapon_hook(uint32_t* r, C12OrigFn orig) {
    uint32_t hero = (uint32_t)r32(0x800efdb8), cur = (uint32_t)r32(0x800efdc8);
    orig(r);
    if (was_on && vm_ok && viewmodel && is_ptr(hero) && is_ptr(cur) && cur == (uint32_t)r32(hero + 0x54) && (uint32_t)r32(cur + 0x50) == hero)
        place_weapon(cur);
}

/* 8005f380(desc, ...) queues a sound effect. Opening the scope view requests two: 800d24cc and
 * 800d24e8 (the switch-on blip and the hum that loops while the view is open). */
static void sfx_hook(uint32_t* r, C12OrigFn orig) {
    if (enabled && clean_scope && (r[4] == 0x800d24ccu || r[4] == 0x800d24e8u)) { r[2] = 0; return; }
    orig(r);
}

__declspec(dllexport) int c12_mod_init(const C12ModApi* a) {
    if (a->version < 2) return 0;
    api = a;
    api->mod_info("First person", "Play from the hero's eyes with the normal controls. No scope overlay, no scope sound.", &enabled);
    api->setting_float("fov", "Field of view (1 = original)", &fov, 0.7f, 2.0f);
    api->setting_float("eye_up", "Eye height", &eye_up, 150.0f, 450.0f);
    api->setting_float("eye_fwd", "Eye forward / back", &eye_fwd, -150.0f, 150.0f);
    api->setting_bool("strafe", "Keep looking where you aim while moving (strafe)", &strafe);
    api->setting_bool("hide_body", "Hide the hero's body", &hide_body);
    api->setting_bool("clean_scope", "Scope view (R2) without scan lines, tint and hum", &clean_scope);
    api->setting_bool("viewmodel", "Weapon view model", &viewmodel);
    api->setting_float("vm_right", "Weapon: right / left", &vm_right, -200.0f, 200.0f);
    api->setting_float("vm_down", "Weapon: down / up", &vm_down, -200.0f, 200.0f);
    api->setting_float("vm_fwd", "Weapon: forward / back", &vm_fwd, 0.0f, 400.0f);
    api->setting_float("vm_yaw", "Weapon: turn (degrees)", &vm_yaw, -180.0f, 180.0f);
    api->setting_float("vm_pitch", "Weapon: tilt (degrees)", &vm_pitch, -90.0f, 90.0f);
    api->hook(0x8005f380, sfx_hook);
    return api->hook(0x80018320, camera_hook) && api->hook(0x80065494, draw_hook) && api->hook(0x80073394, weapon_hook);
}
