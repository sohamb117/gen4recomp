"""bdhc.py MATRIX -> per-tile height map (tile units = 16 world units), from D/P land data BDHC."""
import struct, sys
F = "/Users/soham/Documents/code/nativeplat-e2e-dp/games/diamond/files/fielddata"

def matrix(n):
    d = open("%s/mapmatrix/map_matrix/narc_%04d.bin" % (F, n), "rb").read()
    w, h, hh, hhe, nl = d[0], d[1], d[2], d[3], d[4]
    p = 5 + nl
    if hh: p += 2 * w * h
    if hhe: p += w * h
    return w, h, [struct.unpack_from("<H", d, p + 2 * i)[0] for i in range(w * h)]

def land(i):
    d = open("%s/land_data/land_data_release/narc_%04d.bin" % (F, i), "rb").read()
    perm, bld, mdl, bdhc = struct.unpack_from("<4I", d, 0)
    off = 16 + perm + bld + mdl
    perms = [struct.unpack_from("<H", d, 16 + 2 * k)[0] for k in range(1024)]
    return perms, d[off:off + bdhc]

def heights(b):
    assert b[:4] == b"BDHC", b[:4]
    npt, nsl, nh, npl, nst, nacc = struct.unpack_from("<6H", b, 4)
    p = 16
    pts = [struct.unpack_from("<2i", b, p + 8 * k) for k in range(npt)]; p += 8 * npt
    sl = [struct.unpack_from("<3i", b, p + 12 * k) for k in range(nsl)]; p += 12 * nsl
    hs = [struct.unpack_from("<i", b, p + 4 * k)[0] for k in range(nh)]; p += 4 * nh
    pl = [struct.unpack_from("<4H", b, p + 8 * k) for k in range(npl)]
    out = {}
    for tz in range(32):
        for tx in range(32):
            x = (tx * 16 + 8 - 256) * 4096; z = (tz * 16 + 8 - 256) * 4096
            best = []
            for a, c, si, hi in pl:
                x0, z0 = pts[a]; x1, z1 = pts[c]
                if min(x0, x1) <= x <= max(x0, x1) and min(z0, z1) <= z <= max(z0, z1):
                    nx, ny, nz = sl[si]
                    if ny == 0: continue
                    y = -(nx * x / 4096 + nz * z / 4096 + hs[hi]) / ny * 4096
                    best.append(y / 4096)
            out[(tx, tz)] = sorted(set(round(v, 1) for v in best))
    return out

if __name__ == "__main__":
    w, h, ids = matrix(int(sys.argv[1]))
    for cz in range(h):
        for cx in range(w):
            if ids[cz * w + cx] == 0xFFFF: continue
            perms, b = land(ids[cz * w + cx])
            H = heights(b)
            for tz in range(32):
                row = []
                for tx in range(32):
                    v = H[(tx, tz)]
                    row.append(("%4d" % (v[-1] / 16 * 2) if v else "   .") + ("#" if perms[tz * 32 + tx] & 0x8000 else " "))
                print("%3d %s" % (cz * 32 + tz, "".join(row)))
