#!/usr/bin/env python3
"""Open stand-in for NitroSDK makerom.exe, as invoked by pokediamond (SDK 3.2)
and pokeheartgold (SDK 4.2).

    makerom.py [-DNAME=VALUE ...] SPEC.rsf OUT.nds
    makerom.py --header-template OUT.sbin

Understands the subset of the ROM spec format those rom.rsf files use
(Arm9/Arm7 Static, OverlayDefs, OverlayTable; Property; RomSpec HostRoot/File)
and lays the image out the way the SDK packer does (the same strategy
lhearachel/nitrorom reproduces for Platinum): header, ARM9 static, ARM9
overlay table, ARM9 overlays, ARM7 static, FNT, FAT, banner, then the
filesystem members in spec order, every member aligned to 0x200 with 0xFF.
Compressed (compstatic `_LZ`) statics and overlays are placed as given.
The secure-area CRC and header CRC are finished by fixrom afterwards.
"""
import functools
import os
import re
import struct
import sys

HEADER_SIZE = 0x4000
ALIGN = 0x200
FILL = 0xFF

NINTENDO_LOGO = bytes.fromhex(
    "24ffae51699aa2213d84820a84e409ad11248b98c0817f21a352be199309ce20"
    "10464a4af82731ec58c7e83382e3cebf85f4df94ce4b09c194568ac01372a7fc"
    "9f844d73a3ca9a615897a327fc039876231dc7610304ae56bf38840040a70efd"
    "ff52fe036f9530f197fbc08560d68025a963be03014e38e2f9a234ffbb3e0344"
    "780090cb88113a9465c07c6387f03cafd625e48b380aac7221d4f807")

# Card bus timings (0x60, 0x64) and secure-area delay (0x6E) per RomSpeedType.
SPEED = {
    "MROM": (0x00586000, 0x001808F8, 0x051E),
    "1TROM": (0x00416657, 0x081808F8, 0x0D7E),
}


def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def header_template():
    h = bytearray(HEADER_SIZE)
    h[0x0C:0x10] = b"NTRJ"
    h[0x10:0x12] = b"01"
    mrom = SPEED["MROM"]
    struct.pack_into("<IIHH", h, 0x60, mrom[0], mrom[1], 0, mrom[2])
    h[0xC0:0x15C] = NINTENDO_LOGO
    struct.pack_into("<H", h, 0x15C, crc16(NINTENDO_LOGO))
    return h


def parse_spec(text, defs):
    def subst(s):
        def var(m):
            if m.group(1) not in defs:
                sys.exit(f"makerom: undefined variable $({m.group(1)})")
            return defs[m.group(1)]
        return re.sub(r"\$\((\w+)\)", var, s)

    spec, section = {}, None
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line == "{":
            continue
        if line == "}":
            section = None
            continue
        if section is None:
            section = line
            spec.setdefault(section, [])
            continue
        key, _, value = line.partition(" ")
        value = subst(value.strip())
        if len(value) >= 2 and value[0] == value[-1] == '"':
            value = value[1:-1]
        spec[section].append((key, value))
    return spec


def get(spec, section, key, default=None):
    for k, v in spec.get(section, []):
        if k == key:
            return v
    if default is None:
        sys.exit(f"makerom: missing {section}.{key}")
    return default


def read(path):
    with open(path, "rb") as f:
        return f.read()


def parse_defs(data):
    load, entry, size, autoload_cb = struct.unpack_from("<IIII", data)
    names = [n.decode() for n in data[0x10:].split(b"\0") if n]
    return load, entry, size, autoload_cb, names


def capacity_shift(size):
    shift = 0
    while (0x20000 << shift) <= size:
        shift += 1
    return shift


def mbit(value):
    m = re.fullmatch(r"(\d+)([MG])", value)
    if not m:
        sys.exit(f"makerom: bad RomSize {value}")
    return int(m.group(1)) * (1 << 20 if m.group(2) == "M" else 1 << 30) // 8


def fnt_key(a, b):
    """Directory-entry order: files before subdirectories at each depth,
    then case-insensitive byte order (end of name sorts first)."""
    pa, pb = a.split("/"), b.split("/")
    for i in range(min(len(pa), len(pb))):
        da, db = i < len(pa) - 1, i < len(pb) - 1
        if da != db:
            return 1 if da else -1
        term = "/" if da else "\0"
        ka, kb = pa[i].lower() + term, pb[i].lower() + term
        if ka != kb:
            return -1 if ka < kb else 1
    return 0


def build_fnt(targets, first_id):
    ordered = sorted(targets, key=functools.cmp_to_key(fnt_key))
    dirs = [{"path": (), "file0": first_id, "id": 0xF000, "parent": 0, "children": []}]
    by_path = {(): 0}
    ids = {}
    fid = first_id
    for t in ordered:
        parts = tuple(t.split("/"))
        parent = 0
        for depth in range(1, len(parts)):
            p = parts[:depth]
            if p not in by_path:
                d = {"path": p, "file0": fid, "id": 0xF000 | len(dirs),
                     "parent": dirs[parent]["id"], "children": []}
                dirs[parent]["children"].append((p[-1], d["id"]))
                by_path[p] = len(dirs)
                dirs.append(d)
            parent = by_path[p]
        dirs[parent]["children"].append((parts[-1], 0))
        ids[t] = fid
        fid += 1
    dirs[0]["parent"] = len(dirs)

    table = bytearray()
    body = bytearray()
    for d in dirs:
        table += struct.pack("<IHH", 8 * len(dirs) + len(body), d["file0"], d["parent"])
        for name, dirid in d["children"]:
            nb = name.encode("ascii")
            body.append(len(nb) | (0x80 if dirid else 0))
            body += nb
            if dirid:
                body += struct.pack("<H", dirid)
        body.append(0)
    return bytes(table + body), ids


