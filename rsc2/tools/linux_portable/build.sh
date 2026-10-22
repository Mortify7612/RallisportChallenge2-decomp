#!/bin/bash
# Portable Linux build of rsc2, for releases (issue #9).
#
# A build on the development machine needs that machine's glibc (2.4x) and
# its shared glslang, so it does not start on Linux Mint, Ubuntu LTS or
# Debian. This builds in an Ubuntu 22.04 container instead (podman): glibc
# 2.35, glslang and libstdc++ linked in, and only libraries every desktop
# has left dynamic (SDL2, the Vulkan loader, libepoxy).
#
#   rsc2/tools/linux_portable/build.sh [jobs]
#
# Needs podman, the generated code in rsc2/src/recomp (rsc2/regen.sh), the
# glslang source in third_party/src/glslang (see RUNNING.md) and the Vulkan
# headers in /usr/include. Output: rsc2/build-linux/rsc2. glslang is built
# once into third_party/linux.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
jobs=${1:-$(( $(nproc) / 2 ))}
tp=$(realpath "$root/third_party")

[ -d "$root/rsc2/src/recomp/gen" ] || { echo "no generated code: run rsc2/regen.sh first"; exit 1; }
[ -f "$tp/src/glslang/CMakeLists.txt" ] || { echo "no glslang source in $tp/src/glslang"; exit 1; }

podman build -q -t rsc2-linux-build -f "$here/Containerfile" "$here" >/dev/null

# The renderer wants newer Vulkan headers than Ubuntu 22.04 has; headers
# only, so the host's do.
mkdir -p "$tp/linux/include"
cp -r /usr/include/vulkan /usr/include/vk_video "$tp/linux/include/"

podman run --rm --userns=keep-id -v "$root:$root" -v "$tp:$tp" -w "$root" \
    -e TP="$tp/linux" -e JOBS="$jobs" -e HERE="$here" rsc2-linux-build bash -euo pipefail -c '
if [ ! -f "$TP/lib/libglslang.a" ] || [ ! -x "$TP/bin/glslangValidator" ]; then
    cmake -S third_party/src/glslang -B "$TP/build-glslang" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$TP" \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DENABLE_OPT=OFF -DENABLE_HLSL=OFF -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=ON
    ninja -C "$TP/build-glslang" -j "$JOBS" install
fi
L=$TP/lib
cmake -S rsc2 -B rsc2/build-linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DVulkan_INCLUDE_DIR="$TP/include" \
    -DVulkan_LIBRARY=/usr/lib/x86_64-linux-gnu/libvulkan.so.1 \
    -DXBOXRECOMP_GLSLC="$HERE/glslc.py" \
    -DXBOXRECOMP_GLSLANG_INCLUDE="$TP/include" \
    -DXBOXRECOMP_GLSLANG_LIB="$L/libglslang.a" \
    -DXBOXRECOMP_GLSLANG_RL_LIB="$L/libglslang-default-resource-limits.a" \
    "-DXBOXRECOMP_GLSLANG_EXTRA_LIBS=$L/libSPIRV.a;$L/libMachineIndependent.a;$L/libOSDependent.a;$L/libGenericCodeGen.a;$L/libMachineIndependent.a;$(g++ -print-file-name=libstdc++.a);pthread"
GLSLANG_VALIDATOR="$TP/bin/glslangValidator" ninja -C rsc2/build-linux -j "$JOBS"
'

bin=$root/rsc2/build-linux/rsc2
echo "== $bin"
readelf -d "$bin" | sed -n 's/.*NEEDED.*\[\(.*\)\]/  needs \1/p'
echo "  newest glibc symbol: $(objdump -T "$bin" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
