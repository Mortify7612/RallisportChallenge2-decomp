#!/bin/bash
# hud_check.sh <recording.xi> [build dir] [frame=1300]
#
# Detector for hiding the race HUD (RSC2_HUD=0, the launcher's Race HUD row,
# F4 in the window; issue #16). The game's own Options > HUD turns every
# piece off but Time Attack's RACE TIME box, so there was no way to a clean
# screen in Time Attack.
#
# Two headless replays of a Time Attack run (ss1b.xi, Copper Hill; the car
# stays on the line, so the frames match but for the clock) to <frame>, in
# the race: HUD shown (the default) and RSC2_HUD=0. The RACE TIME box (top
# right of the 640x480 frame) must be gone in the second: the crop differs
# from the first in more than 1500 pixels. The road below the car must not
# differ (under 300), or the two runs are not showing the same thing.
# A build that ignores the variable fails: both runs show the box.
# To make the recording, run the game with RSC2_INPUT_RECORD=ss1b.xi from
# start-up into a Time Attack race on Copper Hill and drive on past <frame>.
# Needs ImageMagick. Exit 0 = pass. HUD_KEEP=1 keeps the frames.
set -u
rec=$(realpath "${1:?usage: hud_check.sh <recording.xi> [build dir] [frame]}")
here=$(cd "$(dirname "$0")" && pwd)
build=${2:-$here/../build}
frame=${3:-1300}
out=$(mktemp -d)
[ -n "${HUD_KEEP:-}" ] && echo "output kept in $out" || trap 'rm -rf "$out"' EXIT

pids=()
run() {  # run <name> [VAR=value...]
    local name=$1; shift
    mkdir -p "$out/$name" "$out/config"   # empty settings: a saved Race HUD row stays out
    (cd "$build" && exec env "$@" XDG_CONFIG_HOME="$out/config" RSC2_INPUT_REPLAY="$rec" RECOMP_FB_WINDOW=0 RECOMP_AUDIO=0 \
        RECOMP_FB_DUMP_AT="$out/$name:$frame" timeout 900 ./rsc2 --no-launcher \
        > "$out/$name.log" 2>&1) &
    pids+=($!)  # timeout (exec'd), which passes a TERM on to the game
}
f=$(printf 'frame%06d.bmp' $frame)
# One at a time: two at once sat on the intro movie past the frame (the
# replay is driven by input calls, and two games sharing the CPU and the
# game directory drift).
for n in shown hidden; do
    if [ $n = shown ]; then run shown; else run hidden RSC2_HUD=0; fi
    for _ in $(seq 900); do [ -s "$out/$n/$f" ] && break; sleep 1; done
    sleep 2
    kill "${pids[@]}" 2>/dev/null   # only the games this script started
    wait 2>/dev/null
    pids=()
done
for n in shown hidden; do
    [ -s "$out/$n/$f" ] || { echo "FAIL: the $n run did not reach frame $frame"; exit 2; }
done
# Pixels whose largest channel differs by more than 10%. compare -fuzz
# counts differently in ImageMagick 6 and 7 (the same two frames: 3219 and
# 876), so the count is made from a difference image, which both agree on.
diffcount() {
    convert "$1" "$2" -compose difference -composite -separate -evaluate-sequence max \
        -threshold 10% -format '%[fx:round(mean*w*h)]' info: 2>/dev/null
}
cmp_crop() {  # cmp_crop <geometry>
    convert "$out/shown/$f" -crop "$1" +repage "$out/a.png" 2>/dev/null
    convert "$out/hidden/$f" -crop "$1" +repage "$out/b.png" 2>/dev/null
    diffcount "$out/a.png" "$out/b.png"
}
box=$(cmp_crop "${HUD_BOX:-110x42+486+58}")
road=$(cmp_crop 200x40+220+430)
echo "pixels differing: RACE TIME box $box (need > 1500), road $road (need < 300)"
[ "${road%%.*}" -lt 300 ] || { echo "FAIL: the two runs show different scenes"; exit 2; }
[ "${box%%.*}" -gt 1500 ] || { echo "FAIL: RSC2_HUD=0 still shows the RACE TIME box"; exit 1; }
echo PASS
