<p align="center"><img src="docs/header.jpg" alt="Divinity II Reborn"></p>

# Divinity II Reborn

A visual, engine and quality-of-life overhaul for **Divinity II: Developer's Cut** (Steam, Windows):
a spoiler-free **mind-reading advisor**, **4x AI-upscaled textures**, HD normal maps and icons, a fix for the
game's **hard, flickering shadow edges**, **SMAA + TAA anti-aliasing** before the interface, 8K shadow maps,
smoother bloom and sun rays, correct **ultrawide menus**, intro skip and more. The mod does not change gameplay,
balance or saves (an optional, separately installed skill-balance module is the only exception, see below).

**Download, full description and installation:** Nexus Mods — *(link)*

This repository contains the source code of **Div2Fix** (`bin/dinput8.dll`), the engine part of the mod,
the patches of the modified DXVK build (`bin/d3d9.dll`) shipped since 1.4, and since 1.5 the sources of the
replacement shaders and of SMAA/TAA (`shaders/`, built by `tools/shaders/`).

## For players

**Requirements:** Divinity II: Developer's Cut (Steam), a Vulkan 1.3 GPU,
and **[Div2Patch](https://github.com/BlanknameES/Div2Patch)** by BlanknameES (install it first).

**Installation:** install Div2Patch → extract `DivinityIIReborn_v1.5.zip` into the game folder (the one with
`bin` and `Data`) → optionally extract the HD texture pack there too → play.
Settings: `bin\Div2Fix.ini`, `bin\dxvk.conf`. Uninstall: `_Div2Reborn\uninstall.cmd`.

### New in 1.5

* **SMAA + TAA before the interface** (DXVK patches 0011, 0012) — right after the engine's tonemap draw
  (`HDRScene_FinalScenePass`, found by the SHA-1 of its pixel shader) DXVK runs SMAA (edges from luma and from the
  engine's packed depth + normal G-buffer) and TAA: sub-pixel jitter of the projection, reprojection with the
  engine's depth and the camera read from the light pre-pass constants, rejection of disoccluded pixels. The
  interface is drawn afterwards and stays sharp. F11 cycles SMAA + TAA / TAA / SMAA / off. 0.8 ms GPU per frame at
  3440x1440 (RTX 3080 Ti).
* **Shadow cascade seam fixed, shadows twice as far** (DXVK patch 0010 + `shaders/far`) — DXVK replaces two pixel
  shaders of the sun light pre-pass (by the SHA-1 of their original bytecode) with recompiled ones: the engine's own
  VSM test, but blended between cascades over the last 15 % of each cascade (the hard seam used to move with the
  hero) and extended to 2x the shadow distance with a smooth fade. Constant layout checked against the original
  effect at build time; `shaders/` rebuilds the shipped `.dxso` files byte for byte.
* **Archive overlay** (`overlay.c`, `[Overlay]`) — detours the archive registration (`0x697D30`, thiscall, ret 10h)
  and registers `Data\Win32\Packed\Reborn\*.dv2` before the 12 patch slots, with the patch flags; files in them win
  over the game and the HD pack (files of the region archives `World\<region>\...` are not overridden). No archive
  of the game is changed.
* **Effects pack** (`Reborn\Reborn_FX.dv2`, 64 MB) — 220 of 527 effect textures (`FX_*`) picked by sharpness
  (`tools/reborn/fx_select.py`: share of strong luminance steps) and upscaled 4x with 4x-UltraSharp; soft glows and
  smoke were left alone.
* **Crash fix** (DXVK patch 0014) — stack overflow, mostly when loading a save: the NVIDIA Vulkan driver's
  `opengl32.dll` subclasses the window on top of DXVK; when the swapchain was re-created, `ResetWindowProc` dropped
  the game's window procedure and `HookWindowProc` stored the subclass as the original, so `D3D9WindowProc` and
  the subclass called each other until the stack ran out. DXVK now stays in such a chain.
* **Crash log** — on a stack overflow `Div2Fix.log` also gets the return addresses on the whole stack per module,
  the innermost and the most frequent game return addresses, and the address-space state.
* Development parts (frame census 0009, GPU timers 0013, F11 debug views 0015) are in the DLL but off in
  `dxvk.conf`; 0010–0012 are built on top of 0009.

### New in 1.4.2

* **Parallel texture copy** (`parcopy.c`) — `CopyRendererDataToSurfaceLevel` copies each new texture level with
  `memcpy_s` (call at `0x5D962D`) into fresh memory-file pages; the copy is page-fault bound. Levels ≥ 1 MB are now
  split between the game thread and 3 workers (returns after all parts). In game: copy time −33 %, P99.9 44 → 31 ms.
* **DXVK patch 0005** `d3d9.parallelManagedUploadThreads` — with 0003, the CS thread packs large managed levels into
  staging with 4 threads. Worst area-entry frames 381 → 199 ms and 257 → 128 ms.
* **DXVK patch 0006** `d3d9.deferUnmappableRelease` — freed memory-file mappings are closed on a background thread;
  the 80–118 ms frames when leaving an area are gone.
* Same route vs 1.4.1: worst frames 336 → 209 ms and 218 → 126 ms, P99.9 44.3 → 29.1 ms (one run each).
  End-to-end DXVK test (16 DXT1 textures, every read-back pixel compared): 0 wrong pixels in all variants.
* **Crash fix** (`crashguard.c`, `[General] FSMaterialGuard`) — `0xBA4FC0` reads the `FSMaterial` string extra data
  at `[edi+0Ch]` without a NULL check (crash `Divinity2.exe+7A4FE8`); the guard returns through the function's own
  epilogue when the lookup or the `NiStringExtraData` cast fails.
* **Crash log** (`[General] CrashLog`) — a vectored exception handler writes code, module+offset and return
  addresses of the first 16 access violations etc. to `Div2Fix.log`; it never changes exception handling.
* **Optional skill-balance module** (`_Div2Reborn/Optional/SkillBalance`, **changes gameplay**, installed by hand):
  `FOV_Patch_2.dv2` overrides `rpgstats_skillprototypes.xml` (both episodes) and the Russian `Skills_Localisation.xml`
  — Summon Mastery, Regenerate, Reflect, Poison Arrows, Blind, Firewall values; Summon/Destruction Mastery
  descriptions. Details in its `README_RU.txt`. Not part of the main mod.

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
| `parcopy.c` | parallel copy of large new texture levels (`[Textures] ParallelCopyThreads`) |
| `crashguard.c` | FSMaterial crash guard, crash log (`[General] FSMaterialGuard`, `CrashLog`) |
| `overlay.c` | archive overlay: registers `Data\Win32\Packed\Reborn\*.dv2` before the patch slots (`[Overlay]`) |
| `detour.c` | minimal x86 function detour used for D3D9 device methods |

## Build

* **Linux**: LLVM 18 (`clang-cl-18`, `lld-link`, `llvm-dlltool`, `llvm-rc-18`) → `sh div2fix/build.sh`
* **Windows**: LLVM for Windows on `PATH` → `div2fix\build.cmd`

Output: `div2fix/build/dinput8.dll`. See `CHECKSUMS.txt` for the hashes of the released files.

**DXVK** (`dxvk/`): DXVK v3.1.1 + `dxvk/patches/*.patch`, built with MSYS2 MINGW32 by `dxvk/build_dxvk.sh`
(instructions at the top of the script; the patches are applied in name order). Except 0014 (a bug fix) the
patches only add options that are off by default:

| Patch | Option | Shipped |
|---|---|---|
| `0001-d3d9-skipManagedTextureInit` | do not zero the CPU copy of new managed textures | **on** (`bin/dxvk.conf`) |
| `0002-d3d9-uploadManagedOnUnlock` | upload managed levels at `UnlockRect` instead of the first draw | off (experimental, worse worst-case frames) |
| `0003-d3d9-asyncManagedUpload` | pack managed texture data for upload on the CS thread | **on** |
| `0004-d3d9-stagingMemoryPerSubmission` | staging memory budget per submission (MB) | **32** |
| `0005-d3d9-parallelManagedUpload` | pack large managed levels with worker threads (with 0003) | **4** |
| `0006-d3d9-deferUnmappableRelease` | close freed memory-file mappings on a background thread | **on** |
| `0009-d3d9-frameCensus` | development report of one frame (draws, shaders, targets) on a key; shader naming used by 0010–0012 | off |
| `0010-d3d9-shaderOverride` | create a shader from `<folder>/<SHA-1 of the original>.dxso` instead | **`Div2Reborn_override\far`** |
| `0011-d3d9-postAA` | SMAA after the engine's tonemap draw, before the interface (`d3d9.postAA`, `postAAKey`) | **on, F11** |
| `0012-d3d9-postTAA` | TAA after SMAA | **on** |
| `0013-d3d9-gpuTimers` | GPU time of the light pre-pass and post AA in the log | off |
| `0014-d3d9-windowProcChain` | bug fix: keep the window-procedure chain when another subclass sits on top of DXVK's | always |
| `0015-d3d9-postAADebugViews` | the F11 key also cycles development views (edges, weights, shadow cascades, ...) | off |

**Shaders** (`shaders/`, needs Python 3 and the system `d3dcompiler_47.dll`):
`py -3 tools/shaders/build_overrides.py <game>\Data\Win32\Packed shaders out` → `out/far/*.dxso` (reads the original
shaders from the game's `MainDataStartup.dv2` to check the constant layout; nothing is written to the game) and
`py -3 tools/shaders/build_post.py shaders out-post` → SMAA/TAA passes and lookup textures. Both give the shipped files
byte for byte.

## Third-party files in the release

* `bin/d3d9.dll` — **altered** DXVK 3.1.1 (zlib license): official source + the patches in `dxvk/patches`.
  Not the official DXVK release. With all the options above off it behaves like official 3.1.1 apart from the
  window fix 0014; the official `x32/d3d9.dll` can also be used instead (then remove those lines from `dxvk.conf`;
  the lighting fix, SMAA and TAA are then gone).
* `bin/Div2Reborn_post/` — built from **[SMAA](https://github.com/iryoku/smaa)** (Jimenez et al., MIT;
  `shaders/third_party/smaa`).
* `Data/Win32/Packed/Reborn/Reborn_FX.dv2` — effect textures of the game (© Larian Studios), upscaled.
* `res/div2.ico` — the game's own icon (© Larian Studios), used as the window icon.

## Credits

* **Larian Studios** — Divinity II and all original assets.
* **[Div2Patch](https://github.com/BlanknameES/Div2Patch)** by BlanknameES — required, not included.
* **[DXVK](https://github.com/doitsujin/dxvk)** by Philip Rebohle and contributors — shipped with small patches (`dxvk/`).
* **[SMAA](https://github.com/iryoku/smaa)** by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez (MIT).
* **4x-UltraSharp** by Kim2091 (CC BY-NC-SA 4.0), run with Real-ESRGAN-ncnn-vulkan — used to upscale the textures.

## License

MIT (see `LICENSE`). Divinity II and all its assets are © Larian Studios; this is a non-commercial fan project.
