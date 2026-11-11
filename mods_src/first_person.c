/* C-12 mod: first-person view.
 *
 * The game's own third-person camera keeps running (so movement, lock-on, shooting and every other
 * control work exactly as usual), but the eye is moved from behind the hero to his head. Unlike the
 * game's built-in scope view (R2) there is no scan-line overlay and no scope sound, and the hero can
 * run in any direction. The weapon is drawn as a view model at an adjustable place in front of the eye.
 *
 * What the mod knows about the game (European release, SCES-03364):
 *   80018320(cam)   camera update (runs for several cameras; the view object's +4 names the one
 *                   that drives the picture - in cutscenes it is not the hero's). cam+0x40 -> view object: camera-to-world rotation (3x3 s16,
 *                   4096 = 1.0, columns = right / down / forward) at +0x0c, eye position (3 x s32)
 *                   at +0x20.
 *   800efdfc        bit 0x400 while the game's own scope view (R2) is open (800f0ca4 = 1 then too;
 *                   800f1338 = 1 only in some places).
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
static int direct_look = 1;        /* mouse turns the view directly, no camera easing */
static float look_yaw, look_pitch;
static int look_valid, saved_ac2 = -1;
static int laser_beam = 0;         /* draw the laser sight's beam (its dot is always drawn) */
static int face_view = 1;          /* the hero faces where you look when standing still */
static int cs_move = 1;           /* move relative to the view at once: backpedal and strafe (CS-style) */
static float scope_walk = 12.0f;    /* walking speed while aiming (game units per frame; running is 41) */
static int scope_move;
static float scope_dx, scope_dz;
static int scope_fov = 1;          /* keep the same field of view in the scope view (R2) */
static int last_view_a = -1;
static int cur_move_a = -1;        /* wanted movement direction this frame, -1 = standing */
static int in_scope, scope_frames, scope_entry_a = -1, scope_last_a = -1;
static float look_sign = 1.0f;     /* +1 if the view's facing angle grows with the look yaw */
static int last_view_p, scope_entry_p, scope_last_p;   /* up/down angles, + = up (4096 per turn) */
static int look_set_pitch = 100000;   /* pending pitch for the direct look, 100000 = none */
static float look_turn;           /* view turn owed to the direct look (in facing units) */

/* atan2 in game angle units (4096 per turn), no math library */
static float atan2_4096(float y, float x) {
    float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    if (ax < 1e-6f && ay < 1e-6f) return 0.0f;
    float t = ax > ay ? ay / ax : ax / ay;                     /* 0..1 */
    float r = t * 0.7853981634f - t * (t - 1.0f) * (0.2447f + 0.0663f * t);
    if (ay > ax) r = 1.5707963268f - r;
    if (x < 0) r = 3.1415926536f - r;
    if (y < 0) r = -r;
    return r * (4096.0f / 6.2831853072f);
}
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
static int scope_open(void) { return (r32(0x800efdfc) & 0x400) || r16(0x800f0ca4) == 1 || r32(0x800f1338) == 1; }
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
static float fsqrt1(float v) { float x = v > 1.0f ? v : 1.0f; for (int i = 0; i < 10; i++) x = 0.5f * (x + v / x); return x; }

/* The hero's facing angle is kept in several copies in the hero object and in his controller
 * (hero+0xb0), and his matrix is a turn about the vertical axis by it. */
static void set_facing(uint32_t hero, int a) {
    static const int hero_offs[] = {0x116, 0x118, 0x11e, 0x120, 0x126, 0x128, 0x12e, 0x130, 0x154};
    for (int i = 0; i < (int)(sizeof hero_offs / sizeof hero_offs[0]); i++) w16(hero + hero_offs[i], a);
    uint32_t ctl = (uint32_t)r32(hero + 0xb0);
    if (is_ptr(ctl)) {
        static const int ctl_offs[] = {0x50, 0x5c, 0x68, 0x74, 0xa8};
        for (int i = 0; i < (int)(sizeof ctl_offs / sizeof ctl_offs[0]); i++) w16(ctl + ctl_offs[i], a);
    }
    float c = fcos(a * (360.0f / 4096.0f)), s = fsin(a * (360.0f / 4096.0f));
    w16(hero + 0x14 + 0, (int)(c * 4096.0f));    /* m00 */
    w16(hero + 0x14 + 4, (int)(s * 4096.0f));    /* m02 */
    w16(hero + 0x14 + 12, (int)(-s * 4096.0f));  /* m20 */
    w16(hero + 0x14 + 16, (int)(c * 4096.0f));   /* m22 */
}

