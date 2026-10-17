#!/bin/sh
# Build the Android APK (arm64-v8a) without Gradle: the NDK builds libmain.so
# and libSDL2.so, then javac, d8, aapt2, zipalign and apksigner from the SDK
# make and sign the package. See README.md for the one-time setup.
#
#   rsc2/android/build_apk.sh            -> rsc2/build-android/rsc2.apk
#
# Environment: ANDROID_SDK (default ~/Android/Sdk), JAVA_HOME (a JDK 17+,
# default ~/Android/jdk21), NDK_VERSION, BUILD_TOOLS, PLATFORM to override
# the newest installed ones. RSC2_ANDROID_CFLAGS (default -gline-tables-only:
# line numbers for simpleperf, stripped from the APK's copy).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
SDK=${ANDROID_SDK:-$HOME/Android/Sdk}
JAVA_HOME=${JAVA_HOME:-$HOME/Android/jdk21}
[ -x "$JAVA_HOME/bin/javac" ] || JAVA_HOME=$HOME/Android/jdk21
NDK=$SDK/ndk/${NDK_VERSION:-$(ls "$SDK/ndk" | sort -V | tail -1)}
BT=$SDK/build-tools/${BUILD_TOOLS:-$(ls "$SDK/build-tools" | sort -V | tail -1)}
PLAT=$SDK/platforms/${PLATFORM:-$(ls "$SDK/platforms" | sort -V | tail -1)}
SDL=$root/third_party/src/SDL2-2.32.10
TP=$root/third_party/android-arm64
OUT=$root/rsc2/build-android
STAGE=$OUT/apk
export JAVA_HOME

for f in "$NDK/build/cmake/android.toolchain.cmake" "$BT/aapt2" "$PLAT/android.jar" \
         "$SDL/android-project" "$TP/lib/libglslang.a"; do
    [ -e "$f" ] || { echo "build_apk.sh: missing $f (see rsc2/android/README.md)"; exit 1; }
done

# ── native ──────────────────────────────────────────────────────
cmake -S "$root/rsc2" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
    "-DCMAKE_C_FLAGS=${RSC2_ANDROID_CFLAGS:--gline-tables-only}" \
    -DXBOXRECOMP_GLSLC="$(command -v glslc)" \
    -DXBOXRECOMP_GLSLANG_INCLUDE="$TP/include" \
    -DXBOXRECOMP_GLSLANG_LIB="$TP/lib/libglslang.a" \
    -DXBOXRECOMP_GLSLANG_RL_LIB="$TP/lib/libglslang-default-resource-limits.a" \
    "-DXBOXRECOMP_GLSLANG_EXTRA_LIBS=$TP/lib/libSPIRV.a;$TP/lib/libMachineIndependent.a;$TP/lib/libOSDependent.a;$TP/lib/libGenericCodeGen.a;$TP/lib/libMachineIndependent.a;c++_static;c++abi" \
    > "$OUT.configure.log"
ninja -C "$OUT"

rm -rf "$STAGE"
mkdir -p "$STAGE/lib/arm64-v8a" "$STAGE/classes" "$STAGE/dex"
STRIP=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
"$STRIP" --strip-unneeded -o "$STAGE/lib/arm64-v8a/libmain.so" "$OUT/libmain.so"
"$STRIP" --strip-unneeded -o "$STAGE/lib/arm64-v8a/libSDL2.so" "$OUT/SDL2/libSDL2.so"
# libadrenotools' hooks: loaded from the app's native library folder.
for h in "$OUT"/adrenotools/src/hook/lib*.so; do
    [ -e "$h" ] && "$STRIP" --strip-unneeded -o "$STAGE/lib/arm64-v8a/$(basename "$h")" "$h"
done
# Turnip, Mesa's Vulkan driver for Adreno (build_turnip.sh), for devices
# whose own driver is older than Vulkan 1.3. RSC2_TURNIP=<file> picks another
# build; without one the APK uses the system driver only.
TURNIP=${RSC2_TURNIP:-$root/third_party/build-android-turnip/src/freedreno/vulkan/libvulkan_freedreno.so}
if [ -e "$TURNIP" ]; then
    "$STRIP" --strip-unneeded -o "$STAGE/lib/arm64-v8a/libvulkan_freedreno.so" "$TURNIP"
else
    echo "build_apk.sh: no Turnip ($TURNIP) -- system Vulkan driver only"
fi

# ── java ────────────────────────────────────────────────────────
"$JAVA_HOME/bin/javac" -nowarn -source 11 -target 11 -encoding UTF-8 \
    -classpath "$PLAT/android.jar" -d "$STAGE/classes" \
    $(find "$SDL/android-project/app/src/main/java" "$here/src" -name '*.java') 2>&1 \
    | grep -v "^warning: \[options\]\|^Note: \|^1 warning" || true
"$BT/d8" --release --min-api 29 --lib "$PLAT/android.jar" --output "$STAGE/dex" \
    $(find "$STAGE/classes" -name '*.class')

# ── package ─────────────────────────────────────────────────────
"$BT/aapt2" link -o "$STAGE/base.apk" -I "$PLAT/android.jar" \
    --manifest "$here/AndroidManifest.xml" --min-sdk-version 29 --target-sdk-version 34
python3 -I - "$STAGE" <<'EOF'
import os, sys, zipfile
stage = sys.argv[1]
with zipfile.ZipFile(os.path.join(stage, 'base.apk'), 'a', zipfile.ZIP_DEFLATED) as z:
    z.write(os.path.join(stage, 'dex', 'classes.dex'), 'classes.dex')
    for so in sorted(os.listdir(os.path.join(stage, 'lib', 'arm64-v8a'))):
        z.write(os.path.join(stage, 'lib', 'arm64-v8a', so), 'lib/arm64-v8a/' + so)
EOF
"$BT/zipalign" -p -f 4 "$STAGE/base.apk" "$STAGE/aligned.apk"

# A local debug key, made once; never committed.
KS=${RSC2_KEYSTORE:-$HOME/.android/rsc2-debug.keystore}
if [ ! -f "$KS" ]; then
    mkdir -p "$(dirname "$KS")"
    "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$KS" -storepass android -keypass android \
        -alias rsc2 -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=RSC2 recomp debug" >/dev/null 2>&1
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --ks-key-alias rsc2 --out "$OUT/rsc2.apk" "$STAGE/aligned.apk"
echo "built $OUT/rsc2.apk ($(du -h "$OUT/rsc2.apk" | cut -f1))"
