# RalliSport Challenge 2 — static recompilation

A native PC build of *RalliSport Challenge 2* (Xbox, NTSC, title `0x4D530039`),
made by statically recompiling the original executable with the
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit. Linux first;
the Windows build is cross-compiled and so far tested under Wine.

**No game data is included.** You need your own copy of the game. The lifted
C code is generated locally from your `default.xbe` and is not part of this
repository either.

## Demo

[![RalliSport Challenge 2 running natively on PC (video)](https://img.youtube.com/vi/hVSkDmrpqtg/maxresdefault.jpg)](https://youtu.be/hVSkDmrpqtg)

## Prebuilt binaries

[Releases](https://github.com/Mortify7612/RallisportChallenge2-decomp/releases)
have ready-built Linux (x86-64) and Windows (x64) packages. They contain the
compiled program only, with the code translated from the game's executable
built in, but no game data. Put your game files in a `game` folder next to
the program, then start `run.sh` (Linux) or `rsc2.exe` (Windows).

- The Linux binary is built on an up-to-date Arch-based system and needs
  glibc 2.43+, SDL2, the Vulkan loader, glslang 16, SPIRV-Tools, libepoxy and
  OpenSSL 3. On other distributions, build from source.
- The Windows binary only needs a GPU driver with Vulkan 1.3.

## What works

- Boot, intro movies, menus, profiles, career, stage loading and racing.
- Vulkan renderer at up to 6x internal resolution, with the game's own
  16:9 widescreen mode, and frame rates above 60 (the game keeps time by the
  clock).
- Optional extras the original never had: MSAA, anisotropic filtering and
  ambient occlusion.
- Sound through SDL2: per-voice resampling, speaker mixdown, and the game's
  per-environment I3DL2 reverb (tunnels, forest, valleys, in the car).
- Keyboard and SDL game controllers, with rumble.
- A pre-game launcher for resolution, aspect ratio, GPU, display mode, frame
  rate limit, audio and controls.

## Layout

| Path | What |
|---|---|
| `xboxrecomp/` | The recompilation toolkit (v0.13.1, MIT) with this project's runtime work: Linux platform layer, NV2A Vulkan renderer, audio, input, kernel fixes. |
| `rsc2/` | The game project: entry point, hand-written overrides, launcher, build files, tools. |
| `rsc2/config/` | Function seeds and trace lists for the recompiler. |
| `addr2func.py` | Maps guest addresses to recompiled functions (debugging). |

## Building (Linux)

Needs CMake 3.20+, Ninja, a C11 compiler (GCC or Clang), Python 3, SDL2
2.0.18+, OpenSSL, libepoxy, the Vulkan headers and loader, `glslc` and
glslang.

1. Put your game files in `RalliSport2/` and copy its `default.xbe` to
   `xboxrecomp/game_files/default.xbe`.
2. Make the Python environment the recompiler runs in:
   ```sh
   python3 -m venv .venv
   .venv/bin/pip install capstone pefile numpy pytest
   ```
3. Generate the lifted C from your XBE (writes `rsc2/src/recomp/`):
   ```sh
   rsc2/regen.sh
   ```
4. Configure and build:
   ```sh
   cmake -S rsc2 -B rsc2/build -G Ninja
   ln -s ../../RalliSport2 rsc2/build/game
   ninja -C rsc2/build
   ```
5. Run:
   ```sh
   rsc2/run.sh
   ```

The launcher opens first; its settings are saved to
`~/.config/rsc2/settings.ini`. `rsc2/run.sh --no-launcher` skips it,
`--launcher` brings it back when "Show at startup" is off. Any `RECOMP_*`
variable set by hand overrides the saved setting.

[`rsc2/RUNNING.md`](rsc2/RUNNING.md) has the keyboard map, the Windows
cross-build and more of the runtime switches.

## Credits and licences

- xboxrecomp by sp00nz and contributors — MIT (`xboxrecomp/LICENSE`).
- Noto Sans — SIL Open Font License 1.1 (`rsc2/assets/fonts/OFL.txt`).
- stb_truetype by Sean Barrett — public domain / MIT.

*RalliSport Challenge 2* is a trademark of its respective owners. This
project contains no game assets. In the source tree, the code translated from
the executable is generated on your machine. There is one exception: the XDK
library's `XInputGetState`, whose translated body `rsc2/src/rsc2_overrides.c`
keeps so input can be recorded and replayed. The prebuilt release binaries
contain the translated code compiled in, and are meant for owners of the
game.
