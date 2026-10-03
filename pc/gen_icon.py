#!/usr/bin/env python3
"""Draw the port's icon and emit it in the three shapes the build needs.

The artwork is procedural on purpose. An icon cut from the game's own art
would be the game's art, and this repository ships source and tools rather
than Nintendo's pixels, so what is drawn here is a generic two-screen
handheld silhouette in platinum grey, owned by this repository and safe to
ship.

Outputs, all under the build directory:

  pokeplatinum.ico   16/32/48 as 32bpp DIBs, 256 as an embedded PNG. This is
                     what Explorer, the taskbar and Alt-Tab read.
  pokeplatinum.rc    a resource script naming that .ico twice: numeric id 1,
                     which is the one Explorer picks (lowest id wins), and
                     the name SDL2 looks up on the window class (`SDL_icon`),
                     so an SDL window on Windows gets it without any code.
                     It also carries the VERSIONINFO block that decides what
                     Task Manager and the file's Properties page call this
                     program, without it they fall back to the filename.
                     Written UTF-8; windres is given -c 65001 to match.
  pc_icon.h          48x48 RGBA8888 as a C array, for SDL_SetWindowIcon on
                     platforms with no resource section; which is how the
                     Linux viewer and launcher get the same picture.

  icon48.png         under --png, and only there: the 48x48 smdhtool wants
  banner.png         and the 256x128 the 3DS Home Menu shows above it.

Everything is drawn at 4x and box-filtered down, which is the whole of the
antialiasing story here.
"""

import argparse
import os
import struct
import zlib

SS = 4  # supersampling factor

# Platinum grey over slate, with one lit screen. Chosen for 16px legibility:
# A dark body, a bright top screen, a dimmer bottom one, and nothing else.
BODY_TOP = (0x3A, 0x41, 0x52)
BODY_BOT = (0x17, 0x1A, 0x22)
RIM = (0xC9, 0xCE, 0xD8)
SCREEN_TOP_A = (0x8C, 0xC0, 0xEE)
SCREEN_TOP_B = (0x2A, 0x5B, 0x8F)
SCREEN_BOT = (0x46, 0x4E, 0x60)


def _lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def _rounded(x, y, x0, y0, x1, y1, r):
    """Is (x, y) inside the rounded rect? Sample points are pixel centres."""
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    dx, dy = x - cx, y - cy
    return dx * dx + dy * dy <= r * r


