#!/bin/sh
# Build the Nintendo Switch homebrew (rsc2.nro) with devkitPro's devkitA64
# and libnx. Without a devkitPro install ($DEVKITPRO unset) it runs itself in
# devkitPro's container image with podman (or docker), which needs no root:
#
#   rsc2/switch/build_nro.sh             -> rsc2/build-switch/rsc2.nro
#
# Environment: DKP_IMAGE (default docker.io/devkitpro/devkita64),
# RSC2_SWITCH_CFLAGS (default -g: the .elf keeps symbols for addr2line on
# crash reports; the .nro is stripped either way). See README.md.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)

if [ -z "$DEVKITPRO" ]; then
    run=$(command -v podman || command -v docker) || {
        echo "build_nro.sh: no DEVKITPRO and no podman/docker (see rsc2/switch/README.md)"; exit 1; }
    exec "$run" run --rm -v "$root:/src:Z" -w /src \
        -e RSC2_SWITCH_CFLAGS -e JOBS -e MAKEFLAGS \
        "${DKP_IMAGE:-docker.io/devkitpro/devkita64}" sh /src/rsc2/switch/build_nro.sh
fi

OUT=$root/rsc2/build-switch
cmake -S "$root/rsc2" -B "$OUT" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG ${RSC2_SWITCH_CFLAGS:--g}" \
    > "$OUT.configure.log" || { cat "$OUT.configure.log"; exit 1; }
make -C "$OUT" -j"${JOBS:-$(nproc)}" --no-print-directory
[ -e "$OUT/rsc2.nro" ] && ls -la "$OUT/rsc2.nro"
