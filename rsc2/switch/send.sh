#!/bin/sh
# Send rsc2.nro to a Switch in hbmenu's netloader (press Y there) and stream
# its stdout and stderr back here, with devkitPro's nxlink:
#
#   rsc2/switch/send.sh [switch-ip] [NAME=value ...]
#
# NAME=value arguments are settings for that run, over the SD card's
# rsc2.env (RECOMP_VULKAN=1 for the OpenGL renderer, say). The output is also
# kept in $LOG (default work/nxlink.log). Without an address nxlink looks for
# the Switch by broadcast.
#
# The .nro is written to sdmc:/switch/rsc2/rsc2.nro first (-p is relative to
# sdmc:/switch/, and the folder must exist), so the game finds its files next
# to it as when started from hbmenu. nxlink -s keeps listening until the game
# exits; the Switch connects back to this machine on TCP 28771, which a
# firewall must let through. Runs in devkitPro's container like build_nro.sh
# when $DEVKITPRO is unset (host networking, for the broadcast and the
# connection back).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
nro=$root/rsc2/build-switch/rsc2.nro
LOG=${LOG:-$root/work/nxlink.log}

if [ -z "$DEVKITPRO" ]; then
    run=$(command -v podman || command -v docker) || {
        echo "send.sh: no DEVKITPRO and no podman/docker (see rsc2/switch/README.md)"; exit 1; }
    exec "$run" run --rm --network host -v "$root:/src:Z" -w /src \
        -e DEVKITPRO=/opt/devkitpro -e WAIT -e "LOG=/src/${LOG#"$root"/}" \
        "${DKP_IMAGE:-docker.io/devkitpro/devkita64}" sh /src/rsc2/switch/send.sh "$@"
fi

addr=
case "$1" in *=*|'') ;; *) addr=$1; shift ;; esac
[ -e "$nro" ] || { echo "send.sh: no $nro (run build_nro.sh)"; exit 1; }
# Until the netloader answers (for up to $WAIT seconds, default 300), so this
# can be started before pressing Y. Don't probe its port by hand: hbmenu
# leaves the netloader after any connection, upload or not.
end=$(( $(date +%s) + ${WAIT:-300} ))
# The last session's connection on 28771 lingers in TIME_WAIT for about a
# minute after it closes, and nxlink's server cannot bind until it is gone:
# the upload succeeded, then "bind listen socket: Address already in use",
# and the game's output went nowhere. Wait it out first (28771 = 0x7063).
i=0
while awk 'NR > 1 && $2 ~ /:7063$/ { f = 1 } END { exit !f }' /proc/net/tcp /proc/net/tcp6 2>/dev/null; do
    [ $i -eq 0 ] && echo "send.sh: waiting for the last session's port 28771 to close"
    i=$((i + 1))
    [ $i -lt 120 ] || break
    sleep 1
done
while :; do
    # Line-buffered, so the log keeps what came before a crash of nxlink.
    stdbuf -oL "$DEVKITPRO/tools/bin/nxlink" -s ${addr:+-a "$addr"} -p rsc2/rsc2.nro \
        "$nro" "$@" > "$LOG" 2>&1 || echo "send.sh: nxlink exited with $?" >> "$LOG"
    grep -q '^Connection to .* failed' "$LOG" && [ "$(date +%s)" -lt "$end" ] || break
    sleep 1
done &
tail --pid=$! -n +1 -f "$LOG" 2>/dev/null