def draw(n):
    """Return n*n RGBA rows (top-down), each a list of (r, g, b, a)."""
    m = n * SS
    body_r = m * 0.20
    rim_w = max(1.0, m * 0.030)
    scr_r = m * 0.045
    margin = m * 0.055

    bx0, by0, bx1, by1 = margin, margin, m - 1 - margin, m - 1 - margin
    sx0, sx1 = m * 0.20, m * 0.80
    top = (m * 0.155, m * 0.455)
    bot = (m * 0.545, m * 0.845)

    big = []
    for py in range(m):
        y = py + 0.5
        row = []
        for px in range(m):
            x = px + 0.5
            if not _rounded(x, y, bx0, by0, bx1, by1, body_r):
                row.append((0, 0, 0, 0))
                continue
            inner = _rounded(x, y, bx0 + rim_w, by0 + rim_w,
                             bx1 - rim_w, by1 - rim_w, max(0.0, body_r - rim_w))
            if not inner:
                row.append(RIM + (255,))
                continue
            if _rounded(x, y, sx0, top[0], sx1, top[1], scr_r):
                t = (y - top[0]) / (top[1] - top[0])
                row.append(_lerp(SCREEN_TOP_A, SCREEN_TOP_B, t) + (255,))
            elif _rounded(x, y, sx0, bot[0], sx1, bot[1], scr_r):
                row.append(SCREEN_BOT + (255,))
            else:
                t = (y - by0) / (by1 - by0)
                row.append(_lerp(BODY_TOP, BODY_BOT, t) + (255,))
        big.append(row)

    out = []
    area = SS * SS
    for y in range(n):
        row = []
        for x in range(n):
            r = g = b = a = 0
            for dy in range(SS):
                src = big[y * SS + dy]
                for dx in range(SS):
                    p = src[x * SS + dx]
                    # Premultiply, or the transparent black outside the body
                    # drags the rim toward black as it is averaged in.
                    r += p[0] * p[3]
                    g += p[1] * p[3]
                    b += p[2] * p[3]
                    a += p[3]
            if a == 0:
                row.append((0, 0, 0, 0))
            else:
                row.append((r // a, g // a, b // a, a // area))
        out.append(row)
    return out


BANNER_BG_TOP = (0x2A, 0x2F, 0x3C)
BANNER_BG_BOT = (0x10, 0x12, 0x18)


def draw_banner(w=256, h=128):
    """The 3DS Home Menu banner: the same handheld, centred on darker slate.

    The device is drawn by draw() and composited, so the banner and the icon
    can never drift apart; there is one piece of artwork here, at two sizes.
    The background is darker than the body's own gradient at every row, which
    with the bright rim is what keeps the silhouette off it at Home Menu size.
    """
    n = int(h * 0.875)
    dev = draw(n)
    x0, y0 = (w - n) // 2, (h - n) // 2

    out = []
    for y in range(h):
        bg = _lerp(BANNER_BG_TOP, BANNER_BG_BOT, y / (h - 1))
        row = []
        for x in range(w):
            p = (0, 0, 0, 0)
            if x0 <= x < x0 + n and y0 <= y < y0 + n:
                p = dev[y - y0][x - x0]
            a = p[3]
            if a == 0:
                row.append(bg + (255,))
            elif a == 255:
                row.append(p[:3] + (255,))
            else:
                row.append(tuple((p[i] * a + bg[i] * (255 - a)) // 255
                                 for i in range(3)) + (255,))
        out.append(row)
    return out


def png_bytes(img):
    h, w = len(img), len(img[0])
    raw = bytearray()
    for row in img:
        raw.append(0)
        for p in row:
            raw += bytes(p)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def dib_bytes(img):
    """32bpp BITMAPINFOHEADER icon image: XOR rows bottom-up, then an AND mask.

    The mask is all zeros because the alpha channel already carries the
    shape, but it has to be there: the height field in the header is twice
    the image height and readers size the buffer from it.
    """
    n = len(img)
    hdr = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0, n * n * 4,
                      0, 0, 0, 0)
    xor = bytearray()
    for row in reversed(img):
        for p in row:
            xor += bytes((p[2], p[1], p[0], p[3]))
    stride = ((n + 31) // 32) * 4
    return hdr + bytes(xor) + bytes(stride * n)


def ico_bytes(entries):
    """entries: [(size, payload)] where payload is a DIB or a whole PNG."""
    out = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    dirs, blobs = b"", b""
    for size, payload in entries:
        dim = 0 if size >= 256 else size
        dirs += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32,
                            len(payload), offset)
        blobs += payload
        offset += len(payload)
    return out + dirs + blobs


def rc_text(ico, title, exe):
    """Explorer takes the lowest-numbered icon; SDL2 asks its window class for
    a resource literally named SDL_icon. Both name the same file.

    No CompanyName or LegalCopyright: this program is a compile of a
    decompilation and neither field would be honest.
    """
    return (
        '1 ICON "%s"\n'
        'SDL_icon ICON "%s"\n'
        '\n'
        '1 VERSIONINFO\n'
        'FILEVERSION 1,0,0,0\n'
        'PRODUCTVERSION 1,0,0,0\n'
        'FILEOS 0x4\n'
        'FILETYPE 0x1\n'
        '{\n'
        '  BLOCK "StringFileInfo"\n'
        '  {\n'
        '    BLOCK "040904B0"\n'
        '    {\n'
        '      VALUE "FileDescription", "%s"\n'
        '      VALUE "ProductName", "%s"\n'
        '      VALUE "FileVersion", "1.0.0.0"\n'
        '      VALUE "ProductVersion", "1.0.0.0"\n'
        '      VALUE "OriginalFilename", "%s"\n'
        '    }\n'
        '  }\n'
        '  BLOCK "VarFileInfo"\n'
        '  {\n'
        '    VALUE "Translation", 0x409, 1200\n'
        '  }\n'
        '}\n'
    ) % (ico, ico, title, title, exe)


def c_header(img, guard="PC_ICON_H"):
    n = len(img)
    flat = [c for row in img for p in row for c in p]
    lines = [
        "/* Generated by pc/gen_icon.py, do not edit. */",
        "#ifndef %s" % guard,
        "#define %s" % guard,
        "",
        "#define PC_ICON_W %d" % n,
        "#define PC_ICON_H_PX %d" % n,
        "",
        "static const unsigned char pc_icon_rgba[%d] = {" % len(flat),
    ]
    for i in range(0, len(flat), 16):
        lines.append("    " + ",".join("%d" % v for v in flat[i:i + 16]) + ",")
    lines += ["};", "", "#endif", ""]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", help="output directory for the Windows shapes")
    ap.add_argument("--png", metavar="DIR",
                    help="write the two PNGs 3DS packaging reads: icon48.png "
                         "for the SMDH and banner.png for the CIA")
    ap.add_argument("--name", default="pokeplatinum")
    ap.add_argument("--title", default="Pokémon Platinum",
                    help="what Windows should call the program")
    ap.add_argument("--exes", default="",
                    help="comma-separated exe names to emit a .rc for; the "
                         "default is <name>.exe alone")
    args = ap.parse_args()
    if not args.out and not args.png:
        ap.error("nothing to write: pass --out, --png, or both")

    if args.png:
        os.makedirs(args.png, exist_ok=True)
        for name, img in (("icon48.png", draw(48)),
                          ("banner.png", draw_banner())):
            with open(os.path.join(args.png, name), "wb") as f:
                f.write(png_bytes(img))
            print("  ICON    %s (%d bytes)"
                  % (name, os.path.getsize(os.path.join(args.png, name))))
        if not args.out:
            return

    os.makedirs(args.out, exist_ok=True)

    entries = [(s, dib_bytes(draw(s))) for s in (16, 32, 48)]
    entries.append((256, png_bytes(draw(256))))
    ico = os.path.join(args.out, args.name + ".ico")
    with open(ico, "wb") as f:
        f.write(ico_bytes(entries))

    exes = [e for e in args.exes.split(",") if e] or [args.name + ".exe"]
    rcs = []
    for exe in exes:
        rc = os.path.join(args.out, os.path.splitext(exe)[0] + ".rc")
        with open(rc, "w", encoding="utf-8") as f:
            f.write(rc_text(args.name + ".ico", args.title, exe))
        rcs.append(os.path.basename(rc))

    hdr = os.path.join(args.out, "pc_icon.h")
    with open(hdr, "w") as f:
        f.write(c_header(draw(48)))

    print("  ICON    %s (%d bytes), %s, %s"
          % (os.path.basename(ico), os.path.getsize(ico),
             " ".join(rcs), os.path.basename(hdr)))


if __name__ == "__main__":
    main()
