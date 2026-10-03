#!/usr/bin/env python3
"""
recover_asm: bring back the assembly a decompilation deleted, and prove it is
the ROM's own code.

Decompiling deletes the .s, so for an already-decompiled function there is no
mechanical twin anywhere in the working tree. Hand-decompiled C then gets the
link with no comparison and no vote, and the thing that would arbitrate it is
gone from the checkout.

It is not gone from git. This reads it back.

Two sources, and only one of them is an authority.

  * git history is the index. It answers where and how far: a recovered .s
    carries `NAME: ; 0xADDR` per label, which is an address, an extent and an
    ARM-or-Thumb state in one artifact. It is not ground truth; it is an
    earlier disassembly, with its own blind spots, such as a `bl` whose
    target's `arm_func_start` it had lost.
  * the dump is the authority. `--verify` assembles what history gave back and
    compares it byte for byte against the same span of the ROM. Where the two
    disagree at a known address, that disagreement is the finding and is
    reported rather than smoothed over.

The address map is built from every blob and not from one file, because
reproducing a function's bytes needs its neighbours' addresses: a `bl` encodes
a PC-relative offset, and the neighbour may itself have been decompiled and
deleted. One pass over every .s blob git has ever held answers all of them.

Dialect: the tree's .s is mwasm, where `;` starts a comment, and GNU as reads
`;` as a statement separator. Every recovered file is translated before it is
assembled, which is why --verify needs binutils.

Nothing this writes is tracked. The output stays in build/, because re-adding a
file the decomp deliberately deleted would fight the next upstream sync.

Usage:
    recover_asm.py --list                       # what history can give back
    recover_asm.py --out DIR PATH [PATH ...]    # write those .s
    recover_asm.py --out DIR --verify --rom R PATH [...]
"""

import argparse
import os
import re
import subprocess
import sys

# `NAME: ; 0xADDR`, a label and the address the disassembler put on it. Both
# code and data carry it, which is why .bss symbols resolve too.
# re.M matters and its absence is silent: this is run with finditer() over a
# whole `cat-file --batch` stream, where a `^` that only matches at offset zero
# finds one label in two thousand blobs and reports an empty map rather than an
# error.
LABEL_ADDR = re.compile(r"^\s*([A-Za-z_.$][\w.$]*)\s*:\s*;\s*0x([0-9A-Fa-f]{6,8})",
                        re.M)

# A label whose *name* is its address, which is how armrec.py's ADDR_IN_NAME
# reads the same thing. Literal pools are written this way and carry no comment.
ADDR_IN_NAME = re.compile(r"^\s*_([0-9A-Fa-f]{6,8})\s*:")

# `;` is a comment in mwasm and a statement separator in GNU as.
COMMENT = re.compile(r";")

FUNC_START = re.compile(r"^\s*(arm_func_start|thumb_func_start|"
                        r"local_arm_func_start|non_word_aligned_thumb_func_start)\s+(\S+)")


