#!/usr/bin/env python3
"""region_same.py A.png B.png X0 Y0 X1 Y1: exit 0 when the two frames'
pixels agree in the box [X0, X1) x [Y0, Y1), 1 when they differ.

For parity.sh's instant-text check: a message box's text area, leaving out
the blinking "more" arrow at its right end. Reads the 8-bit RGB / RGBA,
non-interlaced PNGs sips writes.
"""
import struct
import sys
import zlib


def rows(path):
    data = open(path, 'rb').read()
    pos, idat = 8, b''
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            width, height, depth, colour = struct.unpack('>IIBB', chunk[:10])
            assert depth == 8 and colour in (2, 6), (path, depth, colour)
        elif kind == b'IDAT':
            idat += chunk
        pos += 12 + n
    bpp = 3 if colour == 2 else 4
    stride = width * bpp
    raw = zlib.decompress(idat)
    out, prev = [], bytearray(stride)
    for y in range(height):
        kind, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out.append(bytes(line))
        prev = line
    return bpp, out


def main():
    a, b = sys.argv[1], sys.argv[2]
    x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7])
    bpp_a, ra = rows(a)
    bpp_b, rb = rows(b)
    for y in range(y0, y1):
        for x in range(x0, x1):
            if ra[y][x * bpp_a:x * bpp_a + 3] != rb[y][x * bpp_b:x * bpp_b + 3]:
                return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
