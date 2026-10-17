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

Opens a window (F11 or Alt+Enter = fullscreen; closing it quits) with sound through SDL2.
The keyboard is pad 0, alongside any real controller, and is laid out for
driving with the game's default controller preset (PRESET 1):

| Key | Race | Menus | Pad |
|---|---|---|---|
| Left / Right | Steer (eased) | Left / right | Left stick |
| Up / Down | Accelerate / brake | Up / down | Triggers + d-pad up/down |
| Space | E-brake | Accept | A |
| A / Z | Gear up / down | | B / X |
| C | Change camera | | Black |
| V | Look back | | Y |
| R | Car reset | | White |
| H | Horn | | Right stick click |
| Esc | Pause menu | Back | Start in a race, Back in menus |
| Enter | | Accept | Start |
| Backspace | | Back | Back |

Every key can be changed in the launcher (Controls, Key bindings), and
"Keyboard steering" sets how fast the steering keys reach full lock. The
arrows steer through the stick only: the game treats a held d-pad
left/right as instant full lock, which the d-pad rows on the bindings page
still offer. Esc knows a race from a menu because the game's menu input
handler (`0x001DBF90`, hooked in `rsc2/src/recomp_manual.c`) only runs while
a menu is up. The bindings reach the runtime as `RECOMP_KEYMAP`
(`id=vk[+vk];...`, see `xboxrecomp/include/xbox/keymap.h`) and the easing
as `RECOMP_KEY_STEER_MS`.

The launcher's **Add to Steam** button puts the game in the Steam library
as a non-Steam game (`run.sh` on Linux, the `.exe` on Windows), for the
account that logged in last. Quit Steam first: it rewrites
`userdata/<account>/config/shortcuts.vdf` when it closes. The previous file
is kept as `shortcuts.vdf.bak`.

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
- `RECOMP_FPS_OVERLAY=1`, or F3 in the window: an FPS counter in the
  corner. It shows the average, fastest and slowest frame time of the last
  second, and a graph of the last 160 frames against the frame-rate limit
  (the line). It's also in the launcher, and needs the Vulkan renderer.
