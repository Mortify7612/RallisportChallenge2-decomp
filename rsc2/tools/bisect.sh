#!/usr/bin/env bash
# Snapshot each listed function at its n-th call and replay it natively.
#   tools/bisect.sh <nth> <va-hex>...
# Every function must be in config/trace_functions.json (regen.sh + rebuild).
# Prints, per function, SIGNAL = bytes both runs wrote differently plus bytes
# only the native run wrote. Bytes only the lifted run changed are mostly
# other guest threads working during the lifted call, so they are counted
# separately rather than as a lifting bug.
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
NTH="$1"; shift
for fn in "$@"; do
    d="${SNAP_ROOT:-/tmp/rsc2-snap}/$fn"
    mkdir -p "$d"; rm -f "$d"/pre.bin "$d"/post.bin
    (cd "$HERE/build" && RECOMP_FB_WINDOW=0 RECOMP_SNAPSHOT="$fn:$NTH" RECOMP_SNAPSHOT_DIR="$d" \
        timeout "${SNAP_TIMEOUT:-60}" ./rsc2 > /dev/null 2>&1)
    if [ ! -s "$d/post.bin" ]; then echo "$fn: no snapshot (fewer than $NTH calls?)"; continue; fi
    out=$("$HERE/tools/replay32/replay32" "$fn" "$d/pre.bin" "$d/post.bin" 100000)
    if echo "$out" | grep -q "DID NOT COMPLETE"; then echo "$fn: native run faulted"; continue; fi
    sig=$(echo "$out" | awk '/^SIGNAL/ {print $2}')
    tot=$(echo "$out" | grep "^total")
    echo "$fn: SIGNAL $sig   [$tot]"
    rm -f "$d"/pre.bin "$d"/post.bin
done
