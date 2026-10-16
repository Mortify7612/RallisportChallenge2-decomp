# Android build (arm64, work in progress)

An APK for 64-bit ARM phones and tablets with a Vulkan 1.3 driver. The game
runs as on Linux: SDL2 for the window, input and audio, and the same Vulkan
renderer. It is built without Gradle; `build_apk.sh` calls the NDK and SDK
tools directly.

## One-time setup (no root needed)

Everything goes in `~/Android`:

1. **JDK 17 or newer** in `~/Android/jdk21` (e.g. an Adoptium tarball).
2. **Android SDK command-line tools** in `~/Android/Sdk/cmdline-tools/latest`
   (`commandlinetools-linux-*_latest.zip` from developer.android.com).
3. The SDK packages:
   ```sh
   export JAVA_HOME=~/Android/jdk21
   S=~/Android/Sdk/cmdline-tools/latest/bin/sdkmanager
   yes | $S --sdk_root=$HOME/Android/Sdk --licenses
   $S --sdk_root=$HOME/Android/Sdk "ndk;29.0.14206865" "platforms;android-36" \
      "build-tools;36.1.0" "platform-tools"
   ```
4. **glslang for Android** (static, into `third_party/android-arm64`), from
   the same source as the Windows build (`third_party/src/glslang`, tag
   `vulkan-sdk-1.4.363.0`):
   ```sh
   NDK=~/Android/Sdk/ndk/29.0.14206865
   cmake -S third_party/src/glslang -B third_party/build-android-glslang -G Ninja \
         -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
         -DBUILD_SHARED_LIBS=OFF -DENABLE_OPT=OFF -DENABLE_HLSL=OFF \
         -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=OFF \
         -DCMAKE_INSTALL_PREFIX=$PWD/third_party/android-arm64
   ninja -C third_party/build-android-glslang install
   ```
5. **SDL2 2.32.10 source** in `third_party/src/SDL2-2.32.10`. This is the
   same tarball as the Windows build; its `android-project` Java files are
   compiled into the APK.

The host's `glslc` compiles the renderer's built-in shaders, as on Linux.

## Build and install

```sh
rsc2/android/build_apk.sh                 # -> rsc2/build-android/rsc2.apk
~/Android/Sdk/platform-tools/adb install -r rsc2/build-android/rsc2.apk
```

The APK is signed with a debug key that is made once in
`~/.android/rsc2-debug.keystore`.

## Game data

Android 11 and later do not let an app read files that `adb push` (or a PC)
puts in its `Android/data` folder. Until the app can import a folder itself,
copy the game into the app's internal storage *as the app* (the APK is
debuggable, so `run-as` works):

```sh
A=~/Android/Sdk/platform-tools/adb
cd <your game folder>          # default.xbe, Media, Layer0, ...
tar -cf - . | $A exec-in run-as io.github.rsc2recomp \
    sh -c 'mkdir -p files/game && tar -C files/game -xf -'
```

That is `/data/data/io.github.rsc2recomp/files/game`. The app uses it when
`game/default.xbe` is there, and otherwise falls back to
`/sdcard/Android/data/io.github.rsc2recomp/files`. Next to `game/`:

- `rsc2.log`: stderr and stdout. They also go to logcat, tag `RSC2`.
- `rsc2.env`: `NAME=value` lines applied at start, e.g. `RECOMP_FPS_OVERLAY=1`
  or `RECOMP_VK_SCALE=1`. Write it with
  `echo RECOMP_VK_SCALE=1 | $A exec-in run-as io.github.rsc2recomp sh -c 'cat > files/rsc2.env'`.
- `config/`, `data/`, `cache/`: settings, saves and the shader cache.

The phone must be unlocked with the screen on. SDL waits for a visible
surface before it starts the game.

## First device test (OnePlus, Snapdragon 8 Gen 3 / Adreno 750, Android 15)

- Runs: intro movies, menus and races, with the Xbox controller over
  Bluetooth and the right controls.
- Menus and movies run at about 58 fps (GPU 2 ms/frame).
- Races, measured with the race replay while driving, at the default 2×:
  14–15 fps at first, **73–78 fps** now (phone cooled to 45 °C before each
  run; a hot phone throttles and loses several fps). Fixes, in order:
  - The swapchain was rebuilt every frame, because Android reports
    "suboptimal" whenever the panel is rotated (46 fps).
  - The combiner shader runs at 16-bit precision.
  - A full clear now cancels a superseded shadow-map copy-back instead of
    waiting a frame for it (61 fps).
  - Texture changes are checked sparsely on Android: 1/16 of each texture
    every frame, plus a full check every 30 frames (+5%).
  - Vertex fetch writes each vertex once, and runs of bulk pushbuffer
    methods are handled in one call; integer formats unrolled too (+12%).

Profiling: `simpleperf` from the NDK (`app_profiler.py -p io.github.rsc2recomp`)
works on the debuggable APK. `build_apk.sh` keeps line tables in the
unstripped `rsc2/build-android/libmain.so` for it.

## What differs from Linux

- **Fibers:** bionic has no ucontext, so the XAPI fiber HLE uses
  `rsc2/src/host_coro.c` (`-DRSC2_HOST_CORO=ON` tests it on Linux).
- **Vulkan:** functions newer than Vulkan 1.1 are looked up with
  `vkGetDeviceProcAddr` (`nv2a_vk.c`, `vk_load_device_fns`), because
  `libvulkan.so` exports only 1.1 at API 29.
- **No D3D8-over-OpenGL layer and no OpenSSL.** Neither is used by this game.
- **No launcher window yet.** The game starts full screen with the default
  settings.
- **One guest thread at a time** (`RECOMP_GUEST_LOCK=1`, the ARM default).

Not done yet: race performance on mobile GPUs, touch controls (use a
Bluetooth or USB controller), the launcher, and importing the game folder
from the phone itself.
