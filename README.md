# Batman Arkham Asylum Performance Fix

BmFix: a stutter fix for Batman: Arkham Asylum GOTY (Steam, PC). It is an unofficial fan project, not affiliated with Rocksteady, Warner Bros. or DC.

A drop-in fix for the traversal and camera-turn stutters in Batman: Arkham Asylum GOTY. Copy it into the game
folder and play: no game files are changed. It works when you start
the game from Steam, from the game's launcher (including AsylumLauncher), or from a shortcut. It also works
alongside ReShade, DXVK and other `d3d9.dll` mods.

| | frames slower than 8.33 ms | worst frame |
|---|---|---|
| original game | 1491 | 69 ms |
| with BmFix, from the second session in an area | 1 | 11 ms |

## Install

1. Download `Batman-Arkham-Asylum-Performance-Fix-v1.0.zip` from [Releases](../../releases) and unzip it.
2. Open the game's `Binaries` folder. In Steam: right-click the game > Manage > Browse local files > `Binaries`.
3. Copy into it the two items from the zip's `Binaries` folder:
   - `faultrep.dll`, which goes next to `ShippingPC-BmGame.exe`;
   - the `BmFix` folder, which holds the settings, logs and data.
4. Play.

To uninstall, delete `faultrep.dll` and the `BmFix` folder.

## What it fixes

| cause (measured with a frame-time probe) | fix |
|---|---|
| The game looks up ~110 localization files it never shipped, re-probing 4 folders every time on the game thread | One localization folder (`BmFix\Localization`) holding copies of the game's `.int` files plus empty stubs. |
| A texture or mesh buffer is uploaded to the GPU in the frame that first draws it | New managed textures and large buffers are pre-loaded a few frames after creation, at most 4 per frame. |
| The NVIDIA D3D9 driver compiles a shader the first time it is drawn (3–10 ms bursts when new scenery comes into view) | Every shader combination the game draws is recorded in `BmFixCombos.bin` and drawn once, off screen, while the game loads in later sessions. |
| Making a streamed level visible may take up to 5 ms per frame | That budget is lowered to 1 ms (`stream_ms`). |
| With PhysX on, every cloth mesh (banners, flags) is cooked on the game thread when its level becomes visible, 2–8 ms each and up to ~25 ms per frame, in every session | The cooked cloth is cached in `BmFix\BmFixPhysX.bin` and reused from the second time onwards (about 0.1 ms instead). |

The first time you visit an area it can still stutter once, while its shaders and cloth meshes are compiled and
recorded. On later sessions those shaders are compiled while the game loads, and the cloth comes from the cache. Both
caches start empty and fill as you play: they hold data built from the game's own files, so neither is
redistributed.

With PhysX on, the camera-spin test after a load had 6–10 frames over 8.33 ms, the worst at 25–34 ms. With the cloth
cache filled, it had 5–6 frames over 8.33 ms, the worst at 13 ms. The rest is the GPU PhysX simulation taking longer
for the first few frames after new cloth starts. With PhysX off, the worst frame was 10.6 ms.

## Settings

Edit `BmFix\BmFix.ini`, then restart the game.

- `hook`, `preload`, `warm`: set to 0 to turn an in-game fix off.
- `stream_ms`: the level-streaming budget, in ms.
- `physx_cache`: set to 0 to turn the PhysX cloth cache off. It is only active with the tested PhysX 2.8.1 runtime;
  with any other build, cloth is cooked as usual.
- `localization`: set to 0 to turn the localization fix off.
- `trace=1` logs every frame slower than 8.33 ms to `BmFix\logs\BmFixHook.log`.
- `dxvk=1` runs the game on Vulkan with DXVK. DXVK is not included: put its 32-bit `d3d9.dll` into `BmFix\dxvk` as `dxvk_d3d9.dll`. DXVK compiles shaders in the background, but in
  testing its worst frames were worse than native D3D9 with the warm-up.
- `[overrides]` lists the engine ini lines to replace in the copy the game reads; by default only
  `bInitializeShadersOnDemand=False` (shaders created at load). The mod does not change the frame cap. To set one here, add
  `MaxSmoothedFrameRate=180` (the game's own limiter), or `bSmoothFrameRate=False` to remove the limiter.

`BmFix\logs\BmFixHook.log` shows what was applied at each launch.

## Requirements

- The Steam GOTY version of the game (`ShippingPC-BmGame.exe` 1.1). The engine patches check the exact bytes
  first and skip themselves on any other exe.
- Windows 10 or 11.
- Built and measured on an NVIDIA RTX 4070 with native D3D9.

## How it works

`faultrep.dll` is the name of the Windows Error Reporting DLL. The game imports it only to report crashes, and
Windows loads it from the game folder first. BmFix's copy forwards every export to the real system
`faultrep.dll`. When the game loads it, before the game's code starts, BmFix:

- wraps the game's `Direct3DCreate9`, whatever `d3d9.dll` it is bound to, including another mod's;
- hooks the D3D9 device for the preload and the warm-up;
- patches the level-streaming time budget in memory, after checking the instruction bytes;
- adds `-ENGINEINI=<copy>` the first time the game reads its command line, unless the command line already sets one.

Other mods use `d3d9.dll`, `dinput8.dll`, `dxgi.dll` or `version.dll`, never `faultrep.dll`, so they stack.

## Build

The source is in `src/`, and the files of the `BmFix` folder are in `package/`.

- `hook/BmFixHook.c` is a freestanding 32-bit DLL with no CRT. Build it with `bash hook/build.sh`, using the
  msys2 ucrt64 gcc (`-m32`) and `ld -m i386pe`.

## Credits

- DXVK: https://github.com/doitsujin/dxvk (zlib license). Only used with `dxvk=1`.
