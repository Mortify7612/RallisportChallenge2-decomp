# RalliSport Challenge 2 — running the recompiled build

Static recompilation of the Xbox title (NTSC, 0x4D530039) with the xboxrecomp
toolkit in `../xboxrecomp` (patched locally). Game files live in
`../RalliSport2`; each build directory has a `game` symlink to them.

## Linux

```sh
ninja -C rsc2/build && rsc2/run.sh
```

`run.sh` starts `build/rsc2` on the discrete NVIDIA GPU when the machine has
PRIME offload (`prime-run`, with an inherited `VK_ICD_FILENAMES` dropped);
`RSC2_GPU=igpu rsc2/run.sh` uses the integrated GPU. Running `build/rsc2`
directly works too and uses whatever Vulkan device the environment exposes.

Opens a window (F11 = fullscreen; closing it quits) with sound through SDL2.
Keyboard is pad 0:

| Key | Pad |
|---|---|
| Arrows | D-pad |
| Enter / Backspace | START / BACK |
| Z X A S | A B X Y |
| Q E | White / Black |
| 1 / 3 | Left / right trigger (brake / throttle) |
| Numpad 8 2 4 6 | Left stick |
| I J K L | Right stick |

What works: boot, intro movies with audio, title, profile create/save, career
menus, stage loading, and driving a race (SS1 Cooper Hill) with HUD and timer.

### Renderer

The NV2A is rendered with Vulkan 1.3 (`xboxrecomp/src/kernel/nv2a_vk.c`):
the title's vertex programs are translated to GLSL and compiled with
shaderc at runtime, the register combiners run as a fragment shader, and
the frame is presented straight to the window. It runs at a locked 60 fps
(the flip is paced to vblank like the console) at 2x internal resolution
(1280x960) on an RTX 5070 Ti; the integrated Radeon 610M manages ~30 fps at
1x. Needs Vulkan 1.3 with dynamic rendering; build needs `glslc`, the
Vulkan headers/loader and (for GPU vertex programs) `shaderc`.

- `RECOMP_VK_SCALE=N`: internal resolution multiplier (default 2; 1 for
  weak GPUs, 3 or 4 for a sharper picture).
- `RECOMP_VULKAN=0`: the old software rasteriser (slow, ~10 fps in a race).
- `RECOMP_VK_DEVICE=N`: pick a Vulkan device by index (listed at start).
- `RECOMP_VK_VSYNC=1`: FIFO presentation instead of mailbox.
- `RECOMP_FLIP_HZ=N`: flip pacing (default 60; 0 = unpaced).
- Compiled shaders are cached in `~/.cache/xboxrecomp/vk`
  (`RECOMP_VK_CACHE=dir`, `=0` off).

## Windows (cross-built on Linux, tested under Wine only)

The Vulkan renderer needs three things the MinGW sysroot lacks; they live in
`../third_party/mingw` (rebuild steps below): the Vulkan headers, an import
library for the system's `vulkan-1.dll`, and a static glslang.

```sh
TP=$PWD/third_party/mingw
cmake -S rsc2 -B rsc2/build-win -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=../xboxrecomp/cmake/mingw-w64-x86_64.cmake \
      -DVulkan_INCLUDE_DIR=$TP/include -DVulkan_LIBRARY=$TP/lib/libvulkan-1.a \
      -DXBOXRECOMP_GLSLANG_INCLUDE=$TP/include \
      -DXBOXRECOMP_GLSLANG_LIB=$TP/lib/libglslang.a \
      -DXBOXRECOMP_GLSLANG_RL_LIB=$TP/lib/libglslang-default-resource-limits.a \
      "-DXBOXRECOMP_GLSLANG_EXTRA_LIBS=$TP/lib/libSPIRV.a;$TP/lib/libMachineIndependent.a;$TP/lib/libOSDependent.a;$TP/lib/libGenericCodeGen.a;$TP/lib/libMachineIndependent.a;stdc++"
ninja -C rsc2/build-win
```

`rsc2/build-win/rsc2.exe` is self-contained (static MinGW runtime, glslang
linked in; needs only the GPU driver's `vulkan-1.dll`) and needs the game
folder next to it as `game`. Under Wine it renders with Vulkan on the host
GPU and reaches the menus at ~55 fps.

Known Windows issues:
- A profile saved by the Linux build is listed but "Unable to load".
- A second launch can stall rebuilding its Z: cache; delete
  `%LOCALAPPDATA%\xboxrecomp\Cache` between runs.

Rebuilding `third_party/mingw`: glslang from tag `vulkan-sdk-1.4.363.0`
(`third_party/src/glslang`) built with the same toolchain file and
`-DBUILD_SHARED_LIBS=OFF -DENABLE_OPT=OFF -DENABLE_HLSL=OFF
-DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=OFF`, installed there; the
headers are copies of `/usr/include/{vulkan,vk_video}`; the import library
is `x86_64-w64-mingw32-dlltool -d third_party/vulkan-1.def -l
third_party/mingw/lib/libvulkan-1.a`.

## Useful switches (environment variables)

Defaults are baked into `src/main.c`; `NAME=0` turns one off.

- `RECOMP_AUDIO=0`: no audio device. `RECOMP_AUDIO_WAV=out.wav` records
  the mix.
- `RECOMP_FB_WINDOW=0`: headless. `RECOMP_FB_DUMP=prefix` plus
  `RECOMP_FB_WINDOW_DUMP_EVERY=N` writes BMP frames.
- `RECOMP_PAD_SCRIPT=@file`: scripted input (`ms:button:holdms,...`; buttons
  include start, a, b, x, y, up, down, left, right, lt, rt).
- `RECOMP_RASTER_THREADS=N`: rasteriser threads (default: cores − 4, max 64).
- Vulkan diagnostics: `RECOMP_VK_STATS`, `RECOMP_VK_TEX_TRACE`,
  `RECOMP_VK_SURF_TRACE`, `RECOMP_VK_PRESENT_DUMP=file.bmp`
  (+`RECOMP_VK_PRESENT_DUMP_AT=frames`), `RECOMP_VK_CPU_VSH` (vertex
  programs on the CPU), `RECOMP_VK_PROG_DUMP`, `RECOMP_REPORT_TRACE`.
- Diagnostics: `RSC2_PROFILE=file` (+`RSC2_PROFILE_WALL=1`,
  `RSC2_PROFILE_DELAY=s`), `RECOMP_PIXEL_PROBE=x,y`, `RECOMP_BATCH_LOG`,
  `RECOMP_PROG_STATS`, `RECOMP_PROG_DUMP=hash`, `RECOMP_APU_STATS`,
  `RECOMP_ACK_STATS`, `RECOMP_WATCH=va`, `RSC2_FIBER_TRACE`.
