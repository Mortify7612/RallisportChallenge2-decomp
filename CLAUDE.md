# RalliSport Challenge 2 recompilation: notes for Claude

The Xbox game (NTSC, title 0x4D530039, XDK 5849), statically recompiled with
the vendored `xboxrecomp/` toolkit: lifted x86 to C, an HLE kernel, an NV2A
pushbuffer executor and a Vulkan renderer. Build and run steps are in
`README.md` and `rsc2/RUNNING.md`; read those first. This file holds what
they don't say: how we work, the traps, and the state of the Windows side.
General recompilation lessons are in the `xbox-recomp` skill
(`.claude/skills/xbox-recomp/`). Load it before debugging anything deep.

## Working agreements

- **Never commit or upload game data**, or anything derived from the XBE.
  `RalliSport2/`, `xboxrecomp/game_files/` (the XBE), `rsc2/src/recomp/`
  (generated code), `work/` and `third_party/` are gitignored. Players
  bring their own copy. Release binaries are the one exception (the
  translated code is built in, by the owner's choice). Before a release,
  check the packages: sample high-entropy 48-byte windows from the XBE's
  sections and make sure none appear in them (0 hits; a control run against
  the XBE itself finds all).
- **Ask before launching the game window** or any capture/playtest session,
  every time. Headless runs (`RECOMP_FB_WINDOW=0`) are fine unasked.
- **Commit and push each verified change as you go**, with a descriptive
  message. Features that need a live test are committed after the owner
  confirms them live. Stage only the files you changed.
- The repo is https://github.com/Mortify7612/RallisportChallenge2-decomp
  (the old captainslow48 URL redirects). Issues there track open bugs.

## Layout

- `rsc2/`: the game project. `src/main.c` bakes in the title's env defaults.
  `src/rsc2_overrides.c` holds hand-written replacements for lifted
  functions; `src/recomp_manual.c` covers vtable-only ones. `src/launcher.c`
  is the pre-game settings window. `config/seed_functions.json` and
  `config/trace_functions.json` hold seeds and trace hooks; keep the trace
  list `[]` except while tracing. `tools/` holds analysis scripts.
- `xboxrecomp/`: the toolkit, vendored and patched locally (not upstreamed).
  - `src/kernel/nv2a_vk.c`: Vulkan renderer.
  - `src/kernel/nv2a_pb_exec.c`: pushbuffer executor.
  - `src/kernel/wl_color.c`: Wayland HDR.
  - `src/video/fb_present.c`: the window, SDL or Win32.
  - `src/input/xinput_device.c`: pads and keyboard.
  - `src/apu/`: audio.
- Regenerate the lifted C with `rsc2/regen.sh` after editing seeds,
  overrides (adding a `sub_XXXXXXXX`) or the trace list. Delete
  `rsc2/src/recomp/gen/recomp_types.h` when the toolkit template changes.

## Traps (each cost real time once)

- **The launcher's `settings.ini` applies to headless runs too**
  (`~/.config/rsc2/settings.ini`, `%APPDATA%\rsc2\settings.ini`). Set
  `RECOMP_VK_SCALE`, `RECOMP_WIDESCREEN` and the rest explicitly in tests.
- **Headless replay** is the main test: `RSC2_INPUT_REPLAY=<file.xi>
  RECOMP_FB_WINDOW=0`, with `RECOMP_FB_DUMP_AT=<existing dir>:<frames>` for
  BMPs. The dump prefix is a directory and must exist. Recordings come
  from `RSC2_INPUT_RECORD`; the owner's is `work/my_run2.xi`, which is not
  in git, so copy it to another machine by hand. Replays drift after about
  17 s, so compare early frames (720–1440).
- **The shader UBO layout has two copies.** `k_prog_head` in `nv2a_vk.c`
  must match `xboxrecomp/src/kernel/shaders/nv2a_common.glsl`. Bump `VK_PROG_VERSION` when either
  changes, or cached programs go stale (the symptom: every program falls
  back to the CPU, about 30 fps).
- **Perf numbers are noisy.** NVIDIA sometimes runs a whole run in a slower
  state. Compare alternating runs, and make sure no stray `rsc2` is left
  running.
- **Killing processes on Linux:** `timeout` on `run.sh` kills `prime-run`,
  not the game; use `pkill -x rsc2`. Don't `pkill -f <pattern>` from a shell
  whose own command line contains the pattern: it kills itself.
- **Game-side vs renderer:** before blaming the game, prove a fault with a
  flight capture (`RECOMP_VK_FLIGHT`, F10) or the draw log
  (`RECOMP_VK_DRAWLOG`). The tree pop-in looked like game culling but was
  the executor dropping 63 quads at a split `BEGIN`.
- **gdb is unusable** on Linux (AC97 single-stepping). Use the crash
  handler's output, `kill -QUIT <pid>` for all-thread backtraces,
  `RECOMP_WATCH=<va>` and differential replay (see the skill).

## Windows: state and how to debug it

All Windows work so far was **cross-compiled on Linux with MinGW** and
tested **under Wine only**. Nothing has run on real Windows yet (issue #4).
The cross build is documented in `rsc2/RUNNING.md`, "Windows". It needs
`third_party/mingw`: static glslang and SDL2, the Vulkan headers and a
`vulkan-1.dll` import library. That folder is gitignored, and the rebuild
steps are in RUNNING.md.

**Building on Windows itself works** with MSYS2's MINGW64 shell (GCC 16,
Windows 11); steps in `rsc2/RUNNING.md`, "Windows (native build with
MSYS2)". Running that build on real Windows is still untested. Notes:

