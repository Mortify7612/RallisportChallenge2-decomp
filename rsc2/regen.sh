#!/usr/bin/env bash
# Regenerate the lifted C for RalliSport Challenge 2 from default.xbe.
# Run from anywhere; re-run after editing config/seed_functions.json or
# adding a sub_XXXXXXXX to src/rsc2_overrides.c.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TK="$HERE/../xboxrecomp"
PY="$HERE/../.venv/bin/python"
cd "$TK"
"$PY" -m tools.disasm game_files/default.xbe --seed-functions "$HERE/config/seed_functions.json"
"$PY" -m tools.func_id game_files/default.xbe
"$PY" -m tools.abi_analysis game_files/default.xbe
# Optional: functions to emit entry/exit hooks for (RECOMP_SNAPSHOT,
# RECOMP_TRACE_*). A JSON list of addresses; absent or [] means none.
TRACE=()
if [ -s "$HERE/config/trace_functions.json" ]; then
    TRACE=(--trace-functions "$HERE/config/trace_functions.json")
fi
"$PY" -m tools.recomp game_files/default.xbe --all --split 250 \
    --exclude-manual "$HERE/src/rsc2_overrides.c" \
    "${TRACE[@]}" \
    --gen-dir "$HERE/src/recomp/gen"
