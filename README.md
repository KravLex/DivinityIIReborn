===== SHORT SUMMARY (Nexus "Brief overview" field, < 350 chars) =====

Overhaul for Divinity II: Developer's Cut (requires Div2Patch): a spoiler-free mind-reading advisor, 4x AI-upscaled textures, HD normal maps and icons, fixed soft shadows, 8K shadow maps, smooth bloom and sun rays, DXVK, correct ultrawide menus, intro skip, engine fixes. Saves untouched.


===== DESCRIPTION (paste into the Nexus description editor, BBCode) =====

[center][size=6][b][color=#e0b050]Divinity II Reborn[/color][/b][/size]
[size=3][i]A drop-in visual, engine and quality-of-life overhaul for Divinity II: Developer's Cut[/i][/size][/center]

[line]

[size=4][b][color=#e0b050]What is it?[/color][/b][/size]
Divinity II Reborn makes the 2009 classic look and play better on modern PCs, especially on high-resolution and ultrawide monitors. It does not change gameplay, balance, quests or your saves.

The mod has three parts:
[list]
[*][b]Div2Fix[/b] — a small proxy DLL (bin\dinput8.dll) with engine fixes and new features. Before every change it checks that the game code is exactly what it expects; only then does it patch the game in memory. If the code is different (another game version), that change is skipped and the reason is written to [i]bin\Div2Fix.log[/i]. Every feature has its own switch in [b]bin\Div2Fix.ini[/b].
[*][b]DXVK[/b] — renders the game through Vulkan instead of DirectX 9.
[*][b]HD Texture Pack[/b] (separate download) — AI-upscaled textures, normal maps, icons and interface art.
[/list]
[b]Requires [url=https://github.com/BlanknameES/Div2Patch]Div2Patch[/url][/b] (free fan patch by BlanknameES) — install it first. Div2Fix is built for the game version that Div2Patch installs (v1.4.700.56). On any other version the game still runs, but most of the mod's features switch themselves off, and a message at start-up tells you so.

[b]This mod does not modify any game file:[/b] Divinity2.exe and the .dv2 archives stay as they are (only Div2Patch replaces the exe with its own build), and nothing is written into saves. The texture pack sits in the game's own patch-archive slots, which the engine loads on top of the original data. One script removes everything.

[line]

[size=5][b][color=#e0b050]★ New: Mind-Reading Advisor[/color][/b][/size]
Mind reading costs experience, and in the original game you never know whether it is worth it. Sometimes you get skill points, stat points or an item; sometimes a trader discount or a new dialogue option; very often just a character's random thoughts. Since every NPC can only be read once, players either waste a lot of XP or reach for a wiki and spoil the story.

The advisor solves this without spoilers. When the game shows the confirmation [i]"Mind reading will cost you N experience"[/i], Div2Fix adds one more line underneath with a verdict:

[list]
[*][color=#80d080][b]"You may learn something useful"[/b][/color] — the read gives a tangible reward: skill points, dragon skill points, stat points, an item, a new skill or skill level, experience, gold or a level.
[*][color=#e0d060][b]"You may learn something, but not much"[/b][/color] — a smaller benefit: a trader discount, unlocking a trader, a new dialogue option or quest information.
[*][color=#b0b0b0][b]"Looks like just thoughts here"[/b][/color] — the read changes nothing in the game. Save your XP.
[*][b]"Unknown what this will give"[/b] — the data does not allow a reliable verdict.
[/list]

[b]How it works[/b]
[list]
[*]The verdicts are not guesswork or wiki copies. They were extracted from the game's own data: every [MindRead] branch of every dialogue was traced, recording which flags it sets and where those flags are checked, and the episode scripts (Osiris story) were decompiled to find what actually rewards the read: skill points, stat points, items, discounts and so on.
[*]Data for [b]337 mind-readable characters[/b]: 231 in Ego Draconis and 106 in Flames of Vengeance.
[*]The line only says [i]what kind[/i] of reward is behind the read, never what exactly you will learn. No names, no plot, no quest solutions.
[*]It appears in the game's own popup, in the game's language: English, German, French, Spanish, Italian, Polish or Russian.
[*]The advisor only reads data. The XP cost, the rewards and the read itself are exactly as in the original game.
[*]Settings: [i][MindRead] Enable=1[/i]. With [i]Show=0[/i] the verdict is only written to the log, not shown in game.
[/list]

[line]

[size=5][b][color=#e0b050]Graphics[/color][/b][/size]

[b]Fixed soft shadows[/b] [i](ShadowSoftness=1)[/i]
The sun's shadows are variance shadow maps: the map stores the average depth and its spread, and a blur turns the spread into a soft penumbra. The game's blur has a bug. Its second pass throws away the spread and squares the already blurred depth instead, so the spread becomes zero and every shadow edge becomes hard. On large monitors you see stair-steps, and swaying foliage makes the edges flicker. Div2Fix recognises the game's blur shader and swaps the faulty pass for a corrected copy that blurs both values. Shadows now have soft, stable edges and keep their exact shape, with no swelling or holes. 1 = the game's blur width; 1.5 or 2 = softer; 0 = original behaviour.

[b]8K shadow maps and longer shadow distance[/b] [i](ShadowMapSize=8192, ShadowDistance=300)[/i]
With shadow quality "High" the sun's shadow map is 8192x8192 instead of 4096 (twice as sharp), and shadows reach 300 instead of 200 game metres. The shadow fix above keeps the larger map smooth.

[b]Shadows from all objects in every cascade[/b] [i](ShadowAllObjects=1)[/i]
In the original some object types are left out of the distant shadow cascades, so distant scenery loses its shadows. Now every object casts shadows in every cascade.

[b]3x object detail and draw distance[/b] [i](LODDistanceMultiplier=3)[/i]
Detailed models and distant objects stay on screen three times farther, so there is much less pop-in. Range 1–8.

[b]Smooth bloom and sun rays[/b] [i](BloomScale=2)[/i]
The glow around bright areas was computed at 1/8 of the screen resolution and the sun rays at 1/4, then stretched to full screen. On a 1440p or ultrawide monitor that is only 430x180 pixels, which shows as square blotches in the glow and blocky rays. Div2Fix creates these buffers twice as large and adjusts the draw area for the sun rays. Same look, without the blocks.

[b]4x AI-upscaled textures[/b] (HD pack)
1,516 colour textures (characters, faces, armour, weapons, items, buildings, nature) were upscaled 4x per side, up to 4096x4096, with the 4x-UltraSharp model. For hair, beards and foliage the transparency mask was upscaled too, so strands and leaves are finer. All mip-maps were rebuilt so hair and leaves don't thin out at a distance. Every texture was checked automatically after upscaling and broken results were redone.

[b]HD normal maps and material layers[/b] (HD pack)
1,225 normal maps and 1,508 extra material layers (specular, glow and detail maps) were upscaled 2x. Fine surface detail was taken from the upscaled colour textures, so the relief matches the new textures.

[b]HD icons, interface and world map[/b] (HD pack + Div2Fix)
966 icons (items, skills, conditions, achievements, map markers, cursor) and 227 interface images, including the world map, were upscaled 4x. The game normally draws a texture at its real pixel size, so 4x icons would show only their top-left corner. Div2Fix makes the game draw them at the original on-screen size, just sharper. [i](Icons Enable=1)[/i]

[b]Fixed NPC clothing transparency[/b] (HD pack)
Some NPC clothing textures had broken alpha (holes or missing transparency). They were repaired.

[b]DXVK (Vulkan)[/b]
The game is rendered through Vulkan. Frame pacing is smoother, and the textures stop using the game's tight 32-bit memory, which is what makes the HD pack possible.

[b]ReShade presets[/b] (optional)
Four presets — Off, Recommended, Clean, Vivid — for sharpening, SMAA and colour. Installation notes are in [i]_Div2Reborn\ReShade[/i].

[line]

[size=5][b][color=#e0b050]Fixes[/color][/b][/size]

[b]Ultrawide menus[/b] [i](UIAspect)[/i]
On 21:9 and 32:9 screens the original stretches every menu across the whole width, and mouse clicks miss the buttons. Now the inventory, skills, map, quest log, dialogues, trade, character creation and other menus are shown in their intended 16:9 proportions in the centre, and clicks land correctly. HUD, cursor, damage numbers and enemy labels stay full-width. The list of menus is configurable.

[b]Key names in hints with non-English layouts[/b] [i](KeyNames)[/i]
With a Russian (or other non-Latin) keyboard layout, tutorial and action hints showed "[(?)]" instead of key names. Key names are now taken from the English layout.

[b]Non-Latin text on map markers[/b] [i](UIText)[/i]
Labels you type on world-map flags were shown as boxes when they weren't Latin, e.g. Cyrillic. The engine passed them to the interface in the wrong encoding, in two different ways. Both cases are now fixed for ANSI and UTF-8 text; correct text is never touched.

[b]Switching keyboard layout in game[/b] [i](KeyboardShared=1)[/i]
In the original, Alt+Shift, Ctrl+Shift and Win+Space did nothing while the game had focus, so you couldn't type a Cyrillic flag label. Now the layout switches in game as usual.

[b]Window icon[/b] [i](Icon=1)[/i]
The game window and taskbar button showed a blank icon. Now they show the game's icon, the same as the Steam shortcut.

[b]Strafe left + right[/b] [i](StrafeBothCancel=1)[/i]
Holding both strafe keys made the hero walk left. Now the hero stands still, and releasing either key immediately moves the other way. Covers the default keys A/D and the arrow keys; rebound keys behave as in the original.

[line]

[size=5][b][color=#e0b050]Quality of life[/color][/b][/size]

[b]Intro skip[/b] [i](Intro Skip=1)[/i]
The logo video at start-up is skipped, so the main menu opens right away. Story cutscenes and skill videos are not affected. No game file is edited, and it works with or without Div2Patch.

[b]Wider camera zoom[/b] [i](Camera)[/i]
Div2Patch adds mouse-wheel zoom with 10 steps out. Div2Fix extends that to 30 steps out (up to +7.5 units of distance) and 10 in. The zoom step (ZoomStep, 0.25) and the dragon-form camera distance (DragonDistance, 7.0) can be changed too. Div2Patch's files are not modified; the values are changed in memory.

[b]Frame-time recorder[/b] [i](Diagnostics, off by default)[/i]
Press F11 to start and stop recording. You get a frame-pacing report (average, 1% low, spikes) and a CSV of every frame, which is useful for tuning settings.

[line]

[size=4][b][color=#e0b050]Requirements[/color][/b][/size]
[list]
[*]Divinity II: Developer's Cut (Steam). The GOG version is untested.
[*][b][url=https://github.com/BlanknameES/Div2Patch]Div2Patch[/url] by BlanknameES — required.[/b] It also brings its own fixes: FPS limiter without camera stutter, high-FPS rotation fix, borderless window, keybind fixes and mouse-wheel zoom.
[*]A GPU with Vulkan 1.3 (NVIDIA GTX 900 or newer, AMD RX 400 or newer, Intel Arc/Xe) and an up-to-date driver.
[*]HD Texture Pack: about 17 GB of disk space.
[/list]

[size=4][b][color=#e0b050]Installation[/color][/b][/size]
[list=1]
[*]Install [url=https://github.com/BlanknameES/Div2Patch]Div2Patch[/url] (run its setup and point it to the game folder).
[*]Open the game folder: Steam → right-click the game → Manage → Browse local files. It's the folder with [i]bin[/i] and [i]Data[/i].
[*]Extract [b]DivinityIIReborn_v1.3.zip[/b] there and allow folders to be merged.
[*]Optional: extract the [b]HD Texture Pack[/b] to the same folder.
[*]Play.
[/list]
Settings: [b]bin\Div2Fix.ini[/b]. Every option is commented and can be switched off.

[size=4][b][color=#e0b050]Uninstallation[/color][/b][/size]
Close the game and run [b]_Div2Reborn\uninstall.cmd[/b]. It removes the mod's files from [i]bin[/i] and, after asking you, the texture-pack archives. To remove it by hand, delete [i]bin\dinput8.dll, Div2Fix.ini, Div2Fix_mindread.txt, d3d9.dll, dxvk.conf[/i] and [i]Data\Win32\Packed\Patch*.dv2, DKS_Patch*.dv2, FOV_Patch*.dv2[/i].

[line]

[size=4][b][color=#e0b050]Compatibility[/color][/b][/size]
[list]
[*][color=#80c080]Made for:[/color] Div2Patch 0.97. [color=#80c080]Compatible:[/color] ReShade, alt-tab, borderless window and exclusive fullscreen.
[*][color=#e08080]Do not use with "Div2Patch Enhancer":[/color] it also changes LOD and shadows. The draw-distance multipliers would stack (x12), and Div2Fix would skip its shadow changes. Plain Div2Patch is enough.
[*][color=#e08080]Other texture mods[/color] that use Patch*.dv2, DKS_Patch*.dv2 or FOV_Patch*.dv2 would overwrite the texture pack, or the pack would overwrite them.
[/list]

[size=4][b][color=#e0b050]Performance (tested)[/color][/b][/size]
Windows 11, RTX 3080 Ti, 3440x1440 at 190 Hz, Steam Developer's Cut with Div2Patch 0.97, full HD pack and all graphics options on. Game memory was about 1.5 GB (the limit is 4 GB), at about 160 FPS on average.
Not tested: AMD and Intel GPUs, the GOG version.

[size=4][b][color=#e0b050]Known issues[/color][/b][/size]
[list]
[*]Short hitches the first time you enter a new area while the large textures stream in; fewer on later visits.
[*]Hair still uses the 2009 models and shaders. It is cleaner and denser, but not modern.
[*]Key names in hints come from the English layout, so it must be installed in Windows.
[*]Black screen, or the game does not start: delete [i]bin\d3d9.dll[/i] and [i]bin\dxvk.conf[/i]. The game goes back to DirectX 9, but the HD textures may then not fit into memory.
[*]On a weak GPU, set BloomScale=1 and ShadowSoftness=0 (original behaviour).
[/list]

[line]

[size=4][b][color=#e0b050]Changelog[/color][/b][/size]
[b]1.3[/b]
[list]
[*]New: mind-reading advisor, soft-shadow fix, bloom and sun rays at twice the resolution, HD icons at the correct size, HD interface and world map, intro skip, non-Latin map-marker text, in-game keyboard-layout switching, window icon, strafe fix, wider camera zoom with configurable step and dragon distance.
[*]HD pack: normal maps and material layers (2x), icons and interface (4x), repaired NPC clothing transparency.
[*]New uninstaller.
[/list]
[b]1.2 and earlier[/b] — ultrawide menus, key-hint fix, 8K shadow maps, shadow distance, shadows from all objects, 3x LOD, frame-time recorder, DXVK, HD texture pack (colour textures).

[size=4][b][color=#e0b050]Credits and permissions[/color][/b][/size]
[list]
[*][b]Larian Studios[/b] — Divinity II and all original assets. This is a non-commercial fan mod.
[*][b]DXVK[/b] by Philip Rebohle (doitsujin) and contributors — zlib license (licence text included).
[*][b]4x-UltraSharp[/b] upscaling model by Kim2091 — CC BY-NC-SA 4.0. Upscaling was done with Real-ESRGAN-ncnn-vulkan (xinntao).
[*][b]Div2Patch[/b] by BlanknameES — required, not included. Thanks for keeping this game alive.
[*][b]ReShade[/b] by crosire — not included; presets only.
[*]Div2Fix — MIT license.
[/list]
Banner: official Divinity II: Developer's Cut artwork and logo © Larian Studios.
