# VR (OpenXR) for C-12

The OpenXR layer for the VR mod, ported from the VCS-Recomp VR.asi layer (itself modelled on praydog/REFramework's
OpenXR runtime, MIT), plus a simulated OpenXR runtime and a headless smoke test.

## Files

| File | What |
|---|---|
| `xr.h`, `xr.cpp` | The OpenXR layer: loader loaded at run time (`openxr_loader.dll` next to the mod), instance with `XR_KHR_D3D11_enable`, session on an existing D3D11 device, VIEW / LOCAL / recentred spaces, swapchains per panel, frame loop, projection layer (mono or **stereo**) + quad layers (HUD premultiplied alpha, cinema), recentre, `xrLocateViews` per-eye poses and fovs, `<exe folder>\vr.log`. Main thread only. Built into `build\mods\vr\vr_xr.lib`. |
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
`XRTEST_FORMATS`, `XRTEST_TYPELESS`, `XRTEST_NO_HMD=s`, `XRTEST_LOG=file`, `XRTEST_CAPTURE=dir` (periodic captures +
`layers.txt`), `XRTEST_CAPTURE_EVERY`, `XRTEST_CAPTURE_MAX`, `XRTEST_CAPTURE_FORMAT=bmp|png`; control file commands
(`XRTEST_CONTROL`): `sleep wake unfocus focus loss exit instance_loss recenter head y,p,r[,x,y,z] hz N capture <name>`.

## Notes

- The game's mod loader `LoadLibrary`s every `mods\*.dll`: `openxr_loader.dll` and `vr_xr_test_runtime.dll` get loaded
  too and logged as "has no c12_mod_init export" (harmless). Build with `-NoTestRuntime` to leave the test runtime out
  of `mods\` for a release (point `C12_MODS_OUT` elsewhere for a test build).
- `vr.dll` is C++ with a static CRT; it exports `c12_mod_init` with C linkage and depends only on KERNEL32/ADVAPI32
  (D3D11 functions come through the device it is given).
