#!/usr/bin/env python3
"""Replay every batch snapshot natively and rank functions by divergence.

    replay_sweep.py <snapdir> [--keep]

<snapdir>/<VA>/{pre,post}.bin come from RECOMP_SNAPSHOT_LIST runs. For each,
tools/replay32/replay32 runs the original x86 and diffs it against the lifted
result. Output: one line per function -- SIGNAL bytes, register mismatches,
whether the native run completed -- sorted with the most suspicious first.
Snapshots are deleted after replay unless --keep (they are 256 MB a pair).
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPLAY = os.path.join(HERE, "replay32", "replay32")


def main():
    d = sys.argv[1]
    keep = "--keep" in sys.argv
    rows = []
    for va in sorted(os.listdir(d)):
        pre, post = os.path.join(d, va, "pre.bin"), os.path.join(d, va, "post.bin")
        if not (os.path.exists(pre) and os.path.exists(post)):
            continue
        env = dict(os.environ, REPLAY_IGNORE_EBP="1")
        try:
            r = subprocess.run([REPLAY, va, pre, post, "6"], capture_output=True,
                               text=True, timeout=60, env=env)
            out = r.stdout + r.stderr
        except subprocess.TimeoutExpired:
            out = "TIMEOUT"
        m = re.search(r"SIGNAL (\d+)", out)
        sig = int(m.group(1)) if m else -1
        # eax is the return value; esp/ebp differ by design (ret, frames).
        regmis = sum(1 for l in out.splitlines()
                     if re.match(r"^  (eax|ecx|edx|ebx|esi|edi|st0|xmm[0-7]) ", l) and "differs" in l)
        done = "DID NOT COMPLETE" not in out and "TIMEOUT" not in out
        fault = re.search(r"faulted: .*", out)
        rows.append((sig, regmis, done, va, fault.group(0) if fault else ""))
        with open(os.path.join(d, va, "replay.txt"), "w") as f:
            f.write(out)
        if not keep:
            os.remove(pre)
            os.remove(post)
        print(f"{va} SIGNAL {sig} regs {regmis} {'ok' if done else 'INCOMPLETE'} "
              f"{fault.group(0) if fault else ''}", flush=True)
    rows.sort(key=lambda r: (not r[2], -(r[0] > 0) - (r[1] > 0), -r[0]))
    print("\n== ranked (completed runs with differences first) ==")
    for sig, regmis, done, va, fault in rows:
        if done and (sig > 0 or regmis > 0):
            print(f"{va} SIGNAL {sig} regs {regmis}")


if __name__ == "__main__":
    main()