def git(root, *args):
    r = subprocess.run(["git", "-C", root] + list(args),
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("git %s: %s" % (" ".join(args), r.stderr.strip()))
    return r.stdout


def all_s_blobs(root):
    """{path: sha} for every .s git has ever held, newest revision winning.

    `rev-list --objects --all` names every object with the path it was stored
    under. A path that was edited many times has many blobs; the one wanted is
    the last before deletion, which is the one whose commit is newest, so the
    walk is ordered rather than a dict comprehension.
    """
    out = git(root, "rev-list", "--objects", "--all")
    seen = {}
    for line in out.splitlines():
        f = line.split(maxsplit=1)
        if len(f) == 2 and f[1].endswith(".s"):
            seen.setdefault(f[1], []).append(f[0])
    return seen


def blob_text(root, sha):
    r = subprocess.run(["git", "-C", root, "cat-file", "-p", sha],
                       capture_output=True)
    if r.returncode != 0:
        return None
    return r.stdout.decode("utf-8", "replace")


def newest_blob(root, path, shas):
    """The blob of `path` from just before it was deleted.

    Picking by object id is meaningless and picking the largest is a guess, so
    this asks git which commit removed the path and reads the parent's version.
    Falls back to the longest blob when the path is still present or the delete
    cannot be found.
    """
    log = git(root, "log", "--all", "--format=%H", "--diff-filter=D",
              "--", path).split()
    if log:
        txt = blob_text(root, "%s^:%s" % (log[0], path))
        if txt:
            return txt, log[0]
    best = None
    for sha in shas:
        txt = blob_text(root, sha)
        if txt and (best is None or len(txt) > len(best)):
            best = txt
    return best, None


def address_map(root):
    """({symbol: address}, {symbol: {addresses}}) over every .s blob in history.

    A symbol with two addresses is refused, not guessed, and that is the whole
    design of this function. History holds many revisions of the same path, and
    a re-disassembly can move a symbol; 33 overlays also share one load address,
    so the same name can legitimately mean different things in different files.
    Resolving those by "newest wins" needs a revision order this does not have
   , `cat-file --batch` answers in the order it is asked, which is sha order,
    which is arbitrary.

    An arbitrary pick is not harmless here. A `bl` encodes a PC-relative offset,
    so a wrong address does not fail: it assembles cleanly and produces a wrong
    word, which then reads as "the dump disagrees with history", a finding
    about the ROM that is really a bug in this file. That happened, at
    0x020DBDD8 in CTRDG_backup.s: `EBFFB8AD` against the ROM's `EBFFB902`, one
    `bl` off by 0x154 bytes.

    So conflicts are dropped from the map and reported. An unresolved symbol
    fails at the linker with its own name attached, which is the loud failure
    the ambiguous one was pretending not to be.
    """
    seen = {}
    blobs = all_s_blobs(root)
    shas = sorted({s for v in blobs.values() for s in v})
    if not shas:
        return {}, {}
    # One batch call rather than one per blob: this is ~2,300 objects.
    proc = subprocess.run(["git", "-C", root, "cat-file", "--batch"],
                          input="\n".join(shas), capture_output=True, text=True)
    for m in LABEL_ADDR.finditer(proc.stdout):
        seen.setdefault(m.group(1), set()).add(int(m.group(2), 16))
    amap = {n: next(iter(a)) for n, a in seen.items() if len(a) == 1}
    conflicts = {n: a for n, a in seen.items() if len(a) > 1}
    return amap, conflicts


def to_gnu(text):
    """mwasm -> GNU as: `;` comments become `@`, func-start macros go away.

    The macro is dropped rather than defined because the label it precedes is
    already on its own line in every file here, so the macro contributes no
    bytes, and defining it would need asm/macros.inc, which is a working-tree
    file this deliberately does not depend on.
    """
    out = []
    for line in text.splitlines():
        if FUNC_START.match(line):
            continue
        out.append(COMMENT.sub("@", line, count=1))
    return "\n".join(out) + "\n"


def text_span(text):
    """(lowest, highest+4) over the addresses in the .text half of one file.

    Two spellings, and missing the second truncates the span rather than
    failing: a function's labels carry `; 0xADDR`, but the literal pool behind
    it is written `_020CCBDC: .word ...` with the address only in the name.
    Those pools sit at the *end* of the file, so reading only the commented
    form stops the extent short of the last few words, 392 bytes of a 500-byte
    function, which assembles fine and then disagrees with the dump for a reason
    that has nothing to do with the dump.
    """
    addrs, in_text = [], False
    for line in text.splitlines():
        s = line.strip()
        if s.startswith(".text"):
            in_text = True
            continue
        if s.startswith(".section"):
            in_text = ".text" in s
            continue
        if not in_text:
            continue
        m = LABEL_ADDR.match(line)
        if m:
            addrs.append(int(m.group(2), 16))
            continue
        m = ADDR_IN_NAME.match(line)
        if m:
            addrs.append(int(m.group(1), 16))
    if not addrs:
        return None
    return min(addrs), max(addrs) + 4


def externs(text, amap, defined):
    """{name: addr} for symbols the file names but does not define."""
    want = {}
    for m in re.finditer(r"(?<![\w.$])([A-Za-z_][\w.$]*)", text):
        n = m.group(1)
        if n in defined or n in want:
            continue
        if n in amap:
            want[n] = amap[n]
    return want


def rom_slice(rom_path, addr, length):
    """The ROM's own bytes at a guest address in the ARM9 static module."""
    import struct
    rom = open(rom_path, "rb").read()
    off = struct.unpack_from("<I", rom, 0x20)[0]
    ram = struct.unpack_from("<I", rom, 0x28)[0]
    size = struct.unpack_from("<I", rom, 0x2C)[0]
    if not (ram <= addr and addr + length <= ram + size):
        return None
    start = off + (addr - ram)
    return rom[start:start + length]


def verify(name, text, amap, rom_path, workdir):
    """Assemble the recovered text and diff it against the dump.

    Returns (ok, detail). A missing assembler is not a failure: binutils is
    optional here on purpose, because the *build* never needs it, armrec
    reads the .s directly, and only this check does.
    """
    span = text_span(text)
    if span is None:
        return None, "no .text addresses"
    base, end = span
    length = end - base

    gnu = to_gnu(text)
    # .text only: the .bss half places no bytes and its `.space` would.
    lines, keep = [], False
    for line in gnu.splitlines():
        s = line.strip()
        if s.startswith(".text"):
            keep = True
            continue
        if s.startswith(".section"):
            keep = ".text" in s
            continue
        if keep:
            lines.append(line)

    # `defined` is what the *assembled* text defines, not what the file does.
    # The .bss half was just dropped, so its labels are externs now, and they
    # have to be, because their addresses are what the literal pool encodes.
    body = "\n".join(lines)
    defined = {m.group(1) for m in LABEL_ADDR.finditer(body)}
    defined |= {m.group(1) for m in re.finditer(r"^\s*([A-Za-z_.$][\w.$]*)\s*:",
                                                body, re.M)}
    ext = externs(body, amap, defined)

    src = os.path.join(workdir, name + ".gnu.s")
    obj = os.path.join(workdir, name + ".o")
    elf = os.path.join(workdir, name + ".elf")
    binf = os.path.join(workdir, name + ".bin")
    open(src, "w").write("\n".join(lines) + "\n")

    asm = ["arm-none-eabi-as", "-EL", "-mcpu=arm946e-s", "-o", obj, src]
    for n, a in sorted(ext.items()):
        asm += ["--defsym", "%s=0x%08X" % (n, a)]
    try:
        r = subprocess.run(asm, capture_output=True, text=True)
    except FileNotFoundError:
        return None, "arm-none-eabi-as not installed"
    if r.returncode != 0:
        return False, "assembler: " + r.stderr.strip().splitlines()[0]

    r = subprocess.run(["arm-none-eabi-ld", "-EL",
                        "--section-start=.text=0x%08X" % base,
                        "-o", elf, obj], capture_output=True, text=True)
    if r.returncode != 0:
        return False, "link: " + r.stderr.strip().splitlines()[-1]
    subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", elf, binf],
                   check=True)

    got = open(binf, "rb").read()
    want = rom_slice(rom_path, base, length)
    if want is None:
        return None, "0x%08X is not in the arm9 static module" % base
    if len(got) != len(want):
        return False, "assembled %d bytes, rom span is %d" % (len(got), len(want))
    bad = [i for i, (x, y) in enumerate(zip(got, want)) if x != y]
    if bad:
        return False, ("%d of %d bytes differ, first at 0x%08X"
                       % (len(bad), len(want), base + bad[0]))
    return True, "%d bytes identical at 0x%08X..0x%08X" % (length, base, end)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="*", help="repo-relative .s paths to recover")
    ap.add_argument("--root", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    ap.add_argument("--out", help="directory to write recovered .s into")
    ap.add_argument("--list", action="store_true",
                    help="report every deleted .s history can give back")
    ap.add_argument("--verify", action="store_true",
                    help="assemble and diff against --rom")
    ap.add_argument("--rom", help="the dump, for --verify")
    args = ap.parse_args()

    root = os.path.normpath(args.root)

    if args.list:
        blobs = all_s_blobs(root)
        gone = [p for p in sorted(blobs)
                if not os.path.exists(os.path.join(root, p))]
        print("%d .s paths in history, %d absent from the checkout"
              % (len(blobs), len(gone)))
        for p in gone:
            print("  " + p)
        return 0

    if not args.paths:
        print("nothing to do: pass paths, or --list", file=sys.stderr)
        return 2

    amap, conflicts = address_map(root) if args.verify else ({}, {})
    if args.verify and conflicts:
        print("note: %d symbols have more than one address in history "
              "and are left unresolved rather than guessed" % len(conflicts))
    if args.out:
        os.makedirs(args.out, exist_ok=True)

    rc = 0
    for path in args.paths:
        blobs = all_s_blobs(root).get(path)
        if not blobs:
            print("MISS  %s (no blob in history)" % path)
            rc = 1
            continue
        text, killed = newest_blob(root, path, blobs)
        if text is None:
            print("MISS  %s (blob unreadable)" % path)
            rc = 1
            continue
        name = os.path.basename(path)[:-2]
        if args.out:
            dst = os.path.join(args.out, os.path.basename(path))
            open(dst, "w").write(text)
        note = (" deleted by %s" % killed[:9]) if killed else ""
        print("OK    %s, %d lines%s" % (path, len(text.splitlines()), note))

        if args.verify:
            if not args.rom:
                print("      SKIP verify: no --rom")
                continue
            ok, detail = verify(name, text, amap, args.rom,
                                args.out or "/tmp")
            if ok is None:
                print("      SKIP verify: %s" % detail)
            elif ok:
                print("      VERIFIED against the dump: %s" % detail)
            else:
                print("      DISAGREES with the dump: %s" % detail)
                rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
