# Platforms, releases, distribution

## Linux (primary)

- SDL2 window/input/audio, Vulkan via glslang's C API for runtime shader
  compiles, and OpenSSL for kernel crypto.
- On hybrid GPUs, launch through a `run.sh` that runs `prime-run` with
  `VK_ICD_FILENAMES` unset; the user's shell env pinned the iGPU. `run.sh`
  changes into the build dir, so relative paths in env vars break.

## Windows (cross-built with MinGW, tested under Wine)

- A MinGW toolchain file, `-static -static-libgcc`, and the same
  `-fwrapv`/`-fno-strict-aliasing` for generated code.
- `third_party/mingw` holds a static glslang (tag vulkan-sdk-1.4.363.0),
  static SDL2 2.32.10, the Vulkan headers, and a `vulkan-1` import library
  generated from the headers (`tools/mingw_vulkan_def.py`). A hand-written
  def list broke linking.
- Win32 fibers for the XAPI fiber HLE, and `FileRenameInfo` renames.
- Test under Wine with a throwaway WINEPREFIX. Xvfb can't present Vulkan (no
  DRI3), so test GDI presentation there.

## ARM64 / Android groundwork (`arm64` branch)

- Build with clang + lld and a toolchain file against an Arch Linux ARM
  sysroot made without root (`tools/arm64/make_sysroot.py`: repo databases,
  dependency resolution, SHA-256 checks, mirror fallback with a slow-mirror
  cutoff). Run under `qemu-aarch64 -L <sysroot>` at about a quarter of
  native speed.
- What was x86-only: the fault handler's CONTEXT, the MMIO instruction
  decoder (added AArch64 LDR/STR in all single-register forms, SIMD&FP,
  LDAR/STLR/LDAPR), trap-flag stepping (on AArch64, emulate the store
  instead), `pause` (`yield` on ARM), and profiler PC reads. SSE intrinsics
  already had portable fallbacks.
- **qemu on an x86 host can't show memory-ordering bugs** (it inherits x86
  ordering). Hence `RECOMP_GUEST_LOCK=1` is the ARM default.
- Android (done in RSC2, see `rsc2/android/README.md`): APK built without
  Gradle (NDK CMake plus javac, d8, aapt2, zipalign and apksigner, with
  SDL2's SDLActivity). Fixes needed:
  - bionic has no ucontext: `host_coro.c` stack switch;
  - bionic lacks memfd before API 30, backtrace before API 33,
    explicit_bzero and pthread_cancel;
  - Vulkan entry points after 1.1 must come from vkGetDeviceProcAddr;
  - set SDL_HINT_ORIENTATIONS;
  - pipe stderr to logcat;
  - put game data in internal storage via `run-as`, because adb-pushed
    Android/data files are unreadable by the app on Android 11+. If an
    `adb exec-in ... tar -x` pipe stalls (seen on an Android 14 GSI: adbd
    kept reading and buffered ~900 MB on the device), `adb push` to
    /data/local/tmp and `run-as <pkg> cp -r` from there.
- **Android: thread-local registers are a call each.** The app's library is
  dlopen'd, so bionic gives it dynamic TLS, reached through TLS descriptors:
  `blr` into the linker's resolver once per thread-local per function (5-10
  per lifted function). Initial-exec is refused for dlopen'd libraries, and
  lld can't link AArch64 local-dynamic (R_AARCH64_TLSLD_*). Fix (RSC2
  0dc00e2): all register state in one TLS struct, with the generated code's
  names as macros onto it (`#define g_esp (g_rb.g_esp)`: members keep the
  old names, since a member named `esp` gets rewritten by `#define esp`).
  Hand-written code keeps working through TLS symbol aliases:
  `.globl g_esp; .type g_esp,%tls_object; .size g_esp,4; .set g_esp, g_rb+12`
  in top-level asm. Result: 1 resolver call per function, lifted .text -12%,
  game thread -18% per frame. The Switch's version of the same cost is
  `__aarch64_read_tp` (fixed with `-mtp=tpidr_el0`).
- **The game/executor overlap** (PFB flush answered at once, notify and
  progress counter modelled; see kernel-hle.md) is the biggest single ARM
  gain: Snapdragon 870 headless race 77 -> 117 fps.
- **Measuring on a phone or tablet:**
  - Presenting blocks at the panel's refresh even in mailbox mode, so a
    frame slightly over 1/refresh takes two. Benchmark CPU changes headless
    (`RECOMP_FB_WINDOW=0`, unpaced), with a per-flip log over a fixed flip
    range, since a replay's race can pass in a few seconds.
  - When fps doesn't move because another thread sets the pace, compare
    per-thread CPU time per frame from `/proc/<pid>/task/*/stat`.
  - Read temperatures from `/sys/class/thermal/*/temp`
    (`cpu-*-usr` zones): `dumpsys thermalservice` showed stale cached
    values. Cool to the same temperature before each run.
- Ports judged against each other: Android is feasible; Switch needs a
  non-Vulkan renderer (deko3d/GL) and has no signals; Vita is not realistic
  (444 MHz Cortex-A9, a 32-bit host can't map the 4 GB guest span, and the
  GPU is GLES2-class).

## Releases

- Package Linux `.tar.gz` and Windows `.zip`, each with a `README.txt` and a
  release `run.sh` that checks for `game/default.xbe`, plus `SHA256SUMS.txt`.
  `gh release create --target` needs the full SHA.
- **No game data in packages:** probe the binaries for byte windows sampled
  from the XBE sections (expect 0 hits, and run a control probe on the XBE
  itself that hits everything). The XBE is loaded from `game/` at runtime;
  the translated code is built in, which the user chose to ship.