def makerom(spec_path, out_path, defs):
    spec = parse_spec(open(spec_path, encoding="utf-8").read(), defs)

    header = bytearray(read(get(spec, "Property", "RomHeaderTemplate")).ljust(HEADER_SIZE, b"\0"))
    title = get(spec, "Property", "TitleName").encode("ascii")
    header[0x00:0x0C] = title[:12].ljust(12, b"\0")
    header[0x10:0x12] = get(spec, "Property", "MakerCode").encode("ascii")[:2]
    header[0x1E] = int(get(spec, "Property", "RemasterVersion", "0"), 0)
    speed = get(spec, "Property", "RomSpeedType", "MROM")
    if speed not in SPEED:
        sys.exit(f"makerom: unsupported RomSpeedType {speed}")
    ctrl_dec, ctrl_enc, delay = SPEED[speed]
    struct.pack_into("<II", header, 0x60, ctrl_dec, ctrl_enc)
    struct.pack_into("<H", header, 0x6E, delay)

    arm9 = read(get(spec, "Arm9", "Static"))
    defs9 = get(spec, "Arm9", "OverlayDefs")
    a9load, a9entry, a9size, a9cb, ov9names = parse_defs(read(defs9))
    ovt9 = read(get(spec, "Arm9", "OverlayTable", "")) if ov9names else b""
    arm7 = read(get(spec, "Arm7", "Static"))
    a7load, a7entry, a7size, a7cb, ov7names = parse_defs(read(get(spec, "Arm7", "OverlayDefs")))
    if ov7names:
        sys.exit("makerom: ARM7 overlays are not supported")
    banner = read(get(spec, "Property", "BannerFile"))

    root = get(spec, "RomSpec", "HostRoot").rstrip("/")
    files = get(spec, "RomSpec", "File").split()
    if len(set(files)) != len(files):
        sys.exit("makerom: duplicate filesystem members")

    # Overlay file names in the defs file are relative to the defs file.
    ov9 = [read(os.path.join(os.path.dirname(defs9), n)) for n in ov9names]
    numovys = len(ov9)
    fnt, ids = build_fnt(files, numovys)
    fat = bytearray(8 * (numovys + len(files)))

    out = bytearray(header)

    def place(data):
        off = len(out)
        out.extend(data)
        out.extend(bytes([FILL]) * (-len(data) & (ALIGN - 1)))
        return off

    struct.pack_into("<IIII", header, 0x20, place(arm9), a9entry, a9load, a9size)
    if ovt9:
        struct.pack_into("<II", header, 0x50, place(ovt9), len(ovt9))
    for i, data in enumerate(ov9):
        off = place(data)
        struct.pack_into("<II", fat, 8 * i, off, off + len(data))
    struct.pack_into("<IIII", header, 0x30, place(arm7), a7entry, a7load, a7size)
    fnt_off = len(out)
    place(fnt)
    fat_off = len(out)
    place(bytes(len(fat)))  # placeholder; filled once file offsets are known
    struct.pack_into("<IIII", header, 0x40, fnt_off, len(fnt), fat_off, len(fat))
    struct.pack_into("<I", header, 0x68, place(banner))
    struct.pack_into("<II", header, 0x70, a9cb, a7cb)

    romsize = len(out)
    for name in files:
        data = read(f"{root}/{name}")
        off = place(data)
        struct.pack_into("<II", fat, 8 * ids[name], off, off + len(data))
        romsize = off + len(data)
    out[fat_off:fat_off + len(fat)] = fat

    del out[romsize:]
    cap = get(spec, "Property", "RomSize", "")
    capacity = mbit(cap) if cap else 0x20000 << capacity_shift(romsize)
    if romsize > capacity:
        sys.exit(f"makerom: ROM is 0x{romsize:X} bytes, exceeds RomSize {cap}")
    header[0x14] = capacity_shift(capacity - 1)
    struct.pack_into("<II", header, 0x80, romsize, HEADER_SIZE)
    footer = arm9[a9size:a9size + 12] if len(arm9) >= a9size + 12 else b""
    if len(footer) == 12 and struct.unpack_from("<I", footer)[0] == 0xDEC00621:
        struct.pack_into("<I", header, 0x88, 0x4000 + struct.unpack_from("<I", footer, 4)[0])
    struct.pack_into("<H", header, 0x6C, crc16(out[0x4000:0x8000]))
    struct.pack_into("<H", header, 0x15E, crc16(header[:0x15E]))
    out[:HEADER_SIZE] = header

    if get(spec, "Property", "RomFootPadding", "FALSE").upper() == "TRUE":
        out.extend(bytes([FILL]) * (capacity - len(out)))

    with open(out_path, "wb") as f:
        f.write(out)


def main(argv):
    if len(argv) == 3 and argv[1] == "--header-template":
        with open(argv[2], "wb") as f:
            f.write(header_template())
        return
    defs, pos = {}, []
    for a in argv[1:]:
        if a.startswith("-D"):
            k, _, v = a[2:].partition("=")
            defs[k] = v
        else:
            pos.append(a)
    if len(pos) != 2:
        sys.exit(__doc__)
    makerom(pos[0], pos[1], defs)


if __name__ == "__main__":
    main(sys.argv)
