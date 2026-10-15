#!/bin/sh
# Start RalliSport Challenge 2 (Linux build).
#
# On a PRIME laptop/desktop whose primary GPU is an integrated one, the
# discrete NVIDIA GPU has to be asked for: prime-run offloads to it, and an
# inherited VK_ICD_FILENAMES (pinning the Vulkan loader to another vendor's
# driver) is dropped so the NVIDIA driver is visible at all.
# RSC2_GPU=igpu skips the offload. Extra arguments go to the game.
cd "$(dirname "$0")/build" || exit 1
if [ "${RSC2_GPU:-}" != "igpu" ] && command -v prime-run >/dev/null 2>&1 \
        && [ -e /dev/nvidiactl ]; then
    exec prime-run env -u VK_ICD_FILENAMES ./rsc2 "$@"
fi
exec ./rsc2 "$@"
