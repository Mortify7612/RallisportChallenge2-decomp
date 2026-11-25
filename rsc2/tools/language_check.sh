#!/bin/bash
# language_check.sh <recording.xi> [build dir]
#
# Detector for the game's language (RECOMP_LANGUAGE, issue #11: the port
# always told the game the console was set to English, so its French,
# German, Italian, Japanese and Spanish text, fonts and speech were
# unreachable).
#
# Three headless replays to the loading-tip screen (frame 776 of ss1b.xi,
# a Time Attack run started from boot): the default, RECOMP_LANGUAGE=english
# and RECOMP_LANGUAGE=german. The button-hint bar at the bottom ("Select /
# Quit / Previous tip / Next tip") must differ between the default and
# German -- in German it reads "Wählen / Beenden / ..." -- and English set
# by name must match the default exactly there. The tip text above it is
# picked at random, so it is left out.
# A build that ignores the variable fails: every run shows English.
# For a recording that reaches that screen at another frame, change frame=
# below. Saved launcher settings apply to these runs too; point
# XDG_CONFIG_HOME at an empty directory to run without them.
# Needs ImageMagick (6 or 7). Exit 0 = pass. LANGUAGE_KEEP=1 keeps the frames.
set -u
rec=$(realpath "${1:?usage: language_check.sh <recording.xi> [build dir]}")
here=$(cd "$(dirname "$0")" && pwd)
build=${2:-$here/../build}
frame=776
out=$(mktemp -d)
[ -n "${LANGUAGE_KEEP:-}" ] && echo "output kept in $out" || trap 'rm -rf "$out"' EXIT

run() {  # run <name> [VAR=value...]
    local name=$1; shift
    mkdir -p "$out/$name"
    (cd "$build" && exec env "$@" RSC2_INPUT_REPLAY="$rec" RECOMP_FB_WINDOW=0 RECOMP_AUDIO=0 \
        RECOMP_FB_DUMP_AT="$out/$name:$frame" timeout 600 ./rsc2 --no-launcher \
        > "$out/$name.log" 2>&1) &
}
run default
run english RECOMP_LANGUAGE=english
run german RECOMP_LANGUAGE=german
f=$(printf 'frame%06d.bmp' $frame)
for _ in $(seq 600); do
    [ -s "$out/default/$f" ] && [ -s "$out/english/$f" ] && [ -s "$out/german/$f" ] && break
    sleep 1
done
sleep 2
pkill -x rsc2
wait 2>/dev/null
for n in default english german; do
    [ -s "$out/$n/$f" ] || { echo "FAIL: the $n run did not reach frame $frame"; exit 2; }
done
bar() { convert "$out/$1/$f" -crop 600x30+40+414 +repage "$out/$1-bar.png"; }
bar default; bar english; bar german
# Pixels whose largest channel differs by more than 10%. compare -fuzz
# counts differently in ImageMagick 6 and 7 (IM7 reports a fraction of
# IM6's count for the same frames, and prints "N (normalised)"), so the
# count is made from a difference image, which both agree on.
diffcount() {
    convert "$1" "$2" -compose difference -composite -separate -evaluate-sequence max \
        -threshold 10% -format '%[fx:round(mean*w*h)]' info: 2>/dev/null
}
de=$(diffcount "$out/default-bar.png" "$out/german-bar.png")
en=$(diffcount "$out/default-bar.png" "$out/english-bar.png")
echo "button bar pixels differing: German vs default $de (need > 1000), English vs default $en (need < 50)"
[ "${de%%.*}" -gt 1000 ] || { echo "FAIL: RECOMP_LANGUAGE=german still shows English"; exit 1; }
[ "${en%%.*}" -lt 50 ] || { echo "FAIL: RECOMP_LANGUAGE=english is not the default"; exit 1; }
echo PASS
