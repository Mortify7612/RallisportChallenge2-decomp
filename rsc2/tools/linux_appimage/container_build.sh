#!/bin/bash
# Build half of the AppImage build (see build.sh, Containerfile).
#
# Runs INSIDE the build container, as the invoking user. Builds, in order:
#
#   1. glslang, static -- the game compiles the title's vertex programs to
#      SPIR-V at run time, and glslc.py compiles the toolkit's own shaders
#      with glslangValidator at build time;
#   2. SDL2 2.32.10, shared but with every audio/video backend loaded with
#      dlopen. Ubuntu's libSDL2 depends on libpulse, libsystemd, libsndfile,
#      ... which are not on every system; this one needs libc and little
#      else, so the AppImage can carry it;
#   3. the game into rsc2/build-appimage (never rsc2/build -- that is the
#      developer's build);
#   4. the libraries to put in the AppImage's usr/lib.
#
# The script is idempotent: sources and build trees live in .cache/ next to
# build.sh (gitignored), and are reused until the compiler or the recipe
# changes.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/../../.." && pwd)
cache=${RSC2_APPIMAGE_CACHE:-$here/.cache}
jobs=${JOBS:-$(( ($(nproc) + 1) / 2 ))}

die() { echo "error: $*" >&2; exit 1; }

src=$cache/src
downloads=$cache/downloads
glslang_src=$src/glslang-vulkan-sdk-1.4.363.0
vk_include=$src/Vulkan-Headers-vulkan-sdk-1.4.363.0/include
sdl_src=$src/SDL2-2.32.10
glslang_prefix=$cache/glslang-install
glslang_build=$cache/build-glslang
sdl_prefix=$cache/sdl2-install
sdl_build=$cache/build-sdl2
game_build=$root/rsc2/build-appimage
glslc_py=$root/rsc2/tools/linux_portable/glslc.py

[ -f "$glslang_src/CMakeLists.txt" ] || die "missing extracted glslang in $glslang_src (run build.sh on the host)"
[ -f "$vk_include/vulkan/vulkan.h" ] || die "missing extracted Vulkan headers in $vk_include (run build.sh on the host)"
[ -f "$sdl_src/CMakeLists.txt" ] || die "missing extracted SDL2 in $sdl_src (run build.sh on the host)"
[ -f "$glslc_py" ] || die "missing $glslc_py"

cc_id=$($CXX --version | head -1)
echo "== compiler: $cc_id"

# ── glslang (static) ────────────────────────────────────────────────
# Rebuilt when the compiler changes, like linux_portable/build.sh does: a
# static libstdc++ from another GCC must not be mixed into the link.
if [ "$(cat "$glslang_prefix/compiler" 2>/dev/null || true)" != "$cc_id" ]; then
    rm -rf "$glslang_build" "$glslang_prefix"
fi
if [ ! -f "$glslang_prefix/lib/libglslang.a" ] || [ ! -x "$glslang_prefix/bin/glslangValidator" ]; then
    echo "== building glslang (static)"
    cmake -S "$glslang_src" -B "$glslang_build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$glslang_prefix" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DENABLE_OPT=OFF -DENABLE_HLSL=OFF \
        -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=ON
    ninja -C "$glslang_build" -j "$jobs" install
    echo "$cc_id" > "$glslang_prefix/compiler"
fi

# ── SDL2 2.32.10 (shared, backends dlopen'd) ────────────────────────
# SDL_WAYLAND_SHARED / SDL_X11_SHARED / SDL_ALSA_SHARED /
# SDL_PULSEAUDIO_SHARED / SDL_PIPEWIRE_SHARED are the defaults; passing them
# is a statement of intent -- the bundled libSDL2 must not NEED those
# libraries, or the AppImage would depend on the distribution's audio stack
# versions. JACK, SNDIO, libsamplerate, KMSDRM, D-Bus and IBus are left out:
# a racing game does not need them, and each would add a dependency.
if [ "$(cat "$sdl_prefix/compiler" 2>/dev/null || true)" != "$cc_id" ]; then
    rm -rf "$sdl_build" "$sdl_prefix"
fi
if [ ! -e "$sdl_prefix/lib/libSDL2-2.0.so.0" ]; then
    echo "== building SDL2 2.32.10 (shared, dynamic backends)"
    cmake -S "$sdl_src" -B "$sdl_build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$sdl_prefix" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF \
        -DSDL_WAYLAND=ON -DSDL_WAYLAND_SHARED=ON \
        -DSDL_X11=ON -DSDL_X11_SHARED=ON \
        -DSDL_ALSA=ON -DSDL_ALSA_SHARED=ON \
        -DSDL_PULSEAUDIO=ON -DSDL_PULSEAUDIO_SHARED=ON \
        -DSDL_PIPEWIRE=ON -DSDL_PIPEWIRE_SHARED=ON \
        -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_LIBSAMPLERATE=OFF \
        -DSDL_KMSDRM=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF
    ninja -C "$sdl_build" -j "$jobs" install
    echo "$cc_id" > "$sdl_prefix/compiler"
