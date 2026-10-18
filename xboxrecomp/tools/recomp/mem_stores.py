"""Write guest memory stores as MEMn_W(addr, value).

The lifter emits a store as an assignment to a memory macro,
``MEM32(addr) = value;``. The runtime (templates/runtime/recomp_types.h)
needs every store to be a call instead, so that one at or above 0xFD000000
-- a device register -- reaches the device model as it happens rather than
landing in memory. Assignment is the only store form the lifter produces (no
compound assignment, increment or address-of on a memory macro), so the
rewrite is purely textual, applied to each function's C before it is
written out.

Comments and string literals are skipped, and nested memory reads in the
address or the value are left as they are.
"""
import re

_MEM = re.compile(r"(S?MEM(?:8|16|32|64|F|D))\(")
_IDENT = re.compile(r"[A-Za-z0-9_]")


def _skip_literal(src, i):
    """Index just past the comment or string literal starting at i, or i."""
    if src.startswith("/*", i):
        j = src.find("*/", i + 2)
        return len(src) if j < 0 else j + 2
    if src.startswith("//", i):
        j = src.find("\n", i)
        return len(src) if j < 0 else j
    if src[i] in "\"'":
        q, j = src[i], i + 1
        while j < len(src) and src[j] != q:
            j += 2 if src[j] == "\\" else 1
        return j + 1
    return i


def _match_paren(src, i):
    """src[i - 1] is '('; the index just past its matching ')'."""
    depth = 1
    while depth and i < len(src):
        j = _skip_literal(src, i)
        if j != i:
            i = j
            continue
        c = src[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        i += 1
    return i


def _statement_end(src, i):
    """Index of the ';' ending the expression that starts at i."""
    depth = 0
    while i < len(src):
        j = _skip_literal(src, i)
        if j != i:
            i = j
            continue
        c = src[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == ";" and depth == 0:
            return i
        i += 1
    return -1


def route_mem_stores(src):
    out = []
    i, n = 0, len(src)
    while i < n:
        j = _skip_literal(src, i)
        if j != i:
            out.append(src[i:j])
            i = j
            continue
        m = _MEM.match(src, i)
        if m and (i == 0 or not _IDENT.match(src[i - 1])):
            close = _match_paren(src, m.end())
            k = close
            while k < n and src[k] in " \t":
                k += 1
            if k < n and src[k] == "=" and src[k + 1:k + 2] != "=":
                end = _statement_end(src, k + 1)
                if end > 0:
                    addr = route_mem_stores(src[m.end():close - 1])
                    value = route_mem_stores(src[k + 1:end].strip())
                    out.append(f"{m.group(1)}_W({addr}, {value});")
                    i = end + 1
                    continue
            out.append(src[i:m.end()])
            i = m.end()
            continue
        out.append(src[i])
        i += 1
    return "".join(out)
