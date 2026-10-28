<p align="center"><img src="docs/header.jpg" alt="Divinity II Reborn"></p>

# Divinity II Reborn

A visual, engine and quality-of-life overhaul for **Divinity II: Developer's Cut** (Steam, Windows):
a spoiler-free **mind-reading advisor**, **4x AI-upscaled textures**, HD normal maps and icons, a fix for the
game's **hard, flickering shadow edges**, 8K shadow maps, smoother bloom and sun rays, correct **ultrawide menus**,
intro skip and more. Gameplay, balance and saves are not changed.

**Download, full description and installation:** Nexus Mods — *(link)*

This repository contains the source code of **Div2Fix** (`bin/dinput8.dll`), the engine part of the mod,
and the patches of the modified DXVK build (`bin/d3d9.dll`) shipped since 1.4.

## For players

**Requirements:** Divinity II: Developer's Cut (Steam), a Vulkan 1.3 GPU,
and **[Div2Patch](https://github.com/BlanknameES/Div2Patch)** by BlanknameES (install it first).

**Installation:** install Div2Patch → extract `DivinityIIReborn_v1.4.1.zip` into the game folder (the one with
`bin` and `Data`) → optionally extract the HD texture pack there too → play.
Settings: `bin\Div2Fix.ini`. Uninstall: `_Div2Reborn\uninstall.cmd`.

### New in 1.4.1

* **Tree draw distance** — the SpeedTree code reads `TreeQualityMultiplier` (menu maximum 3) only through the getter
  `0x6A2C20`; Div2Fix makes it return `[Graphics] TreeDistanceMultiplier` (default 6), so trees no longer pop in
  right in front of the hero and the options menu cannot reset it. Object LOD distance default 3 → 5.
* **DXVK patch 0003** `d3d9.asyncManagedUpload` — at the first draw of a managed texture its data is packed into the
  staging buffer on DXVK's CS thread instead of the game's render thread (the render thread only pins the
  memory-file allocation; `MemoryFilePool` defers a free until the last unmap).
* **DXVK patch 0004** `d3d9.stagingMemoryPerSubmission` — the 10 MB staging budget of 32-bit DXVK made the game wait
  for the GPU with 11–22 MB textures; the mod sets 32. With 0003 + 0004 on the same route: frames > 33 ms 7 → 1,
  worst frame 47.5 → 40.3 ms (one run each).
* Fixed a crash of the optional profiler (it read code bytes of an unloaded DLL).

### New in 1.4

* **Shorter texture-loading hitches** — DXVK zeroed the CPU copy of every new managed texture before the game
  overwrote it completely; with 4K textures (11–22 MB each) that cost a memset plus a page fault per page on the
  render thread. The shipped DXVK has an option to skip it (`d3d9.skipManagedTextureInit`, see `dxvk/`).
  Same route in game: texture-loading work −48 %, frames > 33 ms 39 → 4, worst frame 89 → 43 ms.
* **Sharp dragon/character shadow** — `CCharacterShadowMap::Init` hard-codes a 1024² `R32F` map with a hard depth
  compare; `[Graphics] CharacterShadowSize=4096` removes the large stair steps.
* Softer, farther cascade transition (`ShadowSplitWeight=0.6`).
* Fixed a crash: DirectInput hooks of two modules on the shared device vtable could call each other forever when
  the game re-created a device. Fixed a possible start-up crash on other game builds (intro skip).
* Optional diagnostics: sampling profiler of long frames, texture statistics, 32-bit address-space log.

### Highlights

* **Mind-reading advisor** — the "Mind reading will cost you N experience" popup gets one more line telling you
  whether the read is worth it: *useful* (skill/stat points, items, skills, XP, gold), *something, but not much*
  (discounts, dialogue options, quest info) or *just thoughts*. The verdicts come from the game's own dialogue and
  script data for 337 characters; no names, no plot, no spoilers. 7 languages.
* **Soft shadows** — the game's variance-shadow-map blur throws away the depth variance in its second pass, so shadow
  edges are hard, stair-stepped and flicker on foliage. Div2Fix replaces that pass with a corrected copy.
* **HD textures** (separate pack) — 1,516 colour textures 4x, 1,225 normal maps and 1,508 material layers 2x,
  966 icons and 227 interface images 4x; Div2Fix draws the HD icons at their original on-screen size.
* **Ultrawide menus** at 16:9 with correct mouse hit-testing, key names in hints with non-Latin layouts,
  non-Latin map-marker text, in-game keyboard-layout switching, intro skip, window icon, strafe left+right fix,
  8K shadow maps, 3x draw distance, smoother bloom and sun rays, wider camera zoom.

<p align="center">
<img src="docs/compare_face.jpg" width="49%" alt="Texture comparison: face">
<img src="docs/compare_dragon.jpg" width="49%" alt="Texture comparison: dragon">
<img src="docs/compare_icons.jpg" width="98%" alt="Icon comparison">
</p>

## How Div2Fix works

Div2Fix is a `dinput8.dll` proxy: the game loads it from its `bin` folder, every DirectInput call is
forwarded to the real `System32\dinput8.dll`, and at start-up Div2Fix applies small in-memory patches
to `Divinity2.exe`. **No file of the game is modified and nothing is written to saves.**
Every patch first compares the original bytes at its location; if they differ (another game build),
that patch is skipped and the reason is written to `bin\Div2Fix.log`.

The DLL is plain C (no C runtime, no third-party code). It imports only `kernel32.dll` and `user32.dll`
(see `src/kernel32.def`, `src/user32.def`) and does not access the network, the registry or any file
outside the game folder (it reads `Div2Fix.ini`, `Div2Fix_mindread.txt`, `..\lang.ini` and writes
`Div2Fix.log`; with `[Diagnostics] Enable=1` it also writes frame-time reports next to the game).

## Modules (`div2fix/src`)

| File | What it does |
|---|---|
| `div2fix.c` | DLL entry, `dinput8` export forwarding, settings, logging; key names in hints with non-Latin keyboard layouts; ultrawide (21:9/32:9) menus shown at 16:9 with correct mouse hit-testing |
| `gfx.c` | object LOD / draw distance multiplier, shadow-map size, shadow distance, cascade split, shadows from all objects, character (dragon) shadow-map size, tree (SpeedTree) draw distance |
| `shadow.c` | soft shadows: replaces the game's faulty 2nd variance-shadow-map blur pass with a corrected copy of the same shader; larger bloom and light-shaft render targets |
| `mindread.c` | mind-reading advisor: adds a spoiler-free verdict line to the mind-read confirmation popup (data: `Div2Fix_mindread.txt`) |
| `icons.c` | draws HD (4x) interface icons at their original on-screen size |
| `uitext.c` | fixes non-Latin (ANSI / widened UTF-8) text shown as boxes, e.g. map-marker labels |
| `intro.c` | skips the start-up logo video |
| `camera.c` | wider mouse-wheel zoom range, zoom step and dragon-form distance (values of Div2Patch, changed in memory) |
| `window.c` | window icon; keyboard-layout switching in game; strafe left+right cancel |
| `version.c` | warns once at start-up if the game build is not the one the mod targets |
| `diag.c` | optional diagnostics: frame-time recorder (F11, or automatic), address-space log, D3D9 device hooks used by `shadow.c` |
| `texmgr.c` | optional diagnostics: texture creation / lock / upload statistics per frame (`Div2Fix_tex_*`) |
| `profiler.c` | optional diagnostics: sampling profiler of the render thread in long frames (`Div2Fix_prof_*`) |
| `detour.c` | minimal x86 function detour used for D3D9 device methods |

## Build

* **Linux**: LLVM 18 (`clang-cl-18`, `lld-link`, `llvm-dlltool`, `llvm-rc-18`) → `sh div2fix/build.sh`
* **Windows**: LLVM for Windows on `PATH` → `div2fix\build.cmd`

Output: `div2fix/build/dinput8.dll`. See `CHECKSUMS.txt` for the hashes of the released files.

**DXVK** (`dxvk/`): DXVK v3.1.1 + `dxvk/patches/*.patch`, built with MSYS2 MINGW32 by `dxvk/build_dxvk.sh`
(instructions at the top of the script). The patches only add options that are off by default:

| Patch | Option | Shipped |
|---|---|---|
| `0001-d3d9-skipManagedTextureInit` | do not zero the CPU copy of new managed textures | **on** (`bin/dxvk.conf`) |
| `0002-d3d9-uploadManagedOnUnlock` | upload managed levels at `UnlockRect` instead of the first draw | off (experimental, worse worst-case frames) |
| `0003-d3d9-asyncManagedUpload` | pack managed texture data for upload on the CS thread | **on** |
| `0004-d3d9-stagingMemoryPerSubmission` | staging memory budget per submission (MB) | **32** |

## Third-party files in the release

* `bin/d3d9.dll` — **altered** DXVK 3.1.1 (zlib license): official source + the patches in `dxvk/patches`.
  Not the official DXVK release. With `d3d9.skipManagedTextureInit = False` it behaves like official 3.1.1;
  the official `x32/d3d9.dll` can also be used instead (then remove that line from `dxvk.conf`).
* `res/div2.ico` — the game's own icon (© Larian Studios), used as the window icon.

## Credits

* **Larian Studios** — Divinity II and all original assets.
* **[Div2Patch](https://github.com/BlanknameES/Div2Patch)** by BlanknameES — required, not included.
* **[DXVK](https://github.com/doitsujin/dxvk)** by Philip Rebohle and contributors — shipped with small patches (`dxvk/`).
* **4x-UltraSharp** by Kim2091 (CC BY-NC-SA 4.0), run with Real-ESRGAN-ncnn-vulkan — used to upscale the textures.

## License

MIT (see `LICENSE`). Divinity II and all its assets are © Larian Studios; this is a non-commercial fan project.
