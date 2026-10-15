#!/usr/bin/env python3
"""Instrument (or un-instrument) generated sources with per-function call counters.

    callcount_instrument.py on  src/recomp/gen    # add counters
    callcount_instrument.py off src/recomp/gen    # remove them

Each `void sub_XXXXXXXX(void)\\n{` gains one line that counts the call and, on
the first call, registers the counter with rsc2_callcount.c. Idempotent; a
regen (regen.sh) also removes the instrumentation.
"""
import pathlib
import re
import sys

MARK = "/*CC*/"
DECL = ("void rsc2_callcount_register(unsigned va, volatile unsigned long *n);"
        " " + MARK + "\n")
FN = re.compile(r"^(void sub_([0-9A-F]{8})\(void\)\n\{\n)", re.M)


def instrument(text):
    if MARK in text:
        return text
    text = FN.sub(lambda m: m.group(1) +
                  "    { static volatile unsigned long cc_; if (cc_++ == 0) "
                  "rsc2_callcount_register(0x%su, &cc_); } %s\n" % (m.group(2), MARK),
                  text)
    return DECL + text


def strip(text):
    return "".join(l for l in text.splitlines(keepends=True) if MARK not in l)


def main():
    mode, d = sys.argv[1], pathlib.Path(sys.argv[2])
    n = 0
    for p in sorted(d.glob("recomp_*.c")):
        if p.name in ("recomp_dispatch.c",):
            continue
        t = p.read_text()
        u = instrument(t) if mode == "on" else strip(t)
        if u != t:
            p.write_text(u)
            n += 1
    print(f"{mode}: {n} files changed")


if __name__ == "__main__":
    main()
