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
    Android/data files are unreadable by the app on Android 11+.
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
  glslang. Say so in the release notes.

## Steam

- "Add to Steam" appends a binary-VDF entry to
  `userdata/<account>/config/shortcuts.vdf`. Use the most recent account
  from `loginusers.vdf`, dedupe, keep a `.bak`, and refuse while Steam runs:
  Steam rewrites the file on exit.
