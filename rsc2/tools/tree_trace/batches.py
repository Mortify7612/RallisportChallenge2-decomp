import sys, collections
import tt
# The main pass's sprite list and batches around a frame: does a batch merge,
# split or lose items when trees pop?
#   batches.py tree.bin FRAME
BUCKETS = 0x00273872        # sub_00114C80 called from sub_00273710 (buckets)
trace, pf = sys.argv[1], int(sys.argv[2])
lst = {}; bats = collections.defaultdict(list); ctx = None
for f, k, x, ents in tt.read(trace):
    if not (pf - 1 <= f <= pf):
        continue
    if k == 13:
        ctx = x
        if x == BUCKETS:
            lst[f] = [(e[0], e[2]) for e in ents]
    elif k == 11 and ctx == BUCKETS:
        bats[f].append((x >> 24, [e[0] for e in ents]))
for f in (pf - 1, pf):
    out, run = [], 0
    for inst, w in lst.get(f, []):
        if w == 1:
            run += 1
            continue
        if run:
            out.append('B%d' % run)
            run = 0
        out.append('m%.2f' % w if w > 0 else 'M')
    if run:
        out.append('B%d' % run)
    print('F%d list: %s' % (f, ' '.join(out)))
    print('F%d batches (draws, items): %s' % (f, [(d, len(it)) for d, it in bats[f]]))
a = collections.Counter(i for d, it in bats[pf - 1] for i in it)
b = collections.Counter(i for d, it in bats[pf] for i in it)
print('items lost', ['%08X' % i for i in a - b], 'gained', ['%08X' % i for i in b - a])
