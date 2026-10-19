#!/usr/bin/env python3
"""Name the Switch profiler's addresses (RSC2_NX_PROF=1, nx_prof.c).

    rsc2/switch/nx_prof.py [log] [--thread pb] [--skip N] [--reports M] [--top 40]

log is the game's stderr (work/nxlink.log by default). Each report covers
RSC2_NX_PROF_SECS seconds; --skip and --reports pick which (the race, say:
reports are numbered from 0 and printed with their sample counts by
--list). Addresses are named from rsc2/build-switch/rsc2.elf with devkitA64's
addr2line (inline frames included), run in devkitPro's container when
$DEVKITPRO is unset. "self" is time in the function itself, "total" time in
it or anything it called; both are in percent of the thread's samples.
"""
import argparse
import collections
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))


def resolve(elf, addrs):
    """address -> [function, ... outermost], file:line of the innermost."""
    addrs = sorted(addrs)
    if not addrs:
        return {}
    args = ['-a', '-f', '-i', '-C', '-e']
    if os.environ.get('DEVKITPRO'):
        cmd = [os.path.join(os.environ['DEVKITPRO'], 'devkitA64/bin/aarch64-none-elf-addr2line')] + args + [elf]
    else:
        run = shutil.which('podman') or shutil.which('docker')
        cmd = [run, 'run', '--rm', '-v', ROOT + ':/src:Z', 'docker.io/devkitpro/devkita64',
               '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line'] + args + \
              ['/src/' + os.path.relpath(elf, ROOT)]
    out = subprocess.run(cmd + ['%#x' % a for a in addrs], capture_output=True, text=True).stdout
    res, cur, lines = {}, None, out.split('\n')
    i = 0
    while i < len(lines):
        l = lines[i]
        if l.startswith('0x'):
            cur = int(l, 16)
            res[cur] = ([], '')
            i += 1
            continue
        if cur is not None and l:
            fn = l
            loc = lines[i + 1] if i + 1 < len(lines) else ''
            names, first = res[cur]
            names.append(fn)
            if not first:
                res[cur] = (names, os.path.basename(loc.split(' ')[0]))
            i += 2
            continue
        i += 1
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log', nargs='?', default=os.path.join(ROOT, 'work', 'nxlink.log'))
    ap.add_argument('--thread', default='pb')
    ap.add_argument('--skip', type=int, default=0)
    ap.add_argument('--reports', type=int, default=0, help='0: all after --skip')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--callers', type=int, default=8, help='callers of the N hottest leaves')
    ap.add_argument('--lines', action='store_true', help='leaf addresses with file:line')
    ap.add_argument('--elf', default=os.path.join(ROOT, 'rsc2', 'build-switch', 'rsc2.elf'))
    a = ap.parse_args()

    reports = []
    for l in open(a.log, errors='replace'):
        m = re.match(r'\[PROF\] (\S+) (\d+) samples \((\d+) missed\)', l)
        if m and m.group(1) == a.thread:
            reports.append({'n': int(m.group(2)), 'missed': int(m.group(3)),
                            'leaf': collections.Counter(), 'in': collections.Counter(),
                            'pair': collections.Counter()})
            continue
        m = re.match(r'\[PROF\] (\S+) pair (\d+) (0x[0-9a-f]+|0) (0x[0-9a-f]+|0)', l)
        if m and m.group(1) == a.thread and reports:
            reports[-1]['pair'][(int(m.group(3), 16), int(m.group(4), 16))] += int(m.group(2))
            continue
        m = re.match(r'\[PROF\] (\S+) (leaf|in) (\d+) (0x[0-9a-f]+|0)', l)
        if m and m.group(1) == a.thread and reports:
            reports[-1][m.group(2)][int(m.group(4), 16)] += int(m.group(3))
    if a.list:
        for i, r in enumerate(reports):
            print(i, r['n'], 'samples', r['missed'], 'missed')
        return
    pick = reports[a.skip:a.skip + a.reports] if a.reports else reports[a.skip:]
    if not pick:
        sys.exit('no reports for thread %s in %s' % (a.thread, a.log))
    n = sum(r['n'] for r in pick)
    leaf, inn, pair = collections.Counter(), collections.Counter(), collections.Counter()
    for r in pick:
        leaf.update(r['leaf'])
        inn.update(r['in'])
        pair.update(r['pair'])
    names = resolve(a.elf, set(leaf) | set(inn) | {l & ~1 for l, _ in pair} | {u for _, u in pair})
    self_fn, total_fn = collections.Counter(), collections.Counter()
    for ad, c in leaf.items():
        chain = names.get(ad, ([], ''))[0] or ['?%#x' % ad]
        self_fn[chain[0]] += c
        for f in set(chain):
            total_fn[f] += c
    for ad, c in inn.items():
        chain = names.get(ad, ([], ''))[0] or ['?%#x' % ad]
        for f in set(chain):
            total_fn[f] += c
    print('%s: %d samples over %d reports' % (a.thread, n, len(pick)))
    print('self:')
    for f, c in self_fn.most_common(a.top):
        print('  %5.1f%% %s' % (100.0 * c / n, f))
    print('total (in the function or below; approximate where a function recurses):')
    for f, c in total_fn.most_common(a.top):
        print('  %5.1f%% %s' % (100.0 * min(c, n) / n, f))
    # Who called the hottest leaves: by lr (right for a leaf without a frame
    # record, such as memcpy) and by the first frame record (right for one
    # with a record).
    def fn(ad):
        return (names.get(ad, ([], ''))[0] or ['?%#x' % ad])[0]
    def site(ad):
        nm, loc = names.get(ad, ([], ''))
        return '%s (%s)' % (nm[0] if nm else '?%#x' % ad, loc)
    for f, _ in self_fn.most_common(a.callers):
        for kind, bit in (('lr', 0), ('frame', 1)):
            c = collections.Counter()
            for (l, u), k in pair.items():
                if (l & 1) == bit and fn(l & ~1) == f:
                    c[site(u)] += k
            tot = sum(c.values())
            if not tot:
                continue
            print('%s, called from (by %s):' % (f, kind))
            for s_, k in c.most_common(6):
                print('  %5.1f%% %s' % (100.0 * k / n, s_))
    if a.lines:
        print('hottest leaf addresses:')
        for ad, c in leaf.most_common(a.top):
            nm, loc = names.get(ad, ([], ''))
            print('  %5.1f%% %#x %s %s' % (100.0 * c / n, ad, nm[0] if nm else '?', loc))


if __name__ == '__main__':
    main()
