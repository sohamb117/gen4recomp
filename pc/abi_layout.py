#!/usr/bin/env python3
"""Compare struct layout between the ROM build's compiler and the host's.

The port compiles pret's headers with gcc -m32 instead of the Metrowerks ARM
compiler the ROM build uses. Both see the same declarations, so any layout
they disagree on is the ABI and nothing else, and the port's whole 32-bit
decision rests on those two ABIs agreeing about the structs the game actually
uses. Nothing else in the tree checks that.

Neither compiler has to be asked: both already record what they decided.
mwcc emits DWARF 2 under `-sym on` and gcc emits DWARF 5 under `-g`, so the
objects sitting in build/rom and build/pc are the two answers, and this reads
them out of the objects the real builds produced rather than out of a
purpose-built probe that might be compiled differently.

  $ python3 pc/abi_layout.py                 # the comparison, with a summary
  $ python3 pc/abi_layout.py --verbose       # every differing member
  $ python3 pc/abi_layout.py --dump BattleMon    # one struct's two layouts
  $ python3 pc/abi_layout.py --json out.json # for something else to read
  $ python3 pc/abi_layout.py --name OSContext ...   # only these
  $ python3 pc/abi_layout.py --rom-objects DIR --host-objects DIR

Exit status is 0 when the two agree on every struct they share and 1 when
they do not, so it works as a gate.
"""

import argparse
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM_OBJ = os.path.join(ROOT, "build", "rom")
PC_OBJ = os.path.join(ROOT, "build", "pc", "obj")

# A DIE header: " <2><100>: Abbrev Number: 133 (DW_TAG_member)"
DIE_RE = re.compile(r"^\s*<(\d+)><([0-9a-f]+)>:\s+Abbrev Number:\s+\d+(?:\s+\((\w+)\))?")
# An attribute: "    <ed>   DW_AT_name        : FieldOverworldSave"
ATTR_RE = re.compile(r"^\s*<[0-9a-f]+>\s+(DW_AT_\w+)\s*:\s*(.*?)\s*$")
# gcc writes strings indirectly: "(indirect string, offset: 0x2c07): CPContext".
# The offset is not always hex, readelf prints the first string's as plain 0,
# which is one member of one struct in this tree and was worth an hour.
INDIRECT_RE = re.compile(
    r"^\(indirect (?:line )?string, offset: (?:0x[0-9a-f]+|\d+)\):\s*(.*)$")
TYPE_REF_RE = re.compile(r"<0x([0-9a-f]+)>")
# mwcc writes member locations as a location expression, and readelf decodes it
# for us at the end of the line: "5 byte block: c 0 0 0 0 \t(DW_OP_const4u: 0)"
LOCEXPR_RE = re.compile(r"\(DW_OP_(?:const\d+u|plus_uconst|constu):\s*(-?\d+)\)")

MEMBER_TAGS = ("DW_TAG_member",)
RECORD_TAGS = ("DW_TAG_structure_type", "DW_TAG_union_type")


def attr_value(raw):
    """The value of an attribute line, with readelf's decorations removed."""
    m = INDIRECT_RE.match(raw)
    if m:
        return m.group(1)
    return raw


def attr_int(raw):
    """An attribute's integer value, however readelf chose to spell it."""
    m = LOCEXPR_RE.search(raw)
    if m:
        return int(m.group(1))
    tok = raw.split()[0] if raw.split() else ""
    tok = tok.rstrip(",")
    try:
        return int(tok, 0)
    except ValueError:
        return None


class Die:
    __slots__ = ("depth", "off", "tag", "attrs", "children")

    def __init__(self, depth, off, tag):
        self.depth = depth
        self.off = off
        self.tag = tag
        self.attrs = {}
        self.children = []

    def get(self, name):
        return self.attrs.get(name)

    def name(self):
        v = self.attrs.get("DW_AT_name")
        return attr_value(v) if v is not None else None


def parse_dies(text):
    """The DIE forest of one compilation unit stream, and its producer."""
    roots = []
    stack = []
    producer = None
    cur = None
    for line in text.splitlines():
        m = DIE_RE.match(line)
        if m:
            depth, off, tag = int(m.group(1)), int(m.group(2), 16), m.group(3)
            die = Die(depth, off, tag or "")
            while stack and stack[-1].depth >= depth:
                stack.pop()
            if stack:
                stack[-1].children.append(die)
            else:
                roots.append(die)
            stack.append(die)
            cur = die
            continue
        m = ATTR_RE.match(line)
        if m and cur is not None:
            cur.attrs[m.group(1)] = m.group(2)
            if m.group(1) == "DW_AT_producer":
                producer = attr_value(m.group(2))
    return roots, producer


