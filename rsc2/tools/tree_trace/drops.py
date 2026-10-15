import sys, re, collections
# Frames where E92096D6 tree-mesh draws (by vertex address) vanish and stay gone.
def drops(path, prog='E92096D6'):
    fr = collections.defaultdict(set)
    for l in open(path):
        if not l.startswith('F') or ' prog ' + prog not in l: continue
        p = l.split(); f = int(p[0][1:])
        va = l.split(' va ')[1].split()[0]
        fr[f].add(va)
    fs = sorted(fr)
    out = []
    for i in range(1, len(fs) - 3):
        gone = fr[fs[i-1]] - fr[fs[i]]
        stay = {v for v in gone if all(v not in fr[fs[j]] for j in range(i, min(i+4, len(fs))))}
        came = fr[fs[i]] - fr[fs[i-1]]
        if stay or came:
            out.append((fs[i], len(fr[fs[i]]), sorted(stay), sorted(came)))
    return fs, out
for p in sys.argv[1:]:
    fs, out = drops(p)
    print(p, 'frames', fs[0], '-', fs[-1])
    for f, n, g, c in out:
        if g or c: print('  F%d n=%d gone %s came %s' % (f, n, g[:6], c[:6]))
