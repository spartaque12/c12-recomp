# C-12: Final Resistance (SCES-03364) — static recompilation

The game's MIPS executable is translated ahead of time to C (`gen/`), compiled with MSVC and linked
against a native runtime (`runtime/`). No game data is shipped: everything is read from your own
disc image (`cue=` in `c12.ini`).

## Setup (you need your own disc)

This repository contains **no game data and no game code**: the recompiled sources (`gen/`) are produced on your
machine from your own disc image of the European release (SCES-03364).

    python tools\extract_disc.py "<Track 1>.bin" disc     # extract the disc's files once
    copy c12.ini.example c12.ini                           # then set cue= to your .cue
    powershell -File build.ps1                             # recompile + build (VS 2022 Build Tools, CMake, Ninja)

Optional: put a DuckStation-style HD texture pack into the `hd_textures` folder.

## Run / build

    build\c12.exe                 # play (settings in c12.ini; any key can be overridden: --scale 6)
    .\build.ps1                   # regenerate gen\*.c from the disc EXE and rebuild
    .\build.ps1 -NoRecomp         # rebuild the runtime only

Hotkeys: **Tab** = switch classic / remastered presentation, **F1** or **Esc** = settings menu (all options live,
rebindable keys, save to `c12.ini`), **`** (hold) = fast-forward, **Alt+Enter** / **F11** = fullscreen, **F5** = save state, **F9** = load state, **F6** / **F7** = previous / next save state slot (1-9).

Default keyboard + mouse (rebind in the menu, Controls tab): WASD or arrows = move, mouse = camera (right stick),
left click or Space = attack / accept (Cross), right click = first-person view (R2), Left Shift or middle click =
lock on / strafe (R1), Q or mouse wheel = cycle weapons (L1), E = Square, F = Circle, R or Backspace = Triangle (back),
C = L2, Enter = Start, G = Select. XInput pads work at the same time (analog sticks + rumble). Saves go to
`saves\card1.mcd`, a standard raw memory-card image (readable by emulators).

## Layout

| Path | What |
|---|---|
| `tools/recomp/` | The recompiler: function discovery, jump tables, MIPS → C |
| `gen/` | Generated C: main EXE (1704 functions) + 25 overlays (~3900), `*_map.txt` = function lists |
| `runtime/core.cpp` | Memory map, scheduler, IRQs, dispatch, interpreter fallback |
| `runtime/bios.cpp`, `memcard.cpp` | High-level BIOS (no BIOS image needed), memory card image (file API + raw sectors) |
| `runtime/gte.cpp` | GTE with widescreen projection |
| `runtime/gpu*.cpp`, `hdtex.cpp` | GPU front-end, D3D11 renderer, software reference renderer, HD textures |
| `runtime/level3d.cpp`, `gpu3d.h` | Native 3D renderer: level, object models, effect meshes as real 3D triangles |
| `runtime/spu.cpp`, `cdrom.cpp`, `hw.cpp` | Sound, CD-ROM + XA, timers/DMA/pad |
| `runtime/game_c12.cpp` | Game-specific glue: fibres → native coroutines, frame pacing |
| `runtime/savestate.cpp`, `costack.cpp`, `ctxswitch.asm` | Save states; coroutine stacks at fixed addresses |
| `runtime/mods.cpp`, `c12_mod_api.h` | Mod loader and native mod API |
| `re/main_decomp.c` | Ghidra decompilation of the EXE (reference for modding) |

## Modding

* `mods\disc\<PATH>` replaces a disc file, `mods\disc\<PATH>@<HEX>` patches it at a byte offset
  (e.g. `mods\disc\PROJFILE.MWD@1A2000`).
* `mods\*.dll` — native mods. Every original function can be hooked by its PSX address;
  see `runtime\c12_mod_api.h` and `mods_src\example_mod.c`. A mod can register itself and its settings
  (on/off switch, sliders) for the **Mods** tab of the settings menu, change the field of view and a few
  runtime options; settings are stored in `c12.ini` as `mod.<name>.<key>`. One hook per game function.
  Build all mods with `mods_src\build_mods.ps1`.
* `mods_src\first_person.c`: a first-person view mod. The eye sits at the hero's head while the normal
  controls keep working, with CS-style movement and direct mouse look. The hero faces the view, so shots go to the
  crosshair. Climbing, ladders, ledges, crouching and turrets all work. The mod steps aside in cutscenes and menus.
  It hides the body and has an optional adjustable weapon view model, an optional clean scope view (R2) without the
  scan lines, tint and hum, an adjustable field of view, and a give-all-weapons cheat option.
* HD textures: DuckStation `texupload-*.png` packs in the `hd_textures=` folder.

## VR (OpenXR headset)

`mods\vr.dll` (source `mods_src\vr`, build `mods_src\vr\build_vr.ps1`) plays the game in a headset through any OpenXR
runtime (SteamVR, Oculus/Meta, WMR, Virtual Desktop...). Switch it on in the settings menu, Mods tab ("VR"); F8 recentres.
`openxr_loader.dll` must be next to it in `mods\`.

* Real stereo: the runtime draws the last world frame once per eye at the headset's rate (72-120 Hz) from each eye's
  own pose and lens. The native 3D scene (level, characters, objects, effects, sky, shadows) is re-projected per eye
  and blended between the game's 25 Hz frames, so head motion is always smooth; sprites, glows and lines are moved
  into each eye at their sort depth.
* With the first-person mod the camera turns with your head: W/A/S/D and aiming follow where you look, the mouse
  turns the body. Without it the head looks around the game's own camera.
* The HUD floats in front of you; menus, loading screens and films are shown on a screen ahead.
* Settings (Mods tab): world scale, render scale, HUD and screen distance / width.
* Testing without a headset: `--vr_test 1` with test screenshots writes both eyes and the HUD; `mods_src\vr\README.md`
  describes the simulated OpenXR runtime (`vr_xr_test_runtime.dll`, captures of what each eye sees).
* Motion controllers (Touch, Index, Vive, WMR, HP Reverb, simple): left stick move, right stick turn (30 degree
  snaps or smooth), right trigger fire / accept, left trigger crouch, right grip lock on, left grip next weapon,
  A use, B back, X scope view, Y Select, menu Start, right stick click look. Shots go where the right controller
  points (a laser beam shows it, stopped by walls); with the first-person mod the weapon is in your hand. A short
  rumble on firing. Keyboard / mouse / pad keep working.
* Comfort: snap or smooth turn, the HUD follows your head lazily (or fixed to the view), all in the Mods tab.

## Features

**Recompilation**
* The whole main EXE and the level overlays (`RELOCS\*.LVB`) run as native code. Overlays are recompiled
  position-independently (`gen/<name>_*.c`) and bound when the game relocates them (checksum-verified; a modded LVB
  falls back to the interpreter automatically). Code reached only through the interpreter is logged to
  `interp_entries.txt`: add main-EXE addresses to `seeds_main.txt` (overlay offsets to `seeds_<name>.txt`) and rebuild.
* High-level BIOS: no BIOS image needed. The game's fibres run as native coroutines. The memory card is a real raw card
  image (`saves\card1.mcd`, readable by emulators) and file I/O doesn't wait.
* The whole game is playable start to finish, in classic and remastered view and with the first-person mod.

**Remastered graphics** (Tab switches between classic and remastered)
* Native 3D renderer (`runtime/level3d.cpp`, `gpu3d.h`): the level geometry, every object model (hero, enemies,
  weapons, doors, crates: per-part matrices, the game's lighting and fog), effect meshes (control panels, lamps,
  screens, with the PS1's stacking order kept), the sky dome, blob shadows and bullet marks are read from the game's data
  and drawn as real 3D triangles. They get a depth buffer, per-pixel perspective and near-plane clipping instead of the
  game's screen-space projection, subdivision and sort order. Sprites, particles, lines and the HUD still go through the
  PS1 path and are depth-tested against the native scene at the depth of their sort slot. Options: `level3d`
  (also in the menu), `n3_parts` (debug mask: 1 level, 2 effects, 4 objects, 8 sky, 16 shadows and marks),
  `n3_depth_ps1`.
* Internal resolution up to many times native (`scale`), optional bilinear filtering and DuckStation `texupload-*`
  HD texture packs (`hd_textures`).
* Widescreen 16:9: real wider field of view. The view-frustum corner rays (80065db8) are widened so geometry at the
  screen edges isn't culled. Screen-facing effects (lamp glows) stay on their lamps at any aspect ratio and field of
  view. HUD, menus, inventory and map panels are re-proportioned and anchored automatically (`hud_fix`).
* Menus: item icons are drawn by the native renderer with perspective-correct textures, steady sub-pixel positions and
  smooth spinning.

**Smooth motion**
* The game's logic is fixed-step at 25 Hz. `interp=1` (default) draws a motion-interpolated in-between frame for every
  game frame: 50 pictures a second at correct game speed, with about 20 ms extra display delay. The camera and every
  object part are blended between game frames, and rotations stay rotations.
* `smooth_present=1` (default; Video tab) gives the window a picture on every monitor refresh (60, 144, 240 Hz...),
  with the native scene blended to that moment.

**Controls and comfort**
* Keyboard + mouse with rebindable keys, XInput pads with analog sticks and rumble at the same time.
* Raw mouse camera in third-person view, a camera auto-centre toggle, and mouse aim in the scope view and on gun turrets
  (own speed setting).
* Settings menu (F1 / Esc) with every option live and saved to `c12.ini`. Also fast-forward, fullscreen, and fast
  loading (`cd_fast`).
* Sound: SPU with console-like interpolation and reverb, CD-XA audio.

**Save states**
* F5 / F9 / F6-F7 (also in the settings menu with a slot picker). `states\slotN.c12s` holds the whole running game,
  including the native stacks of its coroutines. A state therefore only loads into the same build of c12.exe with the
  same mods: rebuilding makes older states unusable. Loading a state doesn't touch the memory card.

**Mods**
* Native DLL mods with hooks on any game function, a Mods tab with their settings, and disc file replacement.
  The first-person mod and VR mod are described above.

## Notes

* `texpage-*` and `vram-write-*` replacement images are not used (only `texupload-*`).
* The mask-bit test (a rarely used GPU feature) is not implemented in the D3D11 renderer.
* 2D effects drawn inside the 3D scene (not through the HUD layer) are still stretched in widescreen.
* Debug / tests: `--headless --hw --frames N --shots DIR --every K --input script.txt` runs scripted tests;
  `--load_state FILE`, or `--state_file FILE` with `--save_state_at FRAME` / `--load_state_at FRAME`;
  `--store_watch ADDR` logs writes to a guest address with call stacks. Mods must be built with `/GS-`
  (see `c12_mod_api.h`).