- A Linux binary built on a rolling distribution needs current glibc and
  glslang, and won't start on Mint/Ubuntu LTS. Build releases in an Ubuntu
  22.04 container instead (RSC2: `rsc2/tools/linux_portable/build.sh`,
  podman). Use glibc 2.35, link glslang (built from source), libstdc++ and
  libgcc statically, and leave only SDL2 and libvulkan dynamic. Check with
  `objdump -T | grep GLIBC_` and run a headless replay inside a clean
  22.04 container with `--device /dev/dri`.
  - Use a modern GCC (toolchain PPA `gcc-15`; its `gcc-16` is a trunk
    snapshot). 22.04's GCC 11 ran the lifted code 15-25% slower.
  - 22.04 has no `glslc`; `glslangValidator` behind a glslc-argument
    wrapper works.
  - Drop link deps nothing calls: OpenSSL was linked but unused.

## Steam

- "Add to Steam" appends a binary-VDF entry to
  `userdata/<account>/config/shortcuts.vdf`. Use the most recent account
  from `loginusers.vdf`, dedupe, keep a `.bak`, and refuse while Steam runs:
  Steam rewrites the file on exit.

## Nintendo Switch homebrew (RSC2 `switch` branch)

- Build: devkitA64 + libnx in devkitPro's container (`podman`), no root.
  Passing `-DCMAKE_C_FLAGS` drops devkitPro's init flags (incl.
  `-D__SWITCH__`); add flags via `CMAKE_C_FLAGS_RELEASE`.
- Memory: an unprivileged process can place memory at chosen addresses only
  in its stack region (`svcMapMemory`, heap pages moved there, < 2 GB).
  `svcMapPhysicalMemory` (alias region) needs a system resource budget,
  which homebrew may not have. So fold the guest's high half down
  (`XBOX_FOLD_HIGH`: VAs with bit 31 set get bits 31:30 flipped), which
  fits in 0x44000000 bytes; test the fold on Linux first.
- **Make the fold a function, not a macro.** A macro evaluating its
  argument twice broke `MEM8(in++)` in a hand-written override (zlib
  inflate) and surfaced as the game's own "Error decompressing block"
  assert. Find a game's assert screen by its strings ("file", "line",
  "expression") and dump text from the guest stack when it fires.
- Faults: replace libnx's weak `__libnx_exception_entry` (kernel context at
  x1: x0-x8, lr, sp, pc, pstate, afsr0/1, esr, far; x9-x29 and q-regs still
  live), run the VEH handlers, resume with `svcReturnFromException(0)`.
- newlib/libnx gaps: no mmap/sigaction/ucontext/dlfcn/execinfo/sysinfo,
  `timegm`, `sysconf`, `posix_memalign`, `fstatvfs`; `open()` on a
  directory fails; paths with a device prefix (`sdmc:`) or a bare drive
  (`z:`) confuse libnx (it crashed in `mkdir` on "z:"); each thread has its
  own `stderr` FILE, so redirect logs with `dup2` on fds 1/2.
- Ryujinx (Flathub `io.github.ryubing.Ryujinx`, needs the owner's
  `prod.keys`) runs `.nro` files but does not deliver guest faults to the
  program (any bad access ends it) and refuses `svcSetMemoryPermission` on
  stack-region memory. Fault-based device models must be off there.
- **Device registers by direct call, not faults** (the RSC2 port does
  this): the lifter writes every store as `MEMn_W(addr, value)` and the
  runtime routes accesses at or above 0xFD000000 to device callbacks. That
  works in Ryujinx, avoids a kernel round trip per access, and lets a
  register poll yield (see scheduling below).
- **Real hardware findings** (none of these show in Ryujinx):
  - Start hbmenu in title-override mode (hold R on a game): from the Album
    it is a library applet with ~380 MB total, and a big title hangs at the
    first failed mapping. Check `svcGetInfo(InfoType_TotalMemorySize)` and
    say so on screen.
  - Unbuffered SD-card writes cost ~60 ms a line: log through a buffered
    devoptab, or stream with nxlink (`nxlink -s -p rsc2/rsc2.nro ...`; `-p`
    is relative to `sdmc:/switch/`, the folder must exist). Never probe
    the netloader port by hand: hbmenu leaves netloader mode after any
    connection.
  - Horizon's FS refuses a second open of a file open for writing, and a
    rename of an open file (both EIO). Share the open descriptor (own
    position per handle) and rename by close/rename/reopen.
  - No page aliasing (one memory, one mapping): fold aliases like the
    tiled aperture onto their target in the address translation.
  - **Scheduling:** libnx starts every pthread at priority 59 on the
    default core; the main thread stays at the application's 44 (lower
    number = higher priority), and Horizon does not time-slice equal
    priorities. A guest busy-wait on the main thread then starves every
    host thread. Place all threads explicitly (`svcSetThreadPriority`,
    `svcSetThreadCoreMask(.., core, 0x7)`, wrap `pthread_create` with
    `-Wl,--wrap`). Model it on Linux with `taskset -c 0-2 chrt -f 10`
    (one CPU shows every busy-wait).
  - **Hand-written asm labels whose address C takes must be hidden
    globals.** host_coro's `coro_start` was a bare label referenced as
    `extern char coro_start[]`; devkitA64 loaded it through the GOT, which
    held the asm block's first address (`coro_switch`), so every new
    coroutine spun in `coro_switch` forever: no fault, no kernel call.
    Check with objdump that `coro_init` computes the right address.
  - Without signals, a stall watchdog stands in for `kill -QUIT`: per guest
    thread, the last kernel calls, live guest esp (a pointer to its TLS
    `g_esp`), return addresses on the stack, plus the host exception count.
