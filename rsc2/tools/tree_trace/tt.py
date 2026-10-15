import struct, sys, collections
def read(path):
    d = open(path, 'rb').read(); i = 0; out = []
    while i + 20 <= len(d):
        magic, frame, kind, n, extra = struct.unpack_from('<5I', d, i); i += 20
        assert magic == 0x30465454, hex(magic)
        ents = []
        if i + 16 * n > len(d): break
        for k in range(n):
            inst, dist, w, lod, typ, pad = struct.unpack_from('<IffBBH', d, i); i += 16
            ents.append((inst, dist, w, lod, typ, pad))
        out.append((frame, kind, extra, ents))
    return out
if __name__ == '__main__':
    recs = read(sys.argv[1])
    c1 = [(f, len(e), x) for f, k, x, e in recs if k == 1]
    c2 = [(f, len(e)) for f, k, x, e in recs if k == 2]
    hist = collections.Counter(n for f, n, x in c1)
    print('kind1 frames', len(c1), 'count range', min(n for f,n,x in c1), max(n for f,n,x in c1))
    print('top counts', hist.most_common(8))
    print('args (a3:a4)', collections.Counter(hex(x) for f,n,x in c1).most_common(4))
    print('kind2 frames', len(c2), 'range', min(n for f,n in c2), max(n for f,n in c2))
