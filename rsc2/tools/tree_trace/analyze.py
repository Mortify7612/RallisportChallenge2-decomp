import sys, collections
sys.path.insert(0, '.')
import tt
recs = tt.read(sys.argv[1])
marks = [f for f, k, x, e in recs if k == 9]
k1 = {}; k2 = {}
for f, k, x, e in recs:
    if k == 1: k1[f] = {i[0]: i for i in e}
    if k == 2: k2[f] = {i[0]: i for i in e}
print('marks at frames', marks)
def drops(series, f0, f1, hold=10):
    fs = [f for f in sorted(series) if f0 <= f <= f1]
    res = []
    for a, b in zip(fs, fs[1:]):
        for inst in series[a].keys() - series[b].keys():
            later = [f for f in fs if f > b][:hold]
            if all(inst not in series[f] for f in later):
                res.append((b, inst, series[a][inst]))
    return res
for m in marks:
    print('== mark at', m)
    d1 = drops(k1, m - 240, m)
    d2 = drops(k2, m - 240, m)
    in1 = {(f, i) for f, i, _ in d1}
    print('  left cull output:', len(d1), ' left buckets:', len(d2))
    for f, inst, e in d2[-12:]:
        st = 'also left cull' if any(i == inst and abs(ff - f) <= 2 for ff, i, _ in d1) else 'STILL IN CULL OUTPUT' if inst in k1.get(f, {}) else '?'
        print('   bucket drop F%d inst %08X dist %.1f w %.3f lod %d type %d -> %s' % (f, inst, e[1], e[2], e[3], e[4], st))