def member_bit_offset(die, struct_die):
    """A member's offset from the start of its struct, in bits.

    The two compilers describe a bitfield in the two ways DWARF allows, and
    they are not the same measurement. gcc's DW_AT_data_bit_offset is already
    what we want. mwcc writes DWARF 2's older triple, the byte offset of the
    storage unit, the unit's size, and the bit position counted DOWN from the
    unit's most significant bit, so a little-endian target has to be turned
    back into a distance from the bottom.
    """
    dbo = die.get("DW_AT_data_bit_offset")
    if dbo is not None:
        return attr_int(dbo)

    loc = die.get("DW_AT_data_member_location")
    byte_off = attr_int(loc) if loc is not None else 0
    if byte_off is None:
        return None

    bit_size = die.get("DW_AT_bit_size")
    if bit_size is None:
        return byte_off * 8

    bit_off = attr_int(die.get("DW_AT_bit_offset") or "0")
    unit = die.get("DW_AT_byte_size")
    unit = attr_int(unit) if unit is not None else None
    if unit is None or bit_off is None:
        return None
    return byte_off * 8 + (unit * 8 - bit_off - attr_int(bit_size))


def resolve(by_off, die, attr="DW_AT_type"):
    ref = die.get(attr)
    if ref is None:
        return None
    m = TYPE_REF_RE.search(ref)
    return by_off.get(int(m.group(1), 16)) if m else None


def strip_typedefs(by_off, die, depth=8):
    """Past typedefs and cv-qualifiers to the type that has a layout."""
    seen = 0
    while die is not None and seen < depth and die.tag in (
            "DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type"):
        die = resolve(by_off, die)
        seen += 1
    return die


def record_layout(by_off, die, depth=0):
    """(size_in_bytes, [(member, bit_offset, bit_width_or_None)]) or None.

    Anonymous struct and union members are flattened into the parent at their
    own offset, on both sides, because the two compilers do not describe them
    the same way. gcc emits one unnamed member holding an unnamed aggregate;
    mwcc emits a member it calls `__anon` AND every field inside it, hoisted to
    the parent. Neither is more right, and the layout underneath is identical
    either way, flattening is what makes them the same list of names. The
    duplicate names mwcc produces collapse because the offsets agree.
    """
    size = die.get("DW_AT_byte_size")
    if die.get("DW_AT_declaration") is not None or size is None:
        return None
    size = attr_int(size)
    if size is None:
        return None

    members = []
    for child in die.children:
        if child.tag not in MEMBER_TAGS:
            continue
        off = member_bit_offset(child, die)
        if off is None:
            continue
        name = child.name()
        inner = strip_typedefs(by_off, resolve(by_off, child))
        anonymous = (name is None or name == "__anon")
        if anonymous and inner is not None and inner.tag in RECORD_TAGS and depth < 8:
            nested = record_layout(by_off, inner, depth + 1)
            if nested:
                members.extend((n, off + o, w) for n, o, w in nested[1])
                continue
        if name is None:
            continue
        width = child.get("DW_AT_bit_size")
        members.append((name, off, attr_int(width) if width is not None else None))

    # mwcc describes a type it only ever saw declared as a zero-byte record
    # with no members rather than setting DW_AT_declaration. That is an opaque
    # handle, not a layout, and recording it would shadow the real definition
    # from the one translation unit that has it.
    if size == 0 and not members:
        return None
    members.sort(key=lambda m: (m[1], m[0]))
    return size, members


def collect(roots):
    """Every named record layout in one CU, keyed by name.

    A struct declared through a typedef of an anonymous struct is keyed by the
    typedef's name; that is the only name it has in the source, mwcc records
    it that way already, and gcc leaves the struct itself unnamed.
    """
    by_off = {}
    out = {}

    def walk(die):
        by_off[die.off] = die
        for child in die.children:
            walk(child)

    for r in roots:
        walk(r)

    for die in by_off.values():
        if die.tag in RECORD_TAGS and die.name():
            layout = record_layout(by_off, die)
            if layout:
                out.setdefault(die.name(), layout)

    for die in by_off.values():
        if die.tag != "DW_TAG_typedef":
            continue
        t = resolve(by_off, die)
        name = die.name()
        if name is None or t is None or t.tag not in RECORD_TAGS or t.name():
            continue
        layout = record_layout(by_off, t)
        if layout:
            out.setdefault(name, layout)
    return out


