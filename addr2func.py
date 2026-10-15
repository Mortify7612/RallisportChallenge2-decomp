#!/usr/bin/env python3
"""Map guest return addresses to the containing function and print the asm
around each call site. Usage: addr2func.py ADDR [ADDR...] [-c N]"""
import bisect, json, sys
from pathlib import Path
TK = Path(__file__).resolve().parent / "xboxrecomp"
F = json.load(open(TK / "tools/disasm/output/functions.json"))
F = F if isinstance(F, list) else F.get("functions", F)
if isinstance(F, dict): F = list(F.values())
def a(x):
    v = x.get("start", x.get("address")); return int(v, 16) if isinstance(v, str) else v
F.sort(key=a); starts = [a(x) for x in F]
ctx = 0
args = sys.argv[1:]
if "-c" in args:
    i = args.index("-c"); ctx = int(args[i + 1]); del args[i:i + 2]
asm = {}
for p in (TK / "tools/disasm/output/asm").glob("*.asm"):
    for ln in p.read_text().splitlines():
        s = ln.strip()
        if s.startswith("0x"):
            try: asm[int(s.split()[0], 16)] = s
            except ValueError: pass
keys = sorted(asm)
for s in args:
    v = int(s, 16); i = bisect.bisect_right(starts, v) - 1
    f = F[i] if i >= 0 else None
    print(f"{v:08X}  in {f.get('name') if f else '?'} (+0x{v - a(f):X}, size {f.get('size')})")
    if ctx:
        k = bisect.bisect_left(keys, v)
        for j in range(max(0, k - ctx), min(len(keys), k + 2)):
            print("      " + asm[keys[j]])
