# VR (OpenXR) for C-12

The OpenXR layer for the VR mod, ported from the VCS-Recomp VR.asi layer (itself modelled on praydog/REFramework's
OpenXR runtime, MIT), plus a simulated OpenXR runtime and a headless smoke test.

## Files

| File | What |
|---|---|
| `xr.h`, `xr.cpp` | The OpenXR layer: loader loaded at run time (`openxr_loader.dll` next to the mod), instance with `XR_KHR_D3D11_enable`, session on an existing D3D11 device, VIEW / LOCAL / recentred spaces, swapchains per panel, frame loop, projection layer (mono or **stereo**) + quad layers (HUD premultiplied alpha, cinema), recentre, `xrLocateViews` per-eye poses and fovs, motion controllers (`syncInput`, `haptic`), `<exe folder>\vr.log`. Main thread only. Built into `build\mods\vr\vr_xr.lib`. |
| `vr_mod.cpp` | `mods\vr.dll`, a placeholder: registers "VR" (on/off) and logs whether the loader and a runtime are found (`vr::probe`). To be replaced by the real mod. |
| `test/xr_test_runtime.cpp`, `.json` | `mods\vr_xr_test_runtime.dll` + `.json`: a simulated HMD (2 views, IPD 64 mm, 90 Hz, asymmetric 50/45 deg fovs), validates the application's calls (`VALIDATION:` lines), composites what each eye would see on the CPU and dumps it (PNG or BMP). |
| `test/xr_smoke.cpp` | `build\mods\vr\vr_xr_smoke.exe`: own D3D11 device, `vr::start`, N frames of a stereo layer (left eye red, right eye blue, white square in the middle) + a HUD quad, `vr::stop`. Exit code 0 when every frame was submitted. Argument `mono` tests the mono path. |
| `build_vr.ps1` | Builds everything above with MSVC (C++17, `/MT`, `/GS-`, fixed `/BASE` per DLL as `build_mods.ps1` does) and copies `openxr_loader.dll` into `mods\`. `-Test` runs the smoke test. |
| `third_party/openxr/` | OpenXR 1.1.65 headers and `bin/openxr_loader.dll` (Apache-2.0 OR MIT). |

## Stereo submission

Each frame: `vr::poll()`, `vr::waitAndBegin()`; if `vr::shouldRender()`: `vr::locateViews(views)` (both eyes in the
recentred space at the predicted display time), render eye *i* with `views[i]` into
`vr::panelImage(vr::eyePanel(i), w, h, srgb)` (one swapchain per eye; `w`/`h` from `vr::recommendedSize`), the HUD
into `panelImage(Panel::Hud, ...)`, then `vr::endFrame(proj, quads, n)` with `proj.on = proj.stereo = true` and
`proj.views[i] = views[i]`. Mono: `proj.stereo = false`, `proj.pose` / `proj.fov`, image in `Panel::Scene`.

## Motion controllers

One action set (`c12`) is created and its bindings suggested once per instance, and attached to every session as it is
created (before the first `xrSyncActions`), with grip / aim action spaces per hand.

```cpp
struct Hand { bool active = false; Pose grip; Pose aim; float trigger = 0, squeeze = 0; float stick[2] = {0,0};
              bool stickClick = false, primary = false, secondary = false, menu = false; };