def read_object(path):
    """(producer, {name: layout}) for one object file, or (None, {})."""
    try:
        text = subprocess.run(
            ["readelf", "--debug-dump=info", path],
            check=True, capture_output=True, text=True, errors="replace",
        ).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None, {}
    if "DW_TAG_" not in text:
        return None, {}
    roots, producer = parse_dies(text)
    return producer, collect(roots)


def scan(paths, want_producer, jobs):
    """Merge every object's view. Returns (layouts, sources, conflicts, used).

    A name defined two different ways in two translation units is a fact about
    the tree, not about the ABI, so it is recorded separately and left out of
    the comparison; there would be no single layout to compare.
    """
    layouts, sources, conflicts = {}, {}, {}
    used = 0
    # Processes, not threads: readelf runs in parallel either way but the DWARF
    # parsing is Python, so threads spend the whole scan queued on the GIL.
    with ProcessPoolExecutor(max_workers=jobs) as pool:
        for path, (producer, found) in zip(
                paths, pool.map(read_object, paths, chunksize=8)):
            if not producer or want_producer not in producer:
                continue
            used += 1
            rel = os.path.relpath(path, ROOT)
            for name, layout in found.items():
                if name not in layouts:
                    layouts[name] = layout
                    sources[name] = rel
                elif layouts[name] != layout and name not in conflicts:
                    conflicts[name] = (sources[name], rel)
    for name in conflicts:
        del layouts[name]
    return layouts, sources, conflicts, used


def find_objects(root):
    out = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for f in filenames:
            if f.endswith(".o"):
                out.append(os.path.join(dirpath, f))
    out.sort()
    return out


def compare(rom, pc):
    """Every shared name the two compilers lay out differently."""
    diffs = []
    for name in sorted(set(rom) & set(pc)):
        r_size, r_members = rom[name]
        p_size, p_members = pc[name]
        r_at = {m[0]: (m[1], m[2]) for m in r_members}
        p_at = {m[0]: (m[1], m[2]) for m in p_members}
        moved = [
            (m, r_at[m], p_at[m])
            for m in (n for n, _, _ in r_members if n in p_at)
            if r_at[m] != p_at[m]
        ]
        # A member one side does not have at all is a declaration difference:
        # A different #ifdef, not a different ABI, so it is reported
        # apart from the offsets.
        only_rom = [n for n in r_at if n not in p_at]
        only_pc = [n for n in p_at if n not in r_at]
        if r_size != p_size or moved or only_rom or only_pc:
            diffs.append({
                "name": name,
                "rom_size": r_size,
                "pc_size": p_size,
                "moved": moved,
                "only_rom": only_rom,
                "only_pc": only_pc,
            })
    return diffs


