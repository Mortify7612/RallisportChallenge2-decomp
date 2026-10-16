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
- `RECOMP_VK_MSAA=2|4|8`: multisample anti-aliasing. Applies to the 640-wide
  scene targets; each pass resolves into the image everything else reads.
- `RECOMP_VK_ANISO=N`: anisotropic filtering, up to 16. Used where the game
  already filters linearly with mipmaps.
- `RECOMP_VK_AO=1`: screen-space ambient occlusion. It's worked out from the
  game's w-buffer and applied to the shown scene before the HUD.
  - Tuning: `RECOMP_VK_AO_RADIUS` (default 1), `_INTENSITY` (0.7), `_FOV`
    (50, vertical), `_NEAR` (8: fades in past the car), `_FAR` (150).
  - `RECOMP_VK_AO_SHOW=1` shows the occlusion term on its own.
- The launcher sets these three from its Graphics section. Its default is
  16x anisotropic, with MSAA and AO off.
- Compiled shaders are cached in `~/.cache/xboxrecomp/vk`
  (`RECOMP_VK_CACHE=dir`, `=0` off).

## Windows (cross-built on Linux, tested under Wine only)

The MinGW sysroot is missing four things the Windows build needs. They live in
`../third_party/mingw` (rebuild steps below):

- the Vulkan headers
- an import library for the system's `vulkan-1.dll`
- a static glslang
- a static SDL2, for the launcher window

```sh
TP=$PWD/third_party/mingw
cmake -S rsc2 -B rsc2/build-win -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=../xboxrecomp/cmake/mingw-w64-x86_64.cmake \
      -DVulkan_INCLUDE_DIR=$TP/include -DVulkan_LIBRARY=$TP/lib/libvulkan-1.a \
      -DXBOXRECOMP_GLSLANG_INCLUDE=$TP/include \
      -DXBOXRECOMP_GLSLANG_LIB=$TP/lib/libglslang.a \
      -DXBOXRECOMP_GLSLANG_RL_LIB=$TP/lib/libglslang-default-resource-limits.a \
      "-DXBOXRECOMP_GLSLANG_EXTRA_LIBS=$TP/lib/libSPIRV.a;$TP/lib/libMachineIndependent.a;$TP/lib/libOSDependent.a;$TP/lib/libGenericCodeGen.a;$TP/lib/libMachineIndependent.a;stdc++" \
      -DSDL2_DIR=$TP/lib/cmake/SDL2
ninja -C rsc2/build-win
```

`rsc2/build-win/rsc2.exe` is self-contained. It uses a static MinGW runtime
with glslang and SDL2 linked in, and needs only the GPU driver's
`vulkan-1.dll`. Put the game folder next to it as `game`. Under Wine it
renders with Vulkan on the host GPU and reaches the menus at about 55 fps.

On Windows, as on Linux:

- The launcher comes first and saves to `%APPDATA%\rsc2\settings.ini`.
  Without `-DSDL2_DIR` the build still works, but it has no launcher window;
  the saved settings still apply.
- 16:9 opens a 1280x720 window. The window size scales with the display's
  DPI setting.
- The display mode setting (`RECOMP_FULLSCREEN=1`) gives borderless
  fullscreen on the monitor. F11 toggles it.

Known Windows issues:
- A profile saved by the Linux build is listed but "Unable to load".
- A second launch can stall rebuilding its Z: cache; delete
  `%LOCALAPPDATA%\xboxrecomp\Cache` between runs.

Rebuilding `third_party/mingw`:

- **glslang:** tag `vulkan-sdk-1.4.363.0` (`third_party/src/glslang`).
  Build it with the same toolchain file and these options, then install it
  there:
  `-DBUILD_SHARED_LIBS=OFF -DENABLE_OPT=OFF -DENABLE_HLSL=OFF -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=OFF`
- **SDL2:** release 2.32.10 (`SDL2-2.32.10.tar.gz` from libsdl-org/SDL on
  GitHub). Build it with the same toolchain file and these options:
  `-DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DCMAKE_INSTALL_PREFIX=third_party/mingw`
  then run `ninja install`.
- **Vulkan headers:** copies of `/usr/include/{vulkan,vk_video}`.
- **Vulkan import library:** generated from those headers (core 1.0–1.3
  plus the Win32 surface and swapchain functions):
  ```sh
  python3 rsc2/tools/mingw_vulkan_def.py third_party/mingw/include third_party/vulkan-1.def
  x86_64-w64-mingw32-dlltool -d third_party/vulkan-1.def -l third_party/mingw/lib/libvulkan-1.a
  ```

## Useful switches (environment variables)

Defaults are baked into `src/main.c`; `NAME=0` turns one off.

- `RECOMP_AUDIO=0`: no audio device. `RECOMP_AUDIO_WAV=out.wav` records
  the mix.
- `RECOMP_APU_REVERB=x`: reverb level. 0.3, the launcher's Normal, is the
  game's own level; 0 turns it off. The reverb follows the I3DL2 listener
  the game sets as it moves between environments.
  - `RECOMP_APU_I3DL2_TRACE=1` prints each listener the game sets.
  - `RECOMP_APU_REVERB_STATS=1` prints the dry, send and wet levels once a
    second.
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
