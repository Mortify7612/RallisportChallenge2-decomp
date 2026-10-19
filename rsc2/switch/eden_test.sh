#!/bin/sh
# Run rsc2.nro in the Eden emulator with no window (eden-cli in a virtual X
# display), replaying recorded input, and collect what it left behind:
#
#   rsc2/switch/eden_test.sh [-t secs] [-d frames] [-r refdir] [-o outdir]
#                            [-x replay.xi] [NAME=value ...]
#
#   -t  how long to let it run (default 90 s; Eden starts in about 5)
#   -d  frames to dump as PNG, e.g. 300,600,900 (dumping copies every frame
#       back from the GPU, so leave it out when measuring speed)
#   -r  a previous run's folder: its PNGs are compared with this run's
#   -o  where to put the results (default work/eden-runs/<time>)
#   -x  the input recording to replay (default work/my_run2.xi)
#   NAME=value  settings for this run (RECOMP_GL_SPEC=0, ...)
#
# The results folder gets rsc2.log (the game's stderr), eden.log (Eden's),
# the dumped frames, and summary.txt: crashes, stalls, the last [PERF]
# reports and the frame comparison. Exits 1 if the game crashed.
#
# Setup, once (rsc2/switch/README.md, "Eden"): Eden's AppImage extracted to
# work/eden/AppDir (`./Eden.AppImage --appimage-extract` in work/eden), its
# keys in ~/.local/share/eden/keys, and the game in the SD folder below
# (game/ next to rsc2.nro, with copies of UDATA and TDATA, never the
# originals). The .nro is copied there from rsc2/build-switch on each run.
#
# eden-cli's -p does not pass arguments to homebrew, so this run's settings
# are written to rsc2.env on Eden's SD card; the file there is put back
# afterwards. Runs are deterministic only as far as the replay is: compare
# early frames, against references made the same way in Eden (the Eden SD
# card's saves differ from a PC's, so the menus do too).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
secs=90 dumps= ref= out= replay=$root/work/my_run2.xi
while getopts t:d:r:o:x: o; do
    case $o in
    t) secs=$OPTARG ;;
    d) dumps=$OPTARG ;;
    r) ref=$OPTARG ;;
    o) out=$OPTARG ;;
    x) replay=$OPTARG ;;
    *) sed -n '2,20p' "$0"; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

cli=${EDEN_CLI:-$root/work/eden/AppDir/bin/eden-cli}
sd=${EDEN_SD:-$HOME/.local/share/eden/sdmc/switch/rsc2}
nro=$root/rsc2/build-switch/rsc2.nro
out=${out:-$root/work/eden-runs/$(date +%Y%m%d-%H%M%S)}
[ -x "$cli" ] || { echo "eden_test.sh: no $cli (extract Eden's AppImage, see the README)"; exit 2; }
[ -d "$sd/game" ] || { echo "eden_test.sh: no game folder at $sd/game"; exit 2; }
[ -e "$nro" ] || { echo "eden_test.sh: no $nro (run build_nro.sh)"; exit 2; }
command -v xvfb-run >/dev/null || { echo "eden_test.sh: needs xvfb-run (Xvfb)"; exit 2; }
mkdir -p "$out"

cp "$nro" "$sd/rsc2.nro"
cp "$replay" "$sd/test.xi"
# This run's settings, over the SD card's own rsc2.env (put back on exit).
[ -e "$sd/rsc2.env" ] && cp "$sd/rsc2.env" "$out/rsc2.env.saved"
restore() {
    if [ -e "$out/rsc2.env.saved" ]; then cp "$out/rsc2.env.saved" "$sd/rsc2.env"; else rm -f "$sd/rsc2.env"; fi
}
trap restore EXIT INT TERM
{
    echo RECOMP_VULKAN=1
    echo RSC2_INPUT_REPLAY=/switch/rsc2/test.xi
    echo RECOMP_FLIP_HZ=60
    echo RECOMP_AUDIO=0
    echo RECOMP_PERF=1
    [ -n "$dumps" ] && echo "RECOMP_FB_DUMP_AT=/switch/rsc2/dumps:$dumps"
    for a in "$@"; do echo "$a"; done
} > "$sd/rsc2.env"
cp "$sd/rsc2.env" "$out/settings.env"
rm -rf "$sd/dumps"
mkdir -p "$sd/dumps"
rm -f "$sd/rsc2.log"

echo "eden_test.sh: running for ${secs}s -> $out"
# timeout ends xvfb-run, which takes Eden and the X server down with it.
timeout "$secs" xvfb-run -a -s "-screen 0 1280x720x24" "$cli" "$sd/rsc2.nro" \
    > "$out/eden.log" 2>&1 || true

cp "$sd/rsc2.log" "$out/rsc2.log" 2>/dev/null || : > "$out/rsc2.log"
for f in "$sd"/dumps/*.bmp; do
    [ -e "$f" ] || continue
    b=$(basename "$f" .bmp)
    magick "$f" "$out/$b.png" 2>/dev/null || cp "$f" "$out/"
done

crashed=0
{
    echo "settings:"; sed 's/^/  /' "$out/settings.env"
    if grep -qE '^\[CRASH\]|unhandled exception' "$out/rsc2.log"; then
        crashed=1
        echo "CRASHED:"
        grep -E -A3 '^\[CRASH\]|unhandled exception' "$out/rsc2.log" | head -12 | sed 's/^/  /'
    fi
    grep -E '^\[STALL\]' "$out/rsc2.log" | head -4
    grep -E '^\[GL\]' "$out/rsc2.log" | head -6
    echo "last reports:"
    grep -E '^\[PERF\] [0-9]|shaders:' "$out/rsc2.log" | tail -6 | cut -c1-200 | sed 's/^/  /'
    if [ -n "$ref" ]; then
        echo "frames against $ref (differing pixels):"
        for f in "$out"/frame*.png; do
            [ -e "$f" ] || continue
            b=$(basename "$f")
            if [ -e "$ref/$b" ]; then
                echo "  $b $(magick compare -metric AE -fuzz 8% "$ref/$b" "$f" null: 2>&1)"
            else
                echo "  $b (no reference)"
            fi
        done
    fi
} > "$out/summary.txt"
cat "$out/summary.txt"
exit $crashed