- No toolchain file and no `-D` paths: CMake finds MSYS2's glslang, SDL2
  and Vulkan on its own.
- Pass `-D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000007` in
  `CMAKE_C_FLAGS`, or `kernel_file.c` fails on `FileDispositionInfoEx`.
- `rsc2/regen.sh` needs `xboxrecomp/game_files/default_analysis.json`
  (from `tools.xbe_parser --json`) next to the XBE. A venv made with
  `--system-site-packages` over MSYS2's python-capstone/pefile/numpy has
  the `.venv/bin/python` it hardcodes.
- The result links glslang, SPIRV-Tools and the GCC runtime as DLLs from
  `/mingw64/bin`.

The generated `rsc2/src/recomp/` is the same on every host, so copying it
from the Linux machine avoids the Python setup. The game's CMake already
adds `-fwrapv -fno-strict-aliasing` for generated code under MinGW. MSVC
has never been tried: the launcher uses `#embed` and GNU extensions.

**What is Windows-specific in the code:**

- **Fibers:** Win32 fibers in `rsc2_overrides.c` (ucontext on Linux).
- **Faults:** guest faults go through a VEH in `main.c`, with an early
  return for host addresses at `0xFD...`.
- **Files:** renames go through `FileRenameInfo`. Write handles get
  `DELETE` access and `FILE_SHARE_DELETE` always, and deletes use the
  POSIX-semantics disposition.
- **Window and presenting:**
  - Win32 window in `fb_present.c`: DPI-aware, 1280x720 for 16:9,
    borderless fullscreen with F11 or Alt+Enter, and a GDI fallback.
  - Vulkan presents through `nv2a_vk_attach_hwnd`.
  - `wl_color.c` builds as stubs on Windows, so HDR there uses scRGB or
    HDR10. That's untested on Windows.
- **Launcher:** static SDL2 (`RSC2_LAUNCHER_SDL`, `SDL_MAIN_HANDLED`).
- **Pads:** go through XInput.

**Known Windows bugs:**

- **Menus are slow on Windows:** about 40 fps in the menus and car
  select, worse than Linux on the same title (owner's report, from a VM
  with the GPU passed through). Not diagnosed yet.
- **Headless under Wine** (`RECOMP_FB_WINDOW=0`) never reaches a frame dump,
  even with v0.4.0. Check Windows builds there by comparing logs with a
  previous release's.

**The Z: cache on Windows** (once the second-launch stall): RSC2's
CacheManager copies every archive into Z: on each boot and renames each one
into place without replacing; a failed rename leaves the main thread waiting
forever on the cache thread. Win32 backs the raw disk with
`Partition0.img`, where XAPI's XMountUtilityDrive keeps its cache database
(sector 4), and it reads the cluster size (64 KB for this title) from the
FATX header it wrote into `Partition5.img`. With both persisted it rightly
doesn't reformat Z:, so `RECOMP_CACHE_FRESH=1` (a default in `main.c`)
empties the Cache folder at start. Headless runs on real Windows reach
60 fps on both the first and second launch.

**Where things are on Windows:**

- **Settings:** `%APPDATA%\rsc2\settings.ini`.
- **Shader and pipeline cache:** `%LOCALAPPDATA%\xboxrecomp\vk`.
  `RECOMP_VK_CACHE=0` turns it off.
- **Z: cache:** `%LOCALAPPDATA%\xboxrecomp\Cache`.
- **Logs:** `rsc2.exe` is a GUI-subsystem program. Run it from cmd with
  `rsc2.exe > log.txt 2>&1`; that worked under Wine. `XBOX_LOG_LEVEL=3`
  also writes `xbox_kernel.log` next to the program.

The same env switches work on Windows (`set NAME=value` in cmd).
