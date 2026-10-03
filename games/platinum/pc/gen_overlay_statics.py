#!/usr/bin/env python3
"""List each overlay's writable host statics, from nm over its objects.

On hardware, FS_LoadOverlay copies the overlay image out of the ROM and
that reset is the .data/.bss the overlay sees on every load. This port
links overlay TUs as ordinary host objects, so those statics are
initialized once. FS_StartOverlay snapshots the CRT-initialized bytes
on the first load and copies them back on every later one.

Addresses cannot live in this table: they are host addresses, and a
table of pointers large enough to name them would move every one of
them by existing (the same two-pass problem pc_sym.c records). Names
and object-nm sizes are layout-independent. pc_fs_overlay.c resolves
them at runtime against the binary's own .symtab.

--addr-table is for a host with no .symtab at runtime. It adds a second
array, the same length as the first and filled with a sentinel, for a
post-link step to write the addresses into. That does not reopen the
two-pass problem, because the array is at its final size before the link
and only its CONTENTS are written afterwards; nothing moves. It is
opt-in so that a build that does not need it links the same bytes it
always did.

Usage: gen_overlay_statics.py [--addr-table] overlay-map.txt obj/game out.c
"""

import collections
import os
import subprocess
import sys


def meson_flat(rel_o):
    """src/overlay005/loaded_map_buffers.o -> src_overlay005_loaded_map_buffers.c"""
    if not rel_o.endswith(".o"):
        return None
    return rel_o[:-2].replace("/", "_") + ".c"


# Read-only once the loader has finished with it, so not an overlay static
# however `nm` classes it. A PIC build puts a `const` object that needs a
# relocation here rather than in .rodata, `static const WindowTemplate
# sWinTemplates[]` in three of the frontier applications, which nm then calls
# `d`. Nothing writes them, the runtime has no snapshot to take, and counting
# them made three overlays claim one name and stopped this generator. A
# non-PIC build has no such section, so this changes nothing for the ELF, the
# armhf or the Windows toolchain.
RELRO_SECTIONS = (".data.rel.ro",)


def nm_writable(path):
    """Yield (name, size, type) for writable defined symbols in an object."""
    try:
        # sysv format, because the section is the question and the default
        # format does not print it.
        out = subprocess.check_output(
            ["nm", "--format=sysv", "-S", "--defined-only", path],
            text=True, errors="replace")
    except (OSError, subprocess.CalledProcessError):
        return
    for line in out.splitlines():
        parts = [p.strip() for p in line.split("|")]
        # name | value | class | type | size | line | section
        if len(parts) < 7:
            continue
        name, typ, size_s, section = parts[0], parts[2], parts[4], parts[6]
        if not size_s or not all(c in "0123456789abcdefABCDEF" for c in size_s):
            continue
        if typ not in "dDbBC":
            continue
        if any(section == s or section.startswith(s + ".")
               for s in RELRO_SECTIONS):
            continue
        size = int(size_s, 16)
        if size == 0 or not name:
            continue
        yield name, size, typ


def main():
    argv = sys.argv[1:]
    addr_table = "--addr-table" in argv
    argv = [a for a in argv if a != "--addr-table"]
    if len(argv) != 3:
        sys.exit("usage: gen_overlay_statics.py [--addr-table] "
                 "overlay-map.txt obj/game out.c")
    omap_path, objroot, out_path = argv

    ov_of = {}
    with open(omap_path) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            stem, ov = line.rsplit(None, 1)
            ov_of[stem.split("/", 1)[-1]] = int(ov)

    objs = {}
    for dirpath, _, files in os.walk(objroot):
        for fn in files:
            if not fn.endswith(".o"):
                continue
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, objroot).replace(os.sep, "/")
            flat = meson_flat(rel)
            if flat:
                objs[flat] = path

    # (overlay, name, size) -> unique. Two TUs in one overlay that share a
    # local name (the only case today is ov95's compiler-generated v2.0)
    # collapse to one row; the runtime lookup takes every address of that
    # name. The same name in two overlays is a collision we cannot assign
    # without file origin, so it is an error rather than a guess.
    rows = {}
    name_overlays = collections.defaultdict(set)
    for flat, ov in ov_of.items():
        path = objs.get(flat)
        if path is None:
            continue
        for name, size, _typ in nm_writable(path):
            rows[(ov, name, size)] = True
            name_overlays[name].add(ov)

    collisions = sorted(n for n, ovs in name_overlays.items() if len(ovs) > 1)
    if collisions:
        sys.exit("gen_overlay_statics: writable name(s) live in more than "
                 "one overlay; the runtime lookup cannot tell them apart: "
                 + ", ".join(collisions))

    items = sorted(rows)
    total = sum(size for _ov, _n, size in items)
    overlays = sorted({ov for ov, _n, _s in items})

    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or ".", exist_ok=True)
    with open(out_path, "w") as fh:
        fh.write("/* generated by pc/gen_overlay_statics.py, do not edit */\n")
        fh.write("/* %d writable symbol(s) in %d overlay(s), %d bytes */\n"
                 % (len(items), len(overlays), total))
        fh.write("\n")
        fh.write("struct pc_ov_static_desc {\n")
        fh.write("    unsigned overlay;\n")
        fh.write("    const char *name;\n")
        fh.write("    unsigned size;\n")
        fh.write("};\n\n")
        fh.write("const struct pc_ov_static_desc pc_ov_static_desc[] = {\n")
        for ov, name, size in items:
            fh.write("    { %u, \"%s\", %u },\n" % (ov, name, size))
        fh.write("};\n")
        fh.write("const int pc_ov_static_desc_n = %d;\n" % len(items))
        if addr_table:
            # Not const, and the sentinel is not zero: a zero initialiser
            # would put this in .bss, which has no bytes in the file for a
            # post-link step to write. -1 is also what "nobody filled this
            # in" looks like at runtime, so an unpatched binary says so
            # instead of restoring from wherever offset 0 lands.
            fh.write("\n")
            fh.write("struct pc_ov_static_addr {\n")
            fh.write("    int off;       /* from &pc_ov_static_addr[0] */\n")
            fh.write("    unsigned size; /* the linker's, not the object's */\n")
            fh.write("};\n\n")
            fh.write("struct pc_ov_static_addr "
                     "pc_ov_static_addr[%d] = {\n" % len(items))
            for _ov, _name, _size in items:
                fh.write("    { -1, 0 },\n")
            fh.write("};\n")


if __name__ == "__main__":
    main()