- `RECOMP_GAMMA=0`: ignore the game's gamma ramp. The console's video DAC
  maps every pixel through the ramp the game sets; RalliSport Challenge 2's
  race ramp lifts and warms the shadows and midtones. It's applied when
  presenting, and to recordings. `RECOMP_GAMMA_TRACE=1` prints each ramp.
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
- `RECOMP_HDR=1`: HDR output, when the window offers an HDR format. On
  Wayland the game first asks the compositor for its own colour space
  (KWin: BT.2020, gamma 2.2, 0 to the screen's peak, white at the SDR
  brightness) and draws 10-bit frames in it, so KWin has nothing to
  convert or tonemap. The log says "HDR in the compositor's space". Paper
  white and peak then scale with the SDR brightness slider (203 nits is
  its white), and the peak stops at the screen's. Otherwise scRGB
  (16-bit linear) is used if available, else HDR10 (10-bit PQ, BT.2020);
  `RECOMP_HDR_FORMAT=match|scrgb|a2b10|a2r10` picks one. Turn HDR on in the
  system's display settings first; otherwise the game says so and stays SDR.
  The game draws in SDR, so its frame is mapped up:
  - its white sits at paper white, `RECOMP_HDR_PAPER` nits (default 203);
  - only the brightest part (linear luminance above `RECOMP_HDR_KNEE`,
    0.25) is stretched, up to `RECOMP_HDR_PEAK` nits (1000) for pure white.
    `RECOMP_HDR_STRENGTH` (0-2, default 1) scales the stretch;
  - the light the game adds (the sun's glare) is kept in a float buffer and
    goes up towards the peak, scaled by `RECOMP_HDR_GLOW` (1);
  - the HUD and menus stay at paper white, not stretched. The HUD is the
    draws without a depth test after the 3D scene. A frame with no 3D scene
    (menus, films) isn't stretched at all.
  - The launcher has HDR and HDR brightness (paper white).
  - `RECOMP_HDR_DUMP=<prefix>:<frame>[,<frame>...]` writes those frames as
    they would go to an HDR screen, in nits, to `<prefix>_<frame>.raw`. It
    works headless too, and needs no HDR screen.
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
  fullscreen on the monitor. F11 or Alt+Enter toggles it.

Known Windows issues:
- The menus run slower than on Linux (about 40 fps).
- The Z: cache (`%LOCALAPPDATA%\xboxrecomp\Cache`) is emptied at every start,
  because the game copies it again on each boot (`RECOMP_CACHE_FRESH=0`
  keeps it, and the second launch then stalls).

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

## Windows (native build with MSYS2)

This builds on Windows 11 with MSYS2's MINGW64 environment and needs no
`third_party/mingw`: MSYS2 provides SDL2, glslang and the Vulkan loader.
Running the result on real Windows is still untested (issue #4).

In the "MSYS2 MINGW64" shell (run `pacman -Syu` first if the install is
old; pacman refuses new packages against an outdated `gcc-libs`):

```sh
pacman -S --needed mingw-w64-x86_64-{gcc,cmake,ninja,python,SDL2,glslang,vulkan-headers,vulkan-loader,shaderc,spirv-tools,python-capstone,python-pefile,python-numpy,python-pytest}
python -m venv --system-site-packages .venv
```

Put the game in `RalliSport2/` and its `default.xbe` in
`xboxrecomp/game_files/`, as on Linux. `regen.sh` also needs the XBE's
analysis JSON, which it doesn't write itself:

```sh
(cd xboxrecomp && ../.venv/bin/python -m tools.xbe_parser game_files/default.xbe \
    --json game_files/default_analysis.json)
rsc2/regen.sh
```

MSYS2's headers only declare `FileDispositionInfoEx` (used by
`kernel_file.c`) for Windows 10 1607 and later, so set the target version:

```sh
cmake -S rsc2 -B rsc2/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      "-DCMAKE_C_FLAGS=-D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000007"
ninja -C rsc2/build
cmd //c "mklink /J rsc2\\build\\game RalliSport2"
```

This links glslang, SPIRV-Tools and the GCC runtime as DLLs. To run
outside the MSYS2 shell, copy them next to the program:

```sh
ldd rsc2/build/rsc2.exe | awk '/\/mingw64\//{print $3}' | sort -u | xargs -I{} cp {} rsc2/build/
```

## ARM64 test build (Linux on aarch64, run under qemu)

This build is groundwork for an Android port, not a release. It lets the
ARM-specific parts be built and tested on this PC. Those parts are the
fault-trapped device registers (decoded as AArch64 instructions), the signal
frame layout and the absence of a trap flag. The build uses clang and lld
against an Arch Linux ARM sysroot and runs under `qemu-aarch64`, about 4×
slower than native x86.

```sh
python3 -I rsc2/tools/arm64/make_sysroot.py third_party/sysroot-aarch64
cmake -S rsc2 -B rsc2/build-arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/rsc2/cmake/aarch64-linux.cmake \
      -DRSC2_SYSROOT=$PWD/third_party/sysroot-aarch64
ninja -C rsc2/build-arm64
ln -s ../../RalliSport2 rsc2/build-arm64/game
rsc2/tools/arm64/run_qemu.sh RECOMP_FB_WINDOW=0 RECOMP_VULKAN=0 RECOMP_AUDIO=0 \
      RSC2_INPUT_REPLAY=../../work/my_run2.xi RECOMP_FB_DUMP_AT=/tmp/arm:600,1200
```

`make_sysroot.py` needs `bsdtar` and network access; the sysroot is about
900 MB. The toolchain file needs `clang`, `lld` and the host's `glslc`.

What has been checked: headless, with the software renderer, the ARM build
plays the intro movie and goes through the menus and car selection into a
race. The USB pads, APU, AC'97 and GPU interrupt registers are all serviced
by the AArch64 decoder. Up to the race the frames match the x86 build to
within the noise between two x86 runs. Past that point not even two x86 runs
agree, because the replay is not bit-exact.

The Vulkan renderer also runs, on Mesa's software Vulkan driver from the
sysroot (`VK_ICD_FILENAMES=$PWD/third_party/sysroot-aarch64/usr/share/vulkan/icd.d/lvp_icd.json`).
It is slow, about one frame a second, but menus and races render as they
do on the PC, including the menu backgrounds that the software renderer
leaves black on every platform.

What qemu cannot check: x86 orders memory accesses strictly and ARM does
not. Under qemu the ARM code runs on x86 hardware and inherits its ordering,
so a race between the game's threads only shows on a real ARM CPU. The ARM
build therefore defaults to `RECOMP_GUEST_LOCK=1`, which runs one guest
thread at a time, as on the Xbox's single CPU. With the lock on, the x86
build plays the same replay through to the race no slower. `RECOMP_GUEST_LOCK=0`
turns it off.

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
- Frame pacing: `RECOMP_FRAME_TIMES=file` logs, per frame, when the game
  starts it (`G`), the flip (`F`), the present (`P`/`Q`), the pacer
  (`S`), the wait for the GPU (`W`/`w`) and, where the driver has
  VK_KHR_present_wait, when it reached the screen (`D`). With
  `RECOMP_VK_STATS` it also logs each frame's GPU time (`g`).
  `RECOMP_VK_PRESENT_EARLY=1` presents at the flip instead of after the
  pacer (the old timing); `RECOMP_VK_SC_IMAGES=n` sets the swapchain size.
- Diagnostics: `RSC2_PROFILE=file` (+`RSC2_PROFILE_WALL=1`,
  `RSC2_PROFILE_DELAY=s`), `RECOMP_PIXEL_PROBE=x,y`, `RECOMP_BATCH_LOG`,
  `RECOMP_PROG_STATS`, `RECOMP_PROG_DUMP=hash`, `RECOMP_APU_STATS`,
  `RECOMP_ACK_STATS`, `RECOMP_WATCH=va`, `RSC2_FIBER_TRACE`.
