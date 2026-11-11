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
rebindable keys, save to `c12.ini`), **`** (hold) = fast-forward, **Alt+Enter** / **F11** = fullscreen.

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
| `runtime/gte.cpp` | GTE with PGXP + widescreen projection |
| `runtime/gpu*.cpp`, `hdtex.cpp` | GPU front-end, D3D11 renderer, software reference renderer, HD textures |
| `runtime/spu.cpp`, `cdrom.cpp`, `hw.cpp` | Sound, CD-ROM + XA, timers/DMA/pad |
| `runtime/game_c12.cpp` | Game-specific glue: fibres → native fibers, frame pacing |
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
* `mods_src\first_person.c` — first-person view mod: the eye sits at the hero's head while the normal
  third-person controls keep working; hides the body, draws the weapon as an adjustable view model,
  optionally strips the scan lines, tint and hum from the scope view (R2),
  adjustable field of view.
* HD textures: DuckStation `texupload-*.png` packs in the `hd_textures=` folder.

## Known limitations

* Level overlays (`RELOCS\*.LVB`) are recompiled position-independently (`gen/<name>_*.c`) and bound when the game
  relocates them (checksum-verified; a modded LVB automatically falls back to the interpreter). Only `gt` and `ronin1`
  have been exercised so far. Code reached only through the interpreter is logged to `interp_entries.txt`:
  add main-EXE addresses to `seeds_main.txt` (overlay offsets to `seeds_<name>.txt`) and rebuild.
* The game's logic is fixed-step at 25 Hz. `interp=1` (default) draws a motion-interpolated in-between frame for every
  game frame: 50 pictures per second at correct speed, +1 refresh (~20 ms) of display delay. Sprites/particles, the
  sky and a few near-camera ground pieces are not interpolated.* Widescreen visibility: the game's view-frustum corner rays (80065db8) are widened so geometry at the screen edges
  is not culled. All 3D polygons are perspective-correct, including the game's screen-space subdivided near polygons.
* Widescreen: HUD and menu elements are re-proportioned and anchored automatically (`hud_fix`); 2D effects drawn
  inside the 3D scene (not through the HUD layer) are still stretched.
* Polygon seams are closed by PGXP; in classic view (PGXP off) the original seams are visible, as on the console.
* `texpage-*` and `vram-write-*` replacement images are not used (only `texupload-*`).
* Mask-bit test (rarely used GPU feature) is not implemented in the D3D11 renderer.
* Debug: `--headless --hw --frames N --shots DIR --every K --input script.txt` runs scripted tests.