def bits(n):
    return "%d" % (n // 8) if n % 8 == 0 else "%d.%d" % (n // 8, n % 8)


def dump(name, rom, pc):
    """Both compilers' whole view of one struct, member by member.

    A difference report says which members moved; this says what the two
    layouts actually are, which is what you need to see the shape of the
    disagreement rather than infer it from the first field that shifted.
    """
    lines = ["%s:" % name]
    r = rom.get(name)
    p = pc.get(name)
    if r is None or p is None:
        lines.append("  present in %s only" % ("rom" if r else "host" if p else "neither"))
        return lines
    lines.append("  sizeof: rom %d, host %d%s"
                 % (r[0], p[0], "" if r[0] == p[0] else "   <-- differs"))
    r_at = {m[0]: (m[1], m[2]) for m in r[1]}
    p_at = {m[0]: (m[1], m[2]) for m in p[1]}
    order = [m[0] for m in r[1]] + [m[0] for m in p[1] if m[0] not in r_at]
    seen = set()
    for m in order:
        if m in seen:
            continue
        seen.add(m)
        ro, rw = r_at.get(m, (None, None))
        po, pw = p_at.get(m, (None, None))
        def cell(off, width):
            if off is None:
                return "-"
            return "+%s%s" % (bits(off), ":%d" % width if width is not None else "")
        lines.append("  %-34s rom %-14s host %-14s%s"
                     % (m, cell(ro, rw), cell(po, pw),
                        "" if (ro, rw) == (po, pw) else "  <--"))
    return lines


def describe(d, verbose):
    lines = []
    head = "  %s" % d["name"]
    if d["rom_size"] != d["pc_size"]:
        head += ": sizeof %d (rom) vs %d (host)" % (d["rom_size"], d["pc_size"])
    else:
        head += ": sizeof %d, %d member(s) moved" % (d["rom_size"], len(d["moved"]))
    lines.append(head)
    if d["only_rom"] or d["only_pc"]:
        lines.append("      members only in rom: %s" % (", ".join(d["only_rom"]) or "-"))
        lines.append("      members only in host: %s" % (", ".join(d["only_pc"]) or "-"))
    shown = d["moved"] if verbose else d["moved"][:4]
    for name, (ro, rw), (po, pw) in shown:
        w = "" if rw is None and pw is None else "  (bitfield %s vs %s bits)" % (rw, pw)
        lines.append("      %-32s rom +%-8s host +%-8s%s" % (name, bits(ro), bits(po), w))
    if len(d["moved"]) > len(shown):
        lines.append("      ... and %d more" % (len(d["moved"]) - len(shown)))
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--verbose", action="store_true", help="every differing member")
    ap.add_argument("--json", metavar="PATH", help="write the full comparison")
    ap.add_argument("--name", action="append", default=[],
                    help="compare only these structs (repeatable)")
    ap.add_argument("--dump", action="append", default=[],
                    help="print both compilers' whole layout for this struct")
    ap.add_argument("--rom-objects", default=ROM_OBJ, metavar="DIR",
                    help="where the ROM build's objects are (default build/rom)")
    ap.add_argument("--host-objects", default=PC_OBJ, metavar="DIR",
                    help="where the host build's objects are (default build/pc/obj)")
    ap.add_argument("--jobs", type=int, default=min(16, (os.cpu_count() or 4)))
    ap.add_argument("--quiet", action="store_true", help="one summary line only")
    args = ap.parse_args()

    rom_paths = find_objects(args.rom_objects)
    pc_paths = find_objects(args.host_objects)
    if not rom_paths:
        print("abi_layout: no ROM objects under %s, run `ninja -C build/rom`"
              % args.rom_objects, file=sys.stderr)
        return 2
    if not pc_paths:
        print("abi_layout: no host objects under %s, run `make -f pc/Makefile`"
              % args.host_objects, file=sys.stderr)
        return 2

    # The producer string is the filter, not the path: build/rom also holds the
    # host tools (nitrogfx, nitrorom), which are x86 objects gcc built.
    rom, _rsrc, rom_conflicts, rom_used = scan(rom_paths, "Metrowerks", args.jobs)
    pc, _psrc, pc_conflicts, pc_used = scan(pc_paths, "GNU C", args.jobs)

    if args.dump:
        for name in args.dump:
            print("\n".join(dump(name, rom, pc)))
            print()

    if args.name:
        keep = set(args.name)
        rom = {k: v for k, v in rom.items() if k in keep}
        pc = {k: v for k, v in pc.items() if k in keep}

    shared = sorted(set(rom) & set(pc))
    diffs = compare(rom, pc)

    if args.json:
        with open(args.json, "w") as f:
            json.dump({
                "rom_objects": rom_used, "host_objects": pc_used,
                "rom_records": len(rom), "host_records": len(pc),
                "shared": shared,
                "differences": diffs,
                "rom_conflicts": rom_conflicts, "host_conflicts": pc_conflicts,
            }, f, indent=1, sort_keys=True)

    summary = ("abi_layout: %d struct(s) shared by both compilers, %d disagree "
               "(%d mwcc objects, %d gcc objects)"
               % (len(shared), len(diffs), rom_used, pc_used))
    if not args.quiet:
        if diffs:
            print("Structs the two compilers lay out differently:")
            for d in diffs:
                print("\n".join(describe(d, args.verbose)))
            print()
        for label, conflicts in (("rom", rom_conflicts), ("host", pc_conflicts)):
            if conflicts:
                print("%d name(s) defined two different ways within the %s build "
                      "-- not comparable, left out:" % (len(conflicts), label))
                for name, (a, b) in sorted(conflicts.items()):
                    print("  %s: %s vs %s" % (name, a, b))
                print()
    print(summary)
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main())
