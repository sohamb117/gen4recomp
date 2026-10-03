#!/usr/bin/env python3
"""pc/wasm/patch_ov_addrs.py: write the overlay statics' addresses into the module.

pc_fs_overlay.c resets each overlay's writable statics on every load after the
first (on hardware the ROM copy is that reset). The desktop finds them in the
running ELF's .symtab (pc_sym.c); a wasm module has no symbol table for data at
all, so this is the 3DS answer (3ds/gen_ov_addrs.py) for wasm:
pc/gen_overlay_statics.py --addr-table emits pc_ov_static_addr[] at its final
size, filled with a sentinel, and this writes the real values into the linked
module's data segment afterwards. Only the table's bytes change; the file's
size and every other byte are checked to be identical afterwards, so nothing
moved and no address the table names can have gone stale.

Where the addresses come from: the wasm-ld link map (-Map). It lists every
live data symbol, file-local `static`s included, under the input segment
(`obj.o:(.data.<sym>)`) and output segment (.rodata/.data/.bss) it was placed
in, with its address, file offset and size:

         Addr      Off     Size Out     In      Symbol
      b000990     4666      13c .data
      b000990     4670       10         a.o:(.data.foo)
      b000990     4670       10                 foo
      b0009a0     4680       10         b.o:(.data.foo)
      b0009a0     4680       10                 foo

Two file-local statics that share a name appear twice with different
addresses, exactly as two STT_LOCAL entries do in an ELF .symtab, so the
semantics are the ELF reader's and 3ds/gen_ov_addrs.py's: a name with one
address is placed, a name with several is ambiguous (the rows name an overlay,
not a file, and picking one could reset a main-binary static), a name with
none was discarded by --gc-sections (wasm-ld's default) and is dropped. Only
data symbols are counted: functions live in a separate index space and have no
address that could be confused with one.

Where the table's bytes are: the module itself is parsed (data section, id
11). The table must lie inside an ACTIVE segment whose offset is an i32.const;
wasm-ld writes no bytes for .bss (memory starts zeroed), which is why the
generator's sentinel is -1 and never zero. A passive segment, or one placed by
any other expression, is refused. Every active segment the module has must be
an output segment in the map at the same address and size, and the table's
bytes must still hold the sentinel, so a map from another link is refused.
(The map's "Off" column is NOT used: for wasm it is computed before the
segment headers' LEB widths are final and is a few bytes short of the real
file offset.)

Entry format, the generator's and the 3DS one unchanged: { int32 off, uint32
size }, `off` the signed distance from &pc_ov_static_addr[0] to the static,
`size` the linker's. Guest addresses on wasm are absolute and fixed (no loader
relocates a module), so an absolute address would also be correct here; the
self-relative form is kept so the generator's struct, its comment and the C
reader are the same on both hosts that need the table. pc/src/pc_sym.c (wasm
branch) reads it back. An entry whose off is -1 is not an offset; its size
says why:
  0    unpatched (the generator's sentinel; the runtime refuses the table)
  1    dropped by the link
  >=2  that many addresses: ambiguous, skipped

    patch_ov_addrs.py <module.wasm> <link.map> <overlay_statics.c>
"""

import os
import re
import struct
import sys

ROW = re.compile(r'\{\s*(\d+),\s*"([^"]+)",\s*(\d+)\s*\}')
MAP_LINE = re.compile(r'^\s*([0-9a-f]+|-)\s+([0-9a-f]+)\s+([0-9a-f]+) (.*)$')

TABLE = "pc_ov_static_addr"
ENTRY = 8                       # int32 offset from the table, uint32 size
NOT_AN_OFFSET = -1
UNPATCHED, DROPPED = 0, 1
SENTINEL = struct.pack("<iI", NOT_AN_OFFSET, UNPATCHED)

WASM_DATA_SECTION = 11
I32_CONST, END = 0x41, 0x0B


def die(msg):
    sys.exit("patch_ov_addrs: " + msg)


