#!/bin/bash
# wide_shadow_check.sh <recording.xi> [frame] [build dir]
#
# Detector for the ultrawide SS1 shadow ghosts (screen-space shadow buffer
# 83E78000 looked up unsqueezed: caster silhouettes appear (x-320)(k-1)
# title px toward the centre). Runs the recording headless at 21:9,
# captures the presented frame at render scale, and checks:
#
#   1. no translated program failed to compile (a broken k_prog_tail sends
#      every program to the CPU path, which drops the wide-screen squeeze
#      and so hides this bug -- an earlier attempt "passed" that way);
#   2. the ghost box is not darker than the ground beside it.
#
# The recording is an input recording: run the game once with
# RSC2_INPUT_RECORD=<file.xi>, go to Time Attack, Australia SS1 Copper
# Hill and wait on the start line. The box defaults are for one such
# recording at frame 1975: the cone's ghost, title px 160-175 x 268-300,
# against 182-197 x 268-300. Measured: broken 0.82, fixed 0.99. For
# another recording pick the frame and override GHOST_BOX / REF_BOX
# ("x0 y0 x1 y1", title px).
#
# The ghosts came from the renderer's squeeze. By default the game projects
# wide itself and nothing is squeezed, so run it with RSC2_WIDE_NATIVE=0 as
# well to test the squeeze path.
# Needs ImageMagick. Exit 0 = pass.
set -u
rec=$(realpath "${1:?usage: wide_shadow_check.sh <recording.xi> [frame] [build dir]}")
frame=${2:-1975}
build=${3:-$(cd "$(dirname "$0")/../build" && pwd)}
read -r gx0 gy0 gx1 gy1 <<< "${GHOST_BOX:-160 268 175 300}"
read -r rx0 ry0 rx1 ry1 <<< "${REF_BOX:-182 268 197 300}"
out=$(mktemp -d)
pid=
trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; rm -rf "$out"' EXIT

cd "$build" || exit 2
env RECOMP_FB_WINDOW=0 RECOMP_AUDIO=0 RECOMP_WIDESCREEN=1 \
    RECOMP_VIEW_ASPECT=2.3889 RECOMP_VK_SCALE=3 \
    RSC2_INPUT_REPLAY="$rec" RECOMP_VK_HIRES_DUMP="$out/hires.bmp:$frame" \
    ./rsc2 --no-launcher > "$out/log.txt" 2>&1 &
pid=$!   # only the game this script started
for _ in $(seq 1 600); do
    sleep 1
    [ -s "$out/hires.bmp" ] && { sleep 3; break; }
done
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; pid=
[ -s "$out/hires.bmp" ] || { echo "FAIL: no frame $frame captured"; exit 2; }

fails=$(grep -c 'did not compile' "$out/log.txt")
sx=$(grep -o 'widescreen: [0-9]*x across, [0-9]*x down' "$out/log.txt" | head -1)
ax=$(echo "$sx" | awk '{print $2+0}'); ay=$(echo "$sx" | awk '{print $4+0}')
[ -n "$ax" ] && [ "$ax" -gt 0 ] || { echo "FAIL: renderer not widened ($sx)"; exit 1; }
mean() {  # title-pixel box -> mean grey, 0-255
    convert "$out/hires.bmp" -crop $((($3-$1)*ax))x$((($4-$2)*ay))+$(($1*ax))+$(($2*ay)) \
        -colorspace Gray -format '%[fx:255*mean]' info:
}
g=$(mean "$gx0" "$gy0" "$gx1" "$gy1"); r=$(mean "$rx0" "$ry0" "$rx1" "$ry1")
ratio=$(awk -v g="$g" -v r="$r" 'BEGIN { printf "%.4f", (r > 0 ? g / r : 0) }')
printf 'compile failures %d, ghost %.1f, beside %.1f, ratio %.2f (pass >= 0.93)\n' \
    "$fails" "$g" "$r" "$ratio"
[ "$fails" -eq 0 ] || { echo "FAIL: programs did not compile"; exit 1; }
awk -v v="$ratio" 'BEGIN { exit !(v >= 0.93) }' || { echo "FAIL: shadow ghost present"; exit 1; }
echo PASS
