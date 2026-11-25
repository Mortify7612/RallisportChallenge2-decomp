#!/bin/bash
# fov_check.sh <recording.xi> [build dir] [frame=1300]
#
# Detector for the field of view setting (RSC2_FOV, the launcher's Field of
# view row; issue #10). The race view's 70 degrees was fixed.
#
# Three headless replays of a Time Attack recording to <frame>: the default,
# RSC2_FOV=70 (the original, written out) and RSC2_FOV=90. At <frame> the
# race view must be up with the car still on the start line, so the camera
# holds still. Make one with RSC2_INPUT_RECORD=<file>.xi rsc2/run.sh: go
# into a Time Attack race and wait on the line; replay it with
# RECOMP_FB_DUMP_AT=<dir>:<n> to find a frame where the car sits there. The
# car is drawn narrower at 90: its rear (the red of the bumper and lights in
# the bottom half of the 640x480 frame) is measured as the widest row of
# strongly red pixels. Pass: 70 matches the default within 3 pixels, and 90
# is at least 15% narrower (tan(35)/tan(45) predicts 30%). A build that
# ignores the variable fails: all three are the same width.
# The runs read no saved launcher settings (XDG_CONFIG_HOME is an empty
# directory), so a saved Field of view does not change the default run.
# Needs ImageMagick and python3. Exit 0 = pass. FOV_KEEP=1 keeps the frames.
set -u
rec=$(realpath "${1:?usage: fov_check.sh <recording.xi> [build dir] [frame]}")
here=$(cd "$(dirname "$0")" && pwd)
build=${2:-$here/../build}
frame=${3:-1300}
out=$(mktemp -d)
[ -n "${FOV_KEEP:-}" ] && echo "output kept in $out" || trap 'rm -rf "$out"' EXIT

pids=()
run() {  # run <name> [VAR=value...]
    local name=$1; shift
    mkdir -p "$out/$name" "$out/config"
    (cd "$build" && exec env "$@" RSC2_INPUT_REPLAY="$rec" RECOMP_FB_WINDOW=0 RECOMP_AUDIO=0 \
        XDG_CONFIG_HOME="$out/config" RECOMP_FB_DUMP_AT="$out/$name:$frame" \
        timeout 900 ./rsc2 --no-launcher > "$out/$name.log" 2>&1) &
    pids+=($!)  # timeout (exec'd), which passes a TERM on to the game
}
run default
run f70 RSC2_FOV=70
run f90 RSC2_FOV=90
f=$(printf 'frame%06d.bmp' $frame)
for _ in $(seq 900); do
    [ -s "$out/default/$f" ] && [ -s "$out/f70/$f" ] && [ -s "$out/f90/$f" ] && break
    sleep 1
done
sleep 2
kill "${pids[@]}" 2>/dev/null   # only the games this script started
wait 2>/dev/null
for n in default f70 f90; do
    [ -s "$out/$n/$f" ] || { echo "FAIL: the $n run did not reach frame $frame"; exit 2; }
done
python3 - "$out" "$f" <<'PY'
import subprocess, sys
out, f = sys.argv[1], sys.argv[2]
def width(name):
    raw = subprocess.run(["convert", "%s/%s/%s" % (out, name, f), "-depth", "8", "rgb:-"],
                         capture_output=True, check=True).stdout
    best = 0
    for y in range(240, 480):
        xs = [x for x in range(640)
              if raw[(y * 640 + x) * 3] > 150 and raw[(y * 640 + x) * 3] > 2 * raw[(y * 640 + x) * 3 + 1]
              and raw[(y * 640 + x) * 3] > 2 * raw[(y * 640 + x) * 3 + 2]]
        if len(xs) > 20:
            best = max(best, xs[-1] - xs[0])
    return best
d, a, b = width("default"), width("f70"), width("f90")
print("car width in pixels: default %d, RSC2_FOV=70 %d (need within 3), RSC2_FOV=90 %d (need <= %d)"
      % (d, a, b, int(d * 0.85)))
ok = d > 50 and abs(a - d) <= 3 and b <= d * 0.85
print("PASS" if ok else "FAIL: RSC2_FOV does not set the field of view")
sys.exit(0 if ok else 1)
PY