def uleb(buf, pos):
    value = shift = 0
    while True:
        byte = buf[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return value, pos


def sleb(buf, pos):
    value = shift = 0
    while True:
        byte = buf[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            if byte & 0x40:
                value -= 1 << shift
            return value, pos


def map_data(path):
    """(symbols, segments) from a wasm-ld map: name -> [(addr, size, input)]
    for every data symbol, and [(name, addr, size)] for every output data
    segment. Output segments are at indent 0, input segments at 8, symbols
    at 16 (lld/wasm/MapFile.cpp); code has address '-'."""
    syms = {}
    segs = []
    out_seg = None
    in_seg = None
    with open(path, errors="replace") as fh:
        for line in fh:
            m = MAP_LINE.match(line.rstrip("\n"))
            if not m:
                continue
            addr, _off, size, rest = m.groups()
            indent = len(rest) - len(rest.lstrip(" "))
            text = rest.strip()
            if indent == 0:
                out_seg = text if addr != "-" else None
                in_seg = None
                if out_seg is not None:
                    segs.append((text, int(addr, 16), int(size, 16)))
            elif indent == 8:
                in_seg = text
            elif out_seg is not None and addr != "-":
                syms.setdefault(text, []).append(
                    (int(addr, 16), int(size, 16), in_seg))
    return syms, segs


def data_segments(buf):
    """[(address, file offset, length, kind)] for the module's data segments.
    kind is 'active' (placed by an i32.const offset) or 'passive'. An active
    segment placed by any other expression (a PIC module's global.get) is
    refused outright: its address is not a fact of the file."""
    if buf[:8] != b"\0asm\x01\0\0\0":
        die("not a wasm module")
    segs = []
    pos = 8
    while pos < len(buf):
        sid = buf[pos]
        size, pos = uleb(buf, pos + 1)
        end = pos + size
        if sid == WASM_DATA_SECTION:
            n, p = uleb(buf, pos)
            for _ in range(n):
                flags, p = uleb(buf, p)
                addr = None
                kind = "passive"
                if flags in (0, 2):
                    if flags == 2:
                        _mem, p = uleb(buf, p)
                    if buf[p] != I32_CONST:
                        die("data segment %d is placed by opcode 0x%02x, not "
                            "i32.const" % (len(segs), buf[p]))
                    value, p = sleb(buf, p + 1)
                    if buf[p] != END:
                        die("data segment %d's offset is not a lone i32.const"
                            % len(segs))
                    addr, kind, p = value & 0xFFFFFFFF, "active", p + 1
                elif flags != 1:
                    die("data segment flags %d unknown" % flags)
                length, p = uleb(buf, p)
                segs.append((addr, p, length, kind))
                p += length
            if p != end:
                die("data section did not parse to its end")
        pos = end
    return segs


def table_file_offset(buf, base, length):
    for addr, off, seglen, kind in data_segments(buf):
        if kind != "active":
            continue
        if addr <= base and base + length <= addr + seglen:
            return off + (base - addr)
    return None


def main(argv):
    if len(argv) != 4:
        sys.exit("usage: patch_ov_addrs.py <module.wasm> <link.map> "
                 "<overlay_statics.c>")
    module, mapfile, statics = argv[1:]

    rows = ROW.findall(open(statics).read())
    if not rows:
        die("no rows in %s" % statics)
    if not re.search(r"\b%s\[%d\]" % (TABLE, len(rows)), open(statics).read()):
        die("%s has no %s[%d]; was it generated with --addr-table?"
            % (statics, TABLE, len(rows)))

    syms, map_segs = map_data(mapfile)
    if not syms:
        die("%s lists no data symbols; is it a wasm-ld -Map file?" % mapfile)
    if len(syms.get(TABLE, [])) != 1:
        die("%s is not one data symbol in %s" % (TABLE, mapfile))
    base, tsize, _in = syms[TABLE][0]
    if tsize != len(rows) * ENTRY:
        die("%s is %d bytes and %d rows need %d; the generated table and "
            "the link disagree" % (TABLE, tsize, len(rows), len(rows) * ENTRY))

    blob = bytearray()
    placed = ambiguous = 0
    dropped = []
    for _ov, name, objsize in rows:
        where = syms.get(name, [])
        addrs = sorted({a for a, _s, _i in where})
        if not addrs:
            blob += struct.pack("<iI", NOT_AN_OFFSET, DROPPED)
            dropped.append(name)
            continue
        if len(addrs) > 1:
            blob += struct.pack("<iI", NOT_AN_OFFSET, len(addrs))
            ambiguous += 1
            continue
        size = max(s for a, s, _i in where if a == addrs[0])
        if size != int(objsize):
            die("%s is %d bytes in the link and %s in the object; "
                "overlay_statics.c is stale" % (name, size, objsize))
        blob += struct.pack("<iI", addrs[0] - base, size)
        placed += 1

    with open(module, "rb") as fh:
        before = fh.read()
    laid_out = {(a, s) for _n, a, s in map_segs}
    for addr, _o, length, kind in data_segments(before):
        if kind == "active" and (addr, length) not in laid_out:
            die("the module's data segment at 0x%08X (0x%x bytes) is not an "
                "output segment in %s; this map and this module are not one "
                "link" % (addr, length, mapfile))
    off = table_file_offset(before, base, len(blob))
    if off is None:
        kinds = [k for a, _o, n, k in data_segments(before)
                 if k != "active"]
        die("%s at 0x%08X is in no active data segment with an i32.const "
            "offset (%s); a zero initialiser would put it in .bss, which "
            "has no bytes in the module, and a passive segment has no "
            "address to patch against"
            % (TABLE, base, ", ".join(kinds) or "no other segments"))
    current = before[off:off + len(blob)]
    if current != SENTINEL * len(rows) and current != bytes(blob):
        die("%s at file offset 0x%x does not hold the generator's sentinel; "
            "this map and this module are not one link" % (TABLE, off))

    with open(module, "r+b") as fh:
        fh.seek(off)
        fh.write(blob)

    # Read it all back: the table decodes to what was written through a
    # fresh parse of the module, and nothing else in the file changed.
    with open(module, "rb") as fh:
        after = fh.read()
    if len(after) != len(before):
        die("the module changed size")
    if after[:off] != before[:off] or \
            after[off + len(blob):] != before[off + len(blob):]:
        die("bytes outside the table changed")
    reoff = table_file_offset(after, base, len(blob))
    if reoff != off or after[reoff:reoff + len(blob)] != bytes(blob):
        die("the table did not read back")

    print("  OVADDR  %d of %d overlay statics placed, %d ambiguous, "
          "%d dropped by the link%s"
          % (placed, len(rows), ambiguous, len(dropped),
             " (" + ", ".join(dropped) + ")" if dropped else ""))


if __name__ == "__main__":
    main(sys.argv)
