#!/usr/bin/env python3
"""Seed the real functions hiding among the lifter's unresolved-call stubs.

tools.recomp stubs every call/jump target that tools.disasm did not detect as
a function (recomp_stubs_unresolved.c). A stub only pops the return address,
so a *real* function behind one silently does nothing. RalliSport Challenge 2
hit exactly that: zlib's hand-written MASM inflate_fast (inffas32.asm) opens
with `jmp L_do_fast` over its string table, the body at 0x000A0EA0 was never
found, inflate made no progress and boot spun forever.

Most of the stubs, though, are targets of "calls" inside data that was swept
as code -- seeding those would split real functions. A target is kept only if

  1. some reference to it is *reachable* from the entry of the function that
     contains it, following fall-through and direct branches (data decoded as
     code after an unconditional jmp/ret is not reachable), and
  2. the disassembler's own probe reads it as a function body (the same gate
     tools.seed_from_log applies), and
  3. it lies in a gap between detected functions. A target *inside* a known
     body is the back-edge of a loop seen from a tail_jump_alias entry (the
     alias starts mid-loop, so its branch back leaves the alias); the
     enclosing function lifts that branch as a local goto, and seeding the
     target would only split the loop. --include-inside overrides this.

Run from the xboxrecomp checkout (it imports tools.disasm):

    python3 ../rsc2/tools/seed_unresolved.py game_files/default.xbe \
        ../rsc2/src/recomp/gen/recomp_stubs_unresolved.c \
        --seeds ../rsc2/config/seed_functions.json [--dry-run]
"""

import argparse
import bisect
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

from tools.disasm.engine import DisasmEngine
from tools.disasm.loader import DATA_SECTION_NAMES, load_image

OUT = Path("tools/disasm/output")
STUB_RE = re.compile(r"^void sub_([0-9A-F]{8})\(void\)", re.M)
INSN_RE = re.compile(r"^\s*0x([0-9A-Fa-f]{8})\s+[0-9a-f]+\s+(\S+)\s*(.*?)\s*(;.*)?$")

# Flagged executable in the XBE but holding data: .rdata starts with the
# kernel thunk table, whose entries decode as code and must never be seeded.
NOT_CODE = {".rdata", ".data", ".data1", "BINKDATA", "XON_RD", "DOLBY", "DIRTY"}

ENDS_FLOW = ("ret", "retn", "retf", "jmp", "ljmp", "int3", "hlt", "iretd", "ud2")


def load_listing():
    """Address-ordered (va, mnemonic, operand) for every swept instruction."""
    insns = []
    for p in sorted((OUT / "asm").glob("*.asm")):
        for ln in p.read_text().splitlines():
            m = INSN_RE.match(ln)
            if m:
                insns.append((int(m.group(1), 16), m.group(2), m.group(3)))
    insns.sort()
    return insns


def reachable(func_start, func_end, insns, addrs):
    """Instruction addresses reachable from func_start inside [start, end)."""
    seen, todo = set(), [func_start]
    while todo:
        va = todo.pop()
        if va in seen or not (func_start <= va < func_end):
            continue
        i = bisect.bisect_left(addrs, va)
        if i >= len(addrs) or addrs[i] != va:
            continue  # branch into the middle of a decoded instruction
        seen.add(va)
        _, mn, op = insns[i]
        if mn.startswith("j"):
            t = re.fullmatch(r"0x([0-9a-f]+)", op)
            if t:
                todo.append(int(t.group(1), 16))
        if mn not in ENDS_FLOW and i + 1 < len(addrs):
            todo.append(addrs[i + 1])
    return seen


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xbe", type=Path)
    ap.add_argument("stubs", type=Path, help="recomp_stubs_unresolved.c")
    ap.add_argument("--seeds", type=Path, required=True)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--include-inside", action="store_true",
                    help="also seed targets inside a detected function body")
    args = ap.parse_args(argv)

    targets = sorted(int(a, 16) for a in STUB_RE.findall(args.stubs.read_text()))
    funcs = json.loads((OUT / "functions.json").read_text())
    as_int = lambda v: int(v, 0) if isinstance(v, str) else v
    bounds = sorted((as_int(f["start"]), as_int(f["end"])) for f in funcs)
    starts = [b[0] for b in bounds]

    refs = defaultdict(list)
    for x in json.loads((OUT / "xrefs.json").read_text()):
        if x["type"] in ("call", "jump", "cond_jump"):
            refs[int(x["to"], 16)].append(int(x["from"], 16))

    insns = load_listing()
    addrs = [i[0] for i in insns]

    image = load_image(str(args.xbe))
    engine = DisasmEngine(image)
    for s in image.sections:
        if s.executable and s.name not in DATA_SECTION_NAMES:
            engine.linear_sweep(s)

    existing = json.loads(args.seeds.read_text()) if args.seeds.exists() else []
    have = {int(e["start"], 16) for e in existing}

    reach_cache = {}
    kept, junk, noprobe, inside, notcode = [], 0, 0, 0, 0
    for t in targets:
        if t in have:
            continue
        sec = image.get_section_at_va(t)
        if sec is None or sec.name in NOT_CODE or sec.name.startswith("$$"):
            notcode += 1
            continue
        k = bisect.bisect_right(starts, t) - 1
        if (not args.include_inside and k >= 0
                and bounds[k][0] < t < bounds[k][1]):
            inside += 1
            continue
        live = []
        for src in refs.get(t, []):
            k = bisect.bisect_right(starts, src) - 1
            if k < 0 or not (bounds[k][0] <= src < bounds[k][1]):
                continue
            if k not in reach_cache:
                reach_cache[k] = reachable(bounds[k][0], bounds[k][1], insns, addrs)
            if src in reach_cache[k]:
                live.append(src)
        if not live:
            junk += 1
            continue
        if not engine.probes_as_function_body(t):
            noprobe += 1
            print("  - %08X  referenced live from %08X but does not probe as a body"
                  % (t, live[0]))
            continue
        print("  + %08X  referenced live from %s" % (t, ", ".join("%08X" % s for s in live[:4])))
        kept.append({"start": "0x%08X" % t,
                     "note": "unresolved-call stub with a reachable reference "
                             "(first from 0x%08X); decodes as a function body." % live[0]})

    print("targets %d: seed %d, inside a known body %d, in a data section %d, "
          "junk-only references %d, failed probe %d"
          % (len(targets), len(kept), inside, notcode, junk, noprobe))
    if args.dry_run or not kept:
        return 0
    existing.extend(kept)
    args.seeds.write_text(json.dumps(existing, indent=1))
    print("seeds: %d -> %s" % (len(existing), args.seeds))
    return 0


if __name__ == "__main__":
    sys.exit(main())