fi

# ── the game ────────────────────────────────────────────────────────
# A new compiler rebuilds the game from scratch (CMake does not notice a
# changed gcc-15 minor version by itself).
if [ "$(cat "$game_build/.cc-id" 2>/dev/null || true)" != "$cc_id" ]; then
    rm -rf "$game_build"
fi
mkdir -p "$game_build"
echo "$cc_id" > "$game_build/.cc-id"

echo "== configuring the game"
cmake -S "$root/rsc2" -B "$game_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_EXE_LINKER_FLAGS=-static-libgcc \
    -DSDL2_DIR="$sdl_prefix/lib/cmake/SDL2" \
    -DVulkan_INCLUDE_DIR="$vk_include" \
    -DVulkan_LIBRARY=/usr/lib/x86_64-linux-gnu/libvulkan.so.1 \
    -DXBOXRECOMP_GLSLC="$glslc_py" \
    -DXBOXRECOMP_GLSLANG_INCLUDE="$glslang_prefix/include" \
    -DXBOXRECOMP_GLSLANG_LIB="$glslang_prefix/lib/libglslang.a" \
    -DXBOXRECOMP_GLSLANG_RL_LIB="$glslang_prefix/lib/libglslang-default-resource-limits.a" \
    "-DXBOXRECOMP_GLSLANG_EXTRA_LIBS=$glslang_prefix/lib/libSPIRV.a;$glslang_prefix/lib/libMachineIndependent.a;$glslang_prefix/lib/libOSDependent.a;$glslang_prefix/lib/libGenericCodeGen.a;$glslang_prefix/lib/libMachineIndependent.a;$($CXX -print-file-name=libstdc++.a);pthread"

echo "== building the game (jobs: $jobs)"
GLSLANG_VALIDATOR="$glslang_prefix/bin/glslangValidator" \
    ninja -C "$game_build" -j "$jobs"

bin=$game_build/rsc2
[ -x "$bin" ] || die "the build produced no $bin"

# The AppImage carries a stripped copy; the unstripped binary stays in the
# build directory for debugging. -rdynamic keeps the sub_XXXXXXXX names in
# .dynsym, so the game's fault handler can still name a guest function.
cp "$bin" "$game_build/rsc2-appimage"
strip --strip-unneeded "$game_build/rsc2-appimage"
app_bin=$game_build/rsc2-appimage

# ── libraries to bundle ─────────────────────────────────────────────
# Everything the binary needs beyond glibc and the Vulkan loader/driver
# (normally SDL2, and libepoxy if the toolkit's OpenGL fallback got linked
# in). The Vulkan loader and the GPU driver stay on the system: a bundled
# loader would still have to find the distribution's ICD JSONs, and the
# driver must match the installed hardware.
needed() { readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }

libs=$cache/runtime-libs
rm -rf "$libs"
mkdir -p "$libs"

glibc="libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 ld-linux-x86-64.so.2"

for lib in $(needed "$app_bin"); do
    case " $glibc libvulkan.so.1 " in *" $lib "*) continue ;; esac
    path=$(ls "$sdl_prefix/lib/$lib" /usr/lib/x86_64-linux-gnu/"$lib" 2>/dev/null | head -1 || true)
    [ -n "$path" ] || die "the binary needs $lib, but it is not in the SDL2 prefix or /usr/lib; add it to the bundle"
    cp -L "$path" "$libs/"
done
[ -e "$libs/libSDL2-2.0.so.0" ] || die "SDL2 is missing from the bundle (the launcher and the runtime need it)"

# Each bundled library may only need glibc. Anything else would make the
# AppImage depend on the build distribution's audio/display stack, exactly
# what building SDL2 with dlopen'd backends avoids.
for f in "$libs"/*; do
    for lib in $(needed "$f"); do
        case " $glibc " in *" $lib "*) continue ;; esac
        die "$(basename "$f") needs '$lib': that backend was linked instead of dlopen'd; fix the SDL2 options"
    done
done

bundled_list=$(ls "$libs")
is_bundled() {
    for f in $bundled_list; do [ "$f" = "$1" ] && return 0; done
    return 1
}

for lib in $(needed "$app_bin"); do
    case " $glibc libvulkan.so.1 " in *" $lib "*) continue ;; esac
    is_bundled "$lib" || die "the binary needs '$lib', which is not in the bundle"
done

echo
echo "== payload"
ls -l "$app_bin" "$libs"/*
echo
echo "== runtime libraries the system must provide:"
for lib in $(needed "$app_bin"); do
    is_bundled "$lib" || echo "   $lib"
done
echo "== bundled libraries:"
for f in $bundled_list; do echo "   $f"; done

newest=$(objdump -T "$app_bin" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
echo "== newest glibc symbol: $newest (the AppImage runs on distributions with glibc >= that)"
