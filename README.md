# Batman Arkham Asylum Performance Fix

BmFix removes most stutters from Batman: Arkham Asylum Game of the Year Edition, the Steam version for PC. It is an unofficial fan project. Rocksteady, Warner Bros. and DC do not make or support it.

You copy BmFix into the game folder. BmFix does not change the game files. It works when you start the game from Steam, from a game launcher such as AsylumLauncher, or from a shortcut. It also works together with ReShade, DXVK, and other mods that use `d3d9.dll`.

The table shows the results of benchmark run. During a slow frame, the game runs below 120 frames per second (fps).

| | Slow frames | Slowest frame |
|---|---|---|
| Original game | 1491 | 69 ms |
| With BmFix, after the first session in the area | 1 | 11 ms |

## Install

1. Download `Batman-Arkham-Asylum-Performance-Fix-v1.0.zip` from [Releases](../../releases) and extract it.
2. Open the `Binaries` folder of the game. In Steam, right-click the game, select Manage > Browse local files, and open `Binaries`.
3. Copy `faultrep.dll` and the `BmFix` folder from the `Binaries` folder of the zip file into the `Binaries` folder of the game.
4. Start the game.

`faultrep.dll` goes in the same folder as `ShippingPC-BmGame.exe`. The `BmFix` folder contains the configuration, the logs, and the cache files.

To remove BmFix, delete `faultrep.dll` and the `BmFix` folder.

## What it fixes

The causes in the table were measured with a frame-time probe that records how long each frame takes and what the game does during it.

| Cause | Fix |
|---|---|
| The game looks for about 110 language files (`.int` files) that it does not have. Each time, it searches 4 folders on the game thread, which is the thread that runs the game logic. | BmFix gives the game a language folder, `BmFix\Localization`. It contains copies of the `.int` files of the game and empty files for the missing names. |
| The game sends a texture or a mesh buffer (3D model data) to the GPU in the first frame that draws it. | BmFix sends new managed textures (textures that the driver moves to the GPU) and large buffers to the GPU a few frames after the game creates them. It sends 4 at most in each frame. |
| The NVIDIA D3D9 driver compiles a shader (a small GPU program) the first time that the game draws with it. This causes pauses of 3–10 ms when new scenery comes into view. | BmFix records each shader combination that the game draws in `BmFixCombos.bin`. In later sessions, BmFix draws each recorded combination one time, off screen, while the game loads. |
| The game can spend up to 5 ms in each frame to show a streamed level. A streamed level is a part of the map that loads while you play. | BmFix lowers this time limit to 1 ms (`stream_ms`). |
| With PhysX on, the game cooks each cloth mesh (banners, flags) on the game thread when its level becomes visible. Cooking converts the mesh into the format of the physics engine. Each mesh takes 2–8 ms, up to about 25 ms in one frame, in every session. | BmFix keeps the cooked cloth in `BmFix\BmFixPhysX.bin`. From the second time, the game uses the kept data, which takes about 0.1 ms. |

The first time that you visit an area, the game can still stutter one time. During that visit, BmFix records the shaders and the cloth meshes of the area. In later sessions, the game compiles those shaders while it loads, and it gets the cloth from the cache.

## Configuration

To change the configuration, edit `BmFix\BmFix.ini` and restart the game. Each line sets one value:

- `hook`: Set to 0 to turn off all fixes inside the game.
- `preload`, `warm`: Set to 0 to turn off the preload or the shader warm-up.
- `stream_ms`: The time limit in each frame for streamed levels, in ms.
- `physx_cache`: Set to 0 to turn off the PhysX cloth cache. The cache works only with the tested PhysX 2.8.1 runtime. With a different PhysX build, the game cooks cloth as usual.
- `localization`: Set to 0 to turn off the fix for the language files.
- `trace`: Set to 1 to write each slow frame to `BmFix\logs\BmFixHook.log`.
- `dxvk`: Set to 1 to run the game on Vulkan with DXVK (https://github.com/doitsujin/dxvk, zlib license). DXVK is not included. To use it, put the 32-bit `d3d9.dll` of DXVK into `BmFix\dxvk` and rename it to `dxvk_d3d9.dll`. DXVK compiles shaders in the background, but in tests its slowest frames were slower than with native D3D9 and the warm-up.
- `[overrides]`: The lines of the engine configuration that BmFix replaces in the copy that the game reads. By default, the only line is `bInitializeShadersOnDemand=False`. With this line, the game creates its shaders when a level loads.

`BmFix\logs\BmFixHook.log` shows what BmFix applied each time that the game started.

## Requirements

- The Steam Game of the Year version of the game (`ShippingPC-BmGame.exe` 1.1). BmFix compares the exact bytes before it changes the engine code in memory. If the exe is a different version, BmFix does not make those changes.
- BmFix was built and measured on an NVIDIA RTX 4070 with native D3D9.

## Build

The source code is in `src/`. The files of the `BmFix` folder are in `package/`.

`src/hook/BmFixHook.c` is a 32-bit DLL that does not use the C runtime library. To build it, run `bash src/hook/build.sh`. The script uses the msys2 ucrt64 gcc (`-m32`) and `ld -m i386pe` from `C:\msys64`.
