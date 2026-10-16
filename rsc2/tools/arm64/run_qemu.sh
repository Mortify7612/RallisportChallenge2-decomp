#!/bin/sh
# Run the ARM64 test build under qemu-aarch64 (user-mode emulation).
#
#   rsc2/tools/arm64/run_qemu.sh [env...]
#
# Runs rsc2/build-arm64/rsc2 from that directory, so its `game` link is the
# game folder, against the sysroot it was built with. Extra arguments are
# environment assignments, e.g. a headless replay with the software renderer:
#
#   rsc2/tools/arm64/run_qemu.sh RECOMP_FB_WINDOW=0 RECOMP_VULKAN=0 \
#       RECOMP_AUDIO=0 RSC2_INPUT_REPLAY=../../work/my_run2.xi
#
# The Vulkan renderer starts on Mesa's software rasteriser (lavapipe, in the
# sysroot) with VK_ICD_FILENAMES=$RSC2_SYSROOT/usr/share/vulkan/icd.d/lvp_icd.json;
# its output has not been compared yet.
#
# qemu translates ARM code on this x86 host with x86's strong memory ordering,
# so a missing barrier between guest threads cannot show up here; only real
# ARM hardware finds those.
set -e
here=$(cd "$(dirname "$0")/../../.." && pwd)
RSC2_SYSROOT=${RSC2_SYSROOT:-$here/third_party/sysroot-aarch64}
cd "$here/rsc2/build-arm64"
exec env "$@" qemu-aarch64 -L "$RSC2_SYSROOT" ./rsc2
