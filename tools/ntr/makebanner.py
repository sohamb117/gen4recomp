#!/usr/bin/env python3
"""Open stand-in for NitroSDK makebanner.exe (version 1 banners).

    makebanner.py SPEC.bsf OUT.bnr

The spec is UTF-16 with a BOM, one `Command: argument` per line:
ImageFile (512-byte 4bpp icon), PlttFile (32-byte palette) and the six
title/developer entries JP EN FR GE IT SP, whose continuation lines start
with a space and are joined with '\\n'. Paths are relative to the cwd.
Output: version 1 header, CRC-16 of 0x20..0x840, icon, palette, titles of
0x100 bytes each (UTF-16LE, zero-padded).
"""
import struct
import sys

LANGS = ("JP", "EN", "FR", "GE", "IT", "SP")
TITLE_SIZE = 0x100


def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def parse(path):
    raw = open(path, "rb").read()
    if not raw.startswith(b"\xff\xfe"):
        sys.exit("makebanner: spec must be UTF-16LE with a BOM")
    lines = raw[2:].decode("utf-16-le").replace("\r", "").split("\n")
    spec, last = {}, None
    for line in lines:
        if line.startswith(" ") and last in LANGS:
            spec[last] += "\n" + line.lstrip(" ")
            continue
        last = None
        if ":" not in line:
            continue
        cmd, _, arg = line.partition(":")
        cmd, arg = cmd.strip(), arg[1:] if arg.startswith(" ") else arg
        if cmd in LANGS or cmd in ("ImageFile", "PlttFile"):
            spec[cmd] = arg
            last = cmd
        elif cmd != "Version":
            sys.exit(f"makebanner: unsupported command {cmd!r}")
    for k in ("ImageFile", "PlttFile") + LANGS:
        if k not in spec:
            sys.exit(f"makebanner: missing {k}")
    return spec


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)
    spec = parse(argv[1])
    icon = open(spec["ImageFile"], "rb").read()
    pltt = open(spec["PlttFile"], "rb").read()
    if len(icon) != 0x200 or len(pltt) != 0x20:
        sys.exit("makebanner: icon must be 512 bytes and palette 32 bytes")
    body = bytearray(icon + pltt)
    for lang in LANGS:
        title = spec[lang].encode("utf-16-le")
        if len(title) > TITLE_SIZE:
            sys.exit(f"makebanner: {lang} title longer than 128 characters")
        body += title.ljust(TITLE_SIZE, b"\0")
    out = struct.pack("<HH", 1, crc16(body)) + bytes(0x1C) + body
    with open(argv[2], "wb") as f:
        f.write(out)


if __name__ == "__main__":
    main(sys.argv)
