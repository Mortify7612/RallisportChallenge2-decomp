#!/usr/bin/env bash
# Build a self-contained AppImage of the native Linux rsc2 build.
#
#   rsc2/tools/linux_appimage/build.sh [jobs]
#
# Writes rsc2/tools/linux_appimage/dist/rsc2-<version>-x86_64.AppImage.
# The game data is not included; the player puts their copy in a "game"
# folder next to the AppImage (see AppRun).
#
# It builds inside an Ubuntu 22.04 container (glibc 2.35 floor) with GCC 15,
# so the result does not need this machine's glibc 2.4x. docker or podman is
# required, and the first run downloads glslang, the Vulkan headers, SDL2 and
# appimagetool (~30 MB) into .cache/ (gitignored). The game itself is built in
# rsc2/build-appimage; rsc2/build, the developer's build, is never touched.
#
# Environment:
#   JOBS=N                  build parallelism (default: half the cores)
#   VERSION=...             AppImage version / file name tag
#                           (default: `git describe --tags --always --dirty`)
#   OUT=path.AppImage       output path
#   RSC2_APPIMAGE_CACHE=dir where sources/build trees are kept
#   ENGINE=docker|podman    container engine (default: whichever works)
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/../../.." && pwd)
cache=${RSC2_APPIMAGE_CACHE:-$here/.cache}
dist=$here/dist
jobs=${JOBS:-$(( ($(nproc) + 1) / 2 ))}

log() { printf '== %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# Optional positional argument: the jobs count, like linux_portable/build.sh.
if [ $# -gt 0 ]; then
    case $1 in
        ''|*[!0-9]*) die "usage: $0 [jobs]  (jobs must be a number)" ;;
    esac
    jobs=$1
fi

[ -d "$root/rsc2/src/recomp/gen" ] || die "no generated game code: run rsc2/regen.sh first"
[ -f "$root/rsc2/tools/linux_portable/glslc.py" ] || die "missing rsc2/tools/linux_portable/glslc.py"

# ── container engine ────────────────────────────────────────────────
engine=${ENGINE:-}
if [ -z "$engine" ]; then
    if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
        engine=docker
    elif command -v podman >/dev/null 2>&1; then
        engine=podman
    fi
fi
case $engine in
    docker) command -v docker >/dev/null 2>&1 || die "ENGINE=docker but docker is not on PATH" ;;
    podman) command -v podman >/dev/null 2>&1 || die "ENGINE=podman but podman is not on PATH" ;;
    *) die "needs docker or podman with a working daemon" ;;
esac
log "container engine: $engine"

# ── downloads (cached, gitignored) ──────────────────────────────────
fetch() { # url dest
    local url=$1 dest=$2 tmp=$2.part
    [ -s "$dest" ] && return 0
    mkdir -p "$(dirname "$dest")"
    log "downloading $(basename "$dest")"
    rm -f "$tmp"
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 -o "$tmp" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$tmp" "$url"
    else
        die "need curl or wget to download $url"
    fi
    mv "$tmp" "$dest"
}

extract() { # archive destdir expected-top-level-dir
    local archive=$1 dest=$2 expected=$3
    [ -d "$dest/$expected" ] && return 0
    log "extracting $(basename "$archive")"
    mkdir -p "$dest"
    tar -xzf "$archive" -C "$dest"
    [ -d "$dest/$expected" ] || die "unexpected layout in $(basename "$archive")"
}

tag=vulkan-sdk-1.4.363.0
fetch "https://github.com/KhronosGroup/glslang/archive/refs/tags/$tag.tar.gz" \
      "$cache/downloads/glslang-$tag.tar.gz"
fetch "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/$tag.tar.gz" \
      "$cache/downloads/Vulkan-Headers-$tag.tar.gz"
fetch "https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz" \
      "$cache/downloads/SDL2-2.32.10.tar.gz"
fetch "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage" \
      "$cache/downloads/appimagetool-x86_64.AppImage"
# The AppImage runtime appimagetool embeds. Fetching it ourselves keeps the
# build working offline after the first run.
fetch "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64" \
      "$cache/downloads/runtime-x86_64"

extract "$cache/downloads/glslang-$tag.tar.gz" "$cache/src" "glslang-$tag"
extract "$cache/downloads/Vulkan-Headers-$tag.tar.gz" "$cache/src" "Vulkan-Headers-$tag"
extract "$cache/downloads/SDL2-2.32.10.tar.gz" "$cache/src" "SDL2-2.32.10"

# appimagetool is itself an AppImage; FUSE is not needed to use it: extract
# it once and run the extracted AppRun (it ships mksquashfs and zsyncmake).
ait_root=$cache/appimagetool
ait=$ait_root/squashfs-root/AppRun
if [ ! -x "$ait" ]; then
    log "extracting appimagetool"
    chmod +x "$cache/downloads/appimagetool-x86_64.AppImage"
    rm -rf "$ait_root"
    mkdir -p "$ait_root"
    ( cd "$ait_root" && "$cache/downloads/appimagetool-x86_64.AppImage" --appimage-extract >/dev/null )
    [ -x "$ait" ] || die "could not extract appimagetool"