static void restore(uint32_t hero, int scope) {
    if (!was_on) return;
    if (is_ptr(hero) && !scope) set_hidden(hero, 0);
    api->set_fov(1.0f);
    api->game_option("aim_to_view", 0);
    api->game_option("laser_beam", 1);
    api->game_option("pad_forward", 0);
    cur_move_a = -1;
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
    int scope = scope_open();
    /* The view object records the camera that drives it (+4). In cutscenes another camera does:
     * then the mod steps aside and the cutscene plays as made. */
    int cutscene = is_ptr(vo) && (uint32_t)r32(vo + 4) != cam;
    if (enabled && scope && !cutscene) {
        if (!in_scope) {
            /* entering the scope: it looks where the first-person view looked (its yaw at 800f0ca8 uses
             * the hero's facing units) */
            in_scope = 1;
            scope_frames = 0;
            if (last_view_a >= 0) scope_entry_a = last_view_a;
            scope_entry_p = last_view_p;
        }
        if (scope_frames < 3 && scope_entry_a >= 0) {
            /* and up/down: the scope's pitch (800f0ca6, + = up, +-780) */
            int p = scope_entry_p > 780 ? 780 : scope_entry_p < -780 ? -780 : scope_entry_p;
            w16(0x800f0ca8, scope_entry_a);
            w16(0x800f0ca6, p);
            scope_frames++;
        }
        scope_last_a = r16(0x800f0ca8) & 0xfff;
        scope_last_p = r16(0x800f0ca6);
        if (scope_walk > 0.0f) {
            /* walking while aiming: the movement keys no longer turn the aim (the mouse does), and the
             * hero is moved slowly along the scope's own view, relative to where it looks */
            api->game_option("pad_forward", 2);
            int lx = 0, ly = 0;
            uint32_t b = api->pad(&lx, &ly);
            float mx = (float)lx / 127.0f, my = (float)-ly / 127.0f;
            if (b & (1u << 4)) my += 1.0f;
            if (b & (1u << 6)) my -= 1.0f;
            if (b & (1u << 5)) mx += 1.0f;
            if (b & (1u << 7)) mx -= 1.0f;
            float len = mx * mx + my * my;
            scope_move = 0;
            if (len > 0.1f) {
                float a = scope_last_a * (360.0f / 4096.0f);
                float fx = fsin(a), fz = fcos(a);                  /* forward on the ground */
                float dx = fx * my + fz * mx, dz = fz * my - fx * mx;   /* right = forward turned 90 deg */
                float k = 1.0f / fsqrt1(len);
                scope_dx = dx * k; scope_dz = dz * k;
                scope_move = 1;   /* applied in the game's physics step (physics_hook) */
            }
        }
    }
    if (!scope && in_scope) {
        /* leaving the scope: carry on looking where the scope looked */
        in_scope = 0;
        api->game_option("pad_forward", 0);
        scope_move = 0;
        if (scope_entry_a >= 0 && scope_last_a >= 0) {
            int d = ((scope_last_a - scope_entry_a + 2048) & 0xfff) - 2048;
            look_turn += (float)d;
            look_set_pitch = scope_last_p;   /* the view keeps the scope's up/down angle */
        }
        scope_entry_a = scope_last_a = -1;
    }
    if (!enabled || !is_ptr(hero) || !is_ptr(vo) || scope || cutscene) {
        restore(hero, scope);
        if (enabled && scope && scope_fov && !cutscene) api->set_fov(fov);
        return;
    }
    if (!was_on) saved_autocenter = -1;
    was_on = 1;
    if (!direct_look) {   /* without direct look, strafing relies on the runtime's "hold direction" option */
        if (strafe) { int prev = api->game_option("cam_autocenter", 0); if (saved_autocenter < 0) saved_autocenter = prev; }
        else if (saved_autocenter >= 0) { api->game_option("cam_autocenter", saved_autocenter); saved_autocenter = -1; }
    }
    for (int i = 0; i < 3; i++) {
        vm_r[i] = r16(vo + 0x0c + i * 6 + 0) / 4096.0f;
        vm_d[i] = r16(vo + 0x0c + i * 6 + 2) / 4096.0f;
        vm_f[i] = r16(vo + 0x0c + i * 6 + 4) / 4096.0f;
    }
    for (int i = 0; i < 3; i++) vm_eye[i] = (float)r32(hero + 0x28 + i * 4) + vm_f[i] * eye_fwd;
    vm_eye[1] -= eye_up;
    for (int i = 0; i < 3; i++) w32(vo + 0x20 + i * 4, (int)vm_eye[i]);
    set_hidden(hero, hide_body);
    /* view direction as a facing angle (4096 per turn, forward = (sin a, 0, cos a)) */
    int view_a = (int)(atan2_4096(vm_f[0], vm_f[2])) & 0xfff;
    last_view_a = view_a;
    {
        float hz = fsqrt1(vm_f[0] * vm_f[0] + vm_f[2] * vm_f[2]);
        last_view_p = -(int)atan2_4096(vm_f[1], hz);   /* the view's down axis is +y */
    }
    int move_a = -1;
    if (cs_move) {
        /* CS-style movement: the game's controls are relative to the camera and the hero runs where he
         * faces, so he is pointed at the wanted direction (view-relative W/S/A/D or stick) at once, and
         * the game moves him there with its own collision. Shots are traced from the eye anyway. */
        int lx = 0, ly = 0;
        uint32_t b = api->pad(&lx, &ly);
        float mx = (float)lx, my = (float)-ly;
        if (b & (1u << 4)) my += 127.0f;
        if (b & (1u << 6)) my -= 127.0f;
        if (b & (1u << 5)) mx += 127.0f;
        if (b & (1u << 7)) mx -= 127.0f;
        if (mx * mx + my * my > 40.0f * 40.0f) {
            float dx = vm_f[0] * my + vm_r[0] * mx, dz = vm_f[2] * my + vm_r[2] * mx;
            move_a = (int)(atan2_4096(dx, dz)) & 0xfff;
        }
        /* the game runs straight "forward" relative to its camera; for the hero's update (which comes
         * before the next camera update) the camera's yaw is turned to the movement direction - the
         * picture was already built from the real view above */
        cur_move_a = move_a;   /* applied again inside the game's steering (steer_hook) */
    }
    if (face_view || move_a >= 0) {
        /* The hero's facing is kept in several copies in the hero object and in his controller
         * (hero+0xb0), and his matrix is a turn about the vertical axis by it. */
        int a = move_a >= 0 ? move_a : view_a;
        set_facing(hero, a);
    }
    api->set_fov(fov);
    api->game_option("aim_to_view", 1);   /* shots go to the centre of the screen */
    api->game_option("laser_beam", laser_beam);
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

static int sext12(int v) { v &= 0xfff; return v >= 0x800 ? v - 0x1000 : v; }

/* Direct look: the mod keeps the view angles itself (cam+0x20 pitch, cam+0x22 yaw, 4096 per turn,
 * pitch 0 = level), adds the raw mouse movement to them and rebuilds the view rotation from them with
 * the game's own angle-to-matrix routine (800a1db8). The game's camera normally eases towards a
 * target direction; that easing is what made mouse look feel slow. */
static void camera_hook(uint32_t* r, C12OrigFn orig) {
    uint32_t cam = r[4];
    uint32_t hero = (uint32_t)r32(0x800efdb8), vo = (uint32_t)r32(cam + 0x40);
    /* the game updates more than one camera per frame; only the one that follows the hero is ours */
    if (!is_ptr(hero) || !is_ptr(vo) || (uint32_t)r32(cam + 0x44) != hero + 0x14) { orig(r); return; }
    g_cam = cam;
    int active = enabled && direct_look && !scope_open() && (uint32_t)r32(vo + 4) == cam;
    if (active) {
        if (!look_valid) {
            look_yaw = (float)(r16(cam + 0x22) & 0xfff);
            look_pitch = (float)sext12(r16(cam + 0x20));
            saved_ac2 = api->game_option("cam_autocenter", 1);   /* the runtime's own "hold direction" would fight this */
            look_valid = 1;
        }
        float dy = 0, dp = 0;
        api->mouse_look(&dy, &dp);
        look_yaw += dy + look_turn * look_sign;
        look_turn = 0;
        if (look_set_pitch != 100000) { look_pitch = (float)look_set_pitch; look_set_pitch = 100000; }
        look_pitch -= dp;
        w16(cam + 0x20, (int)look_pitch & 0xfff);
        w16(cam + 0x22, (int)look_yaw & 0xfff);
    }
    orig(r);
    if (active) {
        if (!strafe) {   /* let the game swing the view behind the hero as in third person */
            look_yaw = (float)(r16(cam + 0x22) & 0xfff);
            look_pitch = (float)sext12(r16(cam + 0x20));
        }
        if (look_pitch > 900.0f) look_pitch = 900.0f;
        if (look_pitch < -900.0f) look_pitch = -900.0f;
        while (look_yaw >= 4096.0f) look_yaw -= 4096.0f;
        while (look_yaw < 0.0f) look_yaw += 4096.0f;
        w16(cam + 0x20, (int)look_pitch & 0xfff);
        w16(cam + 0x22, (int)look_yaw & 0xfff);
        uint32_t v0 = r[2], v1 = r[3];
        r[4] = cam + 0x20;
        r[5] = vo + 0x0c;
        api->call(0x800a1db8);
        r[2] = v0; r[3] = v1;
    } else if (look_valid) {
        look_valid = 0;
        if (saved_ac2 >= 0) api->game_option("cam_autocenter", saved_ac2);
    }
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

/* 8001dea0 steers the current object (800efdc8) towards the target angle at *(obj+0x9c)+0x1a. For
 * the hero, while CS-style movement is on and he is moving, target and facing are the wanted
 * direction right before it runs, so he runs exactly that way (with the game's own collision). */
static void steer_hook(uint32_t* r, C12OrigFn orig) {
    uint32_t hero = (uint32_t)r32(0x800efdb8);
    if (was_on && cs_move && cur_move_a >= 0 && is_ptr(hero) && (uint32_t)r32(0x800efdc8) == hero) {
        uint32_t tg = (uint32_t)r32(hero + 0x9c);
        if (is_ptr(tg)) w16(tg + 0x1a, cur_move_a);
        set_facing(hero, cur_move_a);
    }
    orig(r);
}

/* 8002c048(obj, dir, speed) is the game's physics step: it moves an object along dir (unit vector
 * x 4096, in the scratchpad) at speed, with the game's own collision. In the scope view it is called
 * for the hero with speed 0; while walking there the mod passes its direction and a slow speed. */
static void physics_hook(uint32_t* r, C12OrigFn orig) {
    uint32_t hero = (uint32_t)r32(0x800efdb8);
    if (enabled && in_scope && scope_move && scope_walk > 0.0f && r[4] == hero && r[6] == 0) {
        api->write32(r[5] + 0, (uint32_t)(int32_t)(scope_dx * 4096.0f));
        api->write32(r[5] + 4, 0);
        api->write32(r[5] + 8, (uint32_t)(int32_t)(scope_dz * 4096.0f));
        r[6] = (uint32_t)(int)scope_walk;
    }
    orig(r);
}

__declspec(dllexport) int c12_mod_init(const C12ModApi* a) {
    if (a->version < 2) return 0;
    api = a;
    api->mod_info("First person", "Play from the hero's eyes with the normal controls. No scope overlay, no scope sound.", &enabled);
    api->setting_float("fov", "Field of view (1 = original)", &fov, 0.7f, 2.0f);
    api->setting_float("eye_up", "Eye height", &eye_up, 150.0f, 450.0f);
    api->setting_float("eye_fwd", "Eye forward / back", &eye_fwd, -150.0f, 150.0f);
    api->setting_bool("direct_look", "Direct mouse look (no camera easing)", &direct_look);
    api->setting_bool("strafe", "Keep looking where you aim while moving (strafe)", &strafe);
    api->setting_bool("cs_move", "CS-style movement (backpedal and strafe relative to the view)", &cs_move);
    api->setting_float("scope_walk", "Walking speed while aiming (0 = keys turn the aim; running is 41)", &scope_walk, 0.0f, 41.0f);
    api->setting_bool("scope_fov", "Same field of view in the scope (R2)", &scope_fov);
    api->setting_bool("laser_beam", "Laser sight beam (the dot stays)", &laser_beam);
    api->setting_bool("face_view", "Hero faces where you look (shots go to the crosshair)", &face_view);
    api->setting_bool("hide_body", "Hide the hero's body", &hide_body);
    api->setting_bool("clean_scope", "Scope view (R2) without scan lines, tint and hum", &clean_scope);
    api->setting_bool("viewmodel", "Weapon view model", &viewmodel);
    api->setting_float("vm_right", "Weapon: right / left", &vm_right, -200.0f, 200.0f);
    api->setting_float("vm_down", "Weapon: down / up", &vm_down, -200.0f, 200.0f);
    api->setting_float("vm_fwd", "Weapon: forward / back", &vm_fwd, 0.0f, 400.0f);
    api->setting_float("vm_yaw", "Weapon: turn (degrees)", &vm_yaw, -180.0f, 180.0f);
    api->setting_float("vm_pitch", "Weapon: tilt (degrees)", &vm_pitch, -90.0f, 90.0f);
    api->hook(0x8005f380, sfx_hook);
    return api->hook(0x80018320, camera_hook) && api->hook(0x80065494, draw_hook) && api->hook(0x80073394, weapon_hook) && api->hook(0x8001dea0, steer_hook) && api->hook(0x8002c048, physics_hook);
}