bool syncInput(Hand out[2]);   // 0 = left, 1 = right; after waitAndBegin()
void haptic(int hand, float amplitude, float seconds);   // amplitude 0 stops; seconds <= 0: shortest pulse
std::string interactionProfile(int hand);                 // "/interaction_profiles/...", "" when none
```

- `syncInput` reads only while the session is **focused** (otherwise, and with no frame begun, it returns false with
  inactive hands). Poses are at the predicted display time in the same recentred space as `locateHead` /
  `locateViews`. `active`: bound and the grip pose tracked; the aim pose's -z is the pointing direction.
- Interaction profile changes (`XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED`, handled in `poll()`) are logged per hand.
- `status()` ends with `input=attached|none left=<profile> right=<profile>`.

| Profile | trigger | squeeze | stick | stickClick | primary | secondary | menu |
|---|---|---|---|---|---|---|---|
| `oculus/touch_controller` | trigger/value | squeeze/value | thumbstick | thumbstick/click | X (L) / A (R) | Y (L) / B (R) | menu (L) |
| `valve/index_controller` | trigger/value | squeeze/value | thumbstick | thumbstick/click | A | B | - |
| `htc/vive_controller` | trigger/value | squeeze/click | trackpad | trackpad/click | trackpad/click | menu | menu |
| `microsoft/motion_controller` | trigger/value | squeeze/click | thumbstick | thumbstick/click | trackpad/click | menu | menu |
| `khr/simple_controller` | select/click | - | - | - | - | - | menu |
| `hp/mixed_reality_controller`* | trigger/value | squeeze/value | thumbstick | thumbstick/click | X / A | Y / B | menu |

All profiles also bind grip/pose, aim/pose and output/haptic. *Only when the runtime offers
`XR_EXT_hp_mixed_reality_controller` (the layer then enables it at instance creation).

## Test

```powershell
mods_src\vr\build_vr.ps1 -Test
```

builds and runs `vr_xr_smoke.exe 100` with `XR_RUNTIME_JSON=mods\vr_xr_test_runtime.json`; at frame 50 it asks the
runtime (control file `build\mods\vr\smoke\control.txt`, line `capture stereo`) to dump
`build\mods\vr\smoke\eye_stereo_left.bmp` / `eye_stereo_right.bmp` (the composited eyes) and `layer_stereo_*` (the
submitted images). Logs: `build\mods\vr\vr.log` (the layer) and `build\mods\vr\smoke\xr_test_runtime.log`.

**Elevated processes**: the Khronos loader ignores `XR_RUNTIME_JSON` in a high-integrity process (this machine has
UAC off, so every process is). The build script then sets `C12_VR_RUNTIME=<mods>\vr_xr_test_runtime.dll`, which makes
`xr.cpp` negotiate with that runtime DLL directly, without the loader.

Test runtime knobs: `XRTEST_HEAD=yaw,pitch,roll[,x,y,z]`, `XRTEST_YAW_SWEEP=deg`, `XRTEST_IPD=mm`, `XRTEST_HZ`,
`XRTEST_FORMATS`, `XRTEST_TYPELESS`, `XRTEST_NO_HMD=s`, `XRTEST_NO_HP_EXT=1` (do not offer
`XR_EXT_hp_mixed_reality_controller`), `XRTEST_LOG=file`, `XRTEST_CAPTURE=dir` (periodic captures + `layers.txt`),
`XRTEST_CAPTURE_EVERY`, `XRTEST_CAPTURE_MAX`, `XRTEST_CAPTURE_FORMAT=bmp|png`; control file commands
(`XRTEST_CONTROL`): `sleep wake unfocus focus loss exit instance_loss recenter head y,p,r[,x,y,z] hz N capture <name>`,
and for the controllers:

| Command | What |
|---|---|
| `hand l\|r x,y,z[,yaw,pitch,roll]` | the grip pose in the recentred space (metres, degrees; yaw left +, pitch up +, roll right +) |
| `button l\|r trigger\|squeeze\|stick_click\|primary\|secondary\|menu <0..1>` | an input's value (primary = X / A, secondary = Y / B) |
| `stick l\|r x,y` | the thumbstick (-1..1) |

The runtime simulates an `oculus/touch_controller` pair: left at (-0.2, -0.3, -0.35), right at (0.2, -0.3, -0.35),
pointing ahead, relative to the head at the session's start (or at the last `recenter`, yaw only); aim = grip (own
action space). A hand gets the Touch profile at the first `xrSyncActions` once something of it is bound there; actions
are inactive (and action spaces untracked) while the session is not FOCUSED. It validates paths, action (set) names,
bindings against each profile's components and the action types, attach / sync / state call order and types
(`VALIDATION:` lines), and logs haptics (`haptic right: amplitude 0.50, duration 100.0 ms, ...`).

The smoke test syncs input every frame (hands printed every 30 frames), checks the default poses, sends
`stick r 0.5,-0.25`, `button r trigger 0.8`, `button l primary 1`, `hand l -0.1,-0.2,-0.4,10,0,0` at frame 20 and
checks they are seen, pulses the right haptic at 25, and `unfocus` (60) / `focus` (70) to check the hands go inactive
and come back: `input checks PASSED`, else exit code 4.

## Notes

- The game's mod loader `LoadLibrary`s every `mods\*.dll`: `openxr_loader.dll` and `vr_xr_test_runtime.dll` get loaded
  too and logged as "has no c12_mod_init export" (harmless). Build with `-NoTestRuntime` to leave the test runtime out
  of `mods\` for a release (point `C12_MODS_OUT` elsewhere for a test build).
- `vr.dll` is C++ with a static CRT; it exports `c12_mod_init` with C linkage and depends only on KERNEL32/ADVAPI32
  (D3D11 functions come through the device it is given).