fi

# ── container build ─────────────────────────────────────────────────
# The image is content-addressed by its Containerfile: changing the recipe
# rebuilds it, otherwise the cached layers are reused.
image_hash=$(sha256sum "$here/Containerfile" | cut -c1-16)
image=rsc2-appimage-build:$image_hash
if ! $engine image inspect "$image" >/dev/null 2>&1; then
    log "building the build image (Ubuntu 22.04 + GCC 15)"
    $engine build -q -t "$image" -f "$here/Containerfile" "$here" >/dev/null
fi

run_args=(--rm -e HOME=/tmp -e "JOBS=$jobs"
          -e "RSC2_APPIMAGE_CACHE=$cache"
          -v "$root:$root" -v "$cache:$cache" -w "$root")
if [ "$engine" = docker ]; then
    run_args+=(--user "$(id -u):$(id -g)")
else
    run_args+=(--userns=keep-id)
fi
log "building glslang, SDL2 and the game (first run: several minutes)"
$engine run "${run_args[@]}" "$image" bash -euo pipefail "$here/container_build.sh"

# ── AppDir ──────────────────────────────────────────────────────────
app_bin=$root/rsc2/build-appimage/rsc2-appimage
libs=$cache/runtime-libs
[ -x "$app_bin" ] || die "the container build produced no $app_bin"
[ -d "$libs" ] || die "the container build produced no $libs"

appdir=$cache/appdir
rm -rf "$appdir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/lib" \
         "$appdir/usr/share/applications" \
         "$appdir/usr/share/icons/hicolor/512x512/apps"

cp "$app_bin" "$appdir/usr/bin/rsc2"
cp -L "$libs"/* "$appdir/usr/lib/"
cp "$here/AppRun" "$appdir/AppRun"
chmod 755 "$appdir/AppRun"
cp "$here/rsc2.desktop" "$appdir/rsc2.desktop"
cp "$here/rsc2.png" "$appdir/rsc2.png"
cp "$here/rsc2.desktop" "$appdir/usr/share/applications/"
cp "$here/rsc2.png" "$appdir/usr/share/icons/hicolor/512x512/apps/"
ln -sf rsc2.png "$appdir/.DirIcon"

# The binary finds the bundled libraries through an RPATH when it is started
# straight from the AppDir; AppRun also exports LD_LIBRARY_PATH.
if command -v patchelf >/dev/null 2>&1; then
    patchelf --set-rpath '$ORIGIN/../lib' "$appdir/usr/bin/rsc2"
else
    log "patchelf not found: relying on AppRun's LD_LIBRARY_PATH"
fi

# ── the AppImage ────────────────────────────────────────────────────
version=${VERSION:-}
if [ -z "$version" ] && git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    version=$(git -C "$root" describe --tags --always --dirty 2>/dev/null || true)
fi
version=${version:-0.0.0}
version=${version//[^A-Za-z0-9._+-]/-}
out=${OUT:-$dist/rsc2-$version-x86_64.AppImage}

mkdir -p "$(dirname "$out")"
rm -f "$out"
log "assembling $out"
ARCH=x86_64 VERSION="$version" \
    "$ait" --no-appstream --runtime-file "$cache/downloads/runtime-x86_64" \
    "$appdir" "$out" >/dev/null

[ -x "$out" ] || die "appimagetool produced no $out"

echo
log "built $out"
printf '   size:   %s\n' "$(du -h "$out" | cut -f1)"
printf '   sha256: %s\n' "$(sha256sum "$out" | cut -d' ' -f1)"
if command -v objdump >/dev/null 2>&1; then
    printf '   glibc >= %s; SDL2 bundled, Vulkan loader/driver from the system\n' \
        "$(objdump -T "$appdir/usr/bin/rsc2" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 | sed 's/GLIBC_//')"
fi
cat <<EOF

Put your game data in a "game" folder next to the AppImage and run it:

   mkdir -p "\$(dirname "$out")/game"
   cp -r /path/to/RalliSport2/* "\$(dirname "$out")/game/"
   "$out"

Or set RSC2_GAME_DIR to the folder holding default.xbe. Headless smoke test:

   RECOMP_FB_WINDOW=0 RECOMP_AUDIO=0 RECOMP_VK_SCALE=1 RECOMP_HDR=0 \\
   RECOMP_VK_MSAA=0 RECOMP_FULLSCREEN=0 \\
   RECOMP_FB_DUMP_AT=/tmp/rsc2-frames:8400 "$out"   # frame 8400: title screen

See rsc2/RUNNING.md, "AppImage".
EOF
