import sys, collections
# Per frame: pixels written by each program; report the biggest frame-to-frame steps.
def load(path):
    q = {}; dr = []
    for l in open(path):
        if l.startswith('Q '):
            _, f, d, n = l.split(); q[(f, d)] = int(n)
        elif l.startswith('F') and ' prog ' in l and ' clip 0,0,640,480 ' in l:
            p = l.split(); dr.append((p[0], p[1], p[3]))
    per = collections.defaultdict(lambda: collections.Counter())
    cnt = collections.defaultdict(lambda: collections.Counter())
    for f, d, prog in dr:
        per[int(f[1:])][prog] += q.get((f, d), 0)
        cnt[int(f[1:])][prog] += 1
    return per, cnt
progs = sys.argv[2].split(',')
per, cnt = load(sys.argv[1])
fs = sorted(per)
for prog in progs:
    steps = []
    for a, b in zip(fs, fs[1:]):
        x, y = per[a][prog], per[b][prog]
        if x - y > 300 and y < x * 0.97:
            steps.append((x - y, b, x, y, cnt[a][prog], cnt[b][prog]))
    steps.sort(reverse=True)
    print(prog, 'frames', fs[0], fs[-1], 'top drops:', ['F%d %d->%d draws %d->%d' % (b, x, y, ca, cb) for d, b, x, y, ca, cb in steps[:4]])
