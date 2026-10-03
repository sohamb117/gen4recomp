#!/usr/bin/env python3
"""
Say whether a decompiled symbol that assembly still names in a `.word` is
Thumb code, ARM code, or not code at all.

A `.word sub_02058ED0` is a stored function pointer, and on ARM bit 0 of one
selects the instruction set a `BX` through it enters. armrec sets that bit from
`thumb_func_start`, which is the assembly's own word for it, but decompiling a
function deletes its .s and with it the marker. The name still encodes the
address, so armrec writes a bare guest address where the ROM has that address
plus one.

70 names, 197 `.word` sites. Most of them live in literal pools at the end of a
function rather than on lines beginning with `.word`, which is why a first scan
found far fewer.

Three sources of evidence, none of them a ROM.

  1. A `bl` or `blx` in the tree's own assembly. BL keeps the caller's
     instruction set and BLX switches it, and the disassembler wrote whichever
     the encoding had, so a call site names the callee's state. Settles 43 of
     the 70.

  2. The historical assembly, from every `.s` blob git has ever held. The
     marker that decompiling deleted is still in history. Settles 67, and the
     two sources agree on all 42 they share, which is the check that makes
     either one worth trusting.

     Matched by guest address and by overlay, because 33 overlays share the
     load address 0x021D74E0 and an address alone does not identify a function.
     Without that filter, history claims a `const struct` is a Thumb function.

  3. `nm` over the built game objects, for what is left. A name-encoded symbol
     that is not text is not a function, and a pointer to it must not have
     bit 0 set. This is the only source that needs anything outside the source
     tree.

Anything none of the three settles is refused rather than guessed, by name.

The output is checked in rather than derived at build time, because history is
mutable in a way the working tree is not: a rebase would silently change what
the build emits, a source export with no .git would build something different,
and source 3 would make every build depend on a previous build.
test_decomp_thumb re-runs this generator and requires the checked-in file to be
identical, so it cannot drift.

Regenerate, after a build, with:

    python3 tools/armrec/gen_decomp_thumb.py -o tools/armrec/decomp_thumb.txt
"""

import argparse
import collections
import glob
import os
import re
import subprocess
import sys


ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "..", ".."))

# A name the disassembler derived from an address, with its overlay if it has
# one. armrec's addr_from_name() accepts the same spellings plus a bare
# `_020424D0` local label, which is always defined in the file that uses it and
# therefore never reaches here.
NAME_RE = re.compile(r"^(?:ov(\d+)_|sub_|FUN_)([0-9A-Fa-f]{6,8})$")

# Everything an .s file can define. A `.word` naming any of these is resolved
# out of the assembly and needs nothing from history.
DEFINES = re.compile(
    r"^[ \t]*(?:(?:local_|non_word_aligned_)?(?:arm|thumb)_func_start"
    r"|arm_func_end|thumb_func_end|\.global)[ \t]+(\S+)", re.M)
LABEL = re.compile(r"^([A-Za-z_.$][\w.$]*):", re.M)

# A `.word` directive, with or without a label in front of it on the same line.
# **The label form is not a detail.** `_0202DAF4: .word sub_020311D0` is how a
# literal pool at the end of a function is written, and it holds most of these
# references: reading only lines that *begin* with `.word` finds 19 names and
# 107 sites where there are 70 and 197.
WORD = re.compile(r"^[ \t]*(?:[A-Za-z_.$][\w.$]*:[ \t]*)?"
                  r"\.(?:word|long|int)[ \t]+(.*?)[ \t]*(?:;.*)?$", re.M)

FUNC_START = re.compile(
    r"^[ \t]*(?:local_|non_word_aligned_)?(arm|thumb)_func_start[ \t]+(\S+)")
CALL = re.compile(r"^[ \t]*(bl|blx)[ \t]+([A-Za-z_.$][\w.$]*)[ \t]*(?:;.*)?$")
OVERLAY_DIR = re.compile(r"(?:^|/)overlays/(\d+)/")

FUNC_START_B = re.compile(
    rb"^[ \t]*(?:local_|non_word_aligned_)?(arm|thumb)_func_start[ \t]+(\S+)",
    re.M)
NAME_B = re.compile(rb"^(?:ov(\d+)_|sub_|FUN_)([0-9A-Fa-f]{6,8})$")


def key_of(name):
    """(overlay or None, guest address), what actually identifies a function."""
    m = NAME_RE.match(name)
    if not m:
        return None
    return (int(m.group(1)) if m.group(1) else None, int(m.group(2), 16))


def asm_files(root):
    out = []
    for dirpath, _dirs, files in os.walk(os.path.join(root, "arm9")):
        for name in files:
            if name.endswith(".s"):
                out.append(os.path.join(dirpath, name))
    return sorted(out)


def needed(root):
    """
    Every name-encoded `.word` target that no .s in the tree defines.

    That is precisely the set armrec cannot answer for: a name it can turn into
    a guest address but whose instruction-set state nothing in the working tree
    records. Returns ({name: number of `.word` operands}, {name: {file}}).
    """
    paths = asm_files(root)
    texts, defined = {}, set()
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        texts[path] = text
        defined.update(DEFINES.findall(text))
        defined.update(LABEL.findall(text))

    sites = collections.Counter()
    where = collections.defaultdict(set)
    for path, text in texts.items():
        for operands in WORD.findall(text):
            for tok in operands.split(","):
                tok = tok.strip()
                if tok and tok not in defined and NAME_RE.match(tok):
                    sites[tok] += 1
                    where[tok].add(os.path.relpath(path, root).replace(os.sep, "/"))
    return sites, where


def from_callers(root, names):
    """
    {name: (state, "bl in path:line")} from the tree's own `bl`/`blx`.

    BL keeps the caller's instruction set, BLX switches it, and the caller's is
    whatever its own arm_func_start/thumb_func_start says. A name that is
    called two ways is dropped rather than resolved, the oracle saying two
    things is the one case where it is not an oracle.
    """
    seen = collections.defaultdict(dict)
    for path in asm_files(root):
        rel = os.path.relpath(path, root).replace(os.sep, "/")
        state = None
        with open(path, encoding="utf-8", errors="replace") as fh:
            for lineno, line in enumerate(fh, 1):
                m = FUNC_START.match(line)
                if m:
                    state = m.group(1)
                    continue
                m = CALL.match(line)
                if not m or m.group(2) not in names or state is None:
                    continue
                target = state if m.group(1) == "bl" else \
                    ("arm" if state == "thumb" else "thumb")
                seen[m.group(2)].setdefault(
                    target, "%s %s:%d" % (m.group(1), rel, lineno))
    return {n: (list(v)[0], list(v.values())[0])
            for n, v in seen.items() if len(v) == 1}


def from_history(root, names):
    """
    {name: (state, "func_start in path")} from every .s blob git has ever held.

    `git rev-list --objects --all` names every object with the path it was
    stored under, so one `cat-file --batch` reads the lot. Deleted files are
    not enough on their own: decompiling one function out of a large .s edits
    that file rather than removing it, and the marker only exists in the older
    revision.

    The overlay in the *name* and the overlay in the *path* must agree with the
    target's, because 33 overlays share one load address.
    """
    want = {}
    for n in names:
        k = key_of(n)
        if k:
            want[k] = n
    objs = subprocess.run(["git", "-C", root, "rev-list", "--objects", "--all"],
                          capture_output=True, text=True)
    if objs.returncode != 0:
        return None, objs.stderr.strip() or "git rev-list failed"
    paths = {}
    for line in objs.stdout.splitlines():
        f = line.split(maxsplit=1)
        if len(f) == 2 and f[1].endswith(".s"):
            paths.setdefault(f[0], f[1])
    if not paths:
        return None, "no .s blobs in the history of this checkout"

    shas = sorted(paths)
    batch = subprocess.run(["git", "-C", root, "cat-file", "--batch"],
                           input=("\n".join(shas) + "\n").encode(),
                           capture_output=True)
    seen = collections.defaultdict(dict)
    buf, pos = batch.stdout, 0
    while pos < len(buf):
        end = buf.find(b"\n", pos)
        if end < 0:
            break
        header = buf[pos:end].split()
        pos = end + 1
        if len(header) < 3:
            continue
        size = int(header[2])
        blob = buf[pos:pos + size]
        pos += size + 1
        path = paths[header[0].decode()]
        m = OVERLAY_DIR.search(path)
        blob_ovl = int(m.group(1)) if m else None
        for kind, sym in FUNC_START_B.findall(blob):
            nm = NAME_B.match(sym)
            if nm is None:
                continue
            name_ovl = int(nm.group(1)) if nm.group(1) else None
            if name_ovl != blob_ovl:
                continue
            key = (blob_ovl, int(nm.group(2), 16))
            if key in want:
                seen[want[key]].setdefault(kind.decode(),
                                           "%s in %s" % (sym.decode(), path))
    return ({n: (list(v)[0], list(v.values())[0])
             for n, v in seen.items() if len(v) == 1}, None)


def from_objects(root, names, objects=None, nm="nm"):
    """
    {name: (state, "nm says R in path")} for names the build says are not code.

    A `.word` naming a decompiled *object*, `ov52_021D76C8` is a
    `const struct OverlayManagerTemplate`, is a data pointer, and bit 0 of
    one is part of the address. `nm`'s symbol class is the tree's own answer to
    which it is, and it is the same one gen_decomp_syms.py uses to decide what
    may be registered as a function.
    """
    if objects is None:
        objects = sorted(glob.glob(os.path.join(root, "build", "pc", "obj",
                                                "game", "**", "*.o"),
                                   recursive=True))
    objects = [o for o in objects if os.path.exists(o)]
    if not objects:
        return {}
    out = subprocess.run([nm, "--defined-only", "--print-file-name"] + objects,
                         capture_output=True, text=True).stdout
    found = {}
    for line in out.splitlines():
        obj, _, rest = line.partition(":")
        fields = rest.split()
        if len(fields) != 3:
            continue
        _addr, kind, sym = fields
        if sym not in names:
            continue
        state = "code" if kind in ("T", "W", "t", "w") else "data"
        found.setdefault(sym, {}).setdefault(
            state, "nm says %s in %s" % (kind, os.path.relpath(obj, root)))
    return {n: ("data", v["data"]) for n, v in found.items()
            if list(v) == ["data"]}


HEADER = """\
# Generated by tools/armrec/gen_decomp_thumb.py, do not edit by hand.
# Instruction-set state for every decompiled symbol the assembly still names
# in a bare `.word`: armrec sets bit 0 for Thumb, so a BX through the stored
# pointer lands in the right instruction set. The evidence column is what in
# this repo says so. Regenerate after a build with:
#     python3 tools/armrec/gen_decomp_thumb.py -o tools/armrec/decomp_thumb.txt
# name             state words  evidence
"""


def render(sites, callers, history, objects):
    """The table text, or (None, complaints) if something is unsettled."""
    lines, bad = [HEADER], []
    for name in sorted(sites):
        got = [("the tree's own call site", callers.get(name)),
               ("git history", history.get(name)),
               ("nm", objects.get(name))]
        got = [(src, v) for src, v in got if v]
        states = set(v[0] for _src, v in got)
        if not got:
            bad.append("%s: nothing in this repo says what it is, no "
                       "assembly call site, no func_start anywhere in git "
                       "history, and no symbol of that name in the build's "
                       "objects" % name)
            continue
        if len(states) != 1:
            bad.append("%s: %s" % (name, "; ".join(
                "%s says %s (%s)" % (src, v[0], v[1]) for src, v in got)))
            continue
        state, = states
        lines.append("%-18s %-5s %-6d %s"
                     % (name, state, sites[name],
                        "; ".join(v[1] for _src, v in got)))
    return ("\n".join(lines) + "\n") if not bad else None, bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", help="write here instead of stdout")
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--object", action="append",
                    help="a built game object for the `nm` source; the "
                         "default is build/pc/obj/game")
    ap.add_argument("--nm", default=os.environ.get("NM", "nm"))
    args = ap.parse_args()

    sites, _where = needed(args.root)
    names = set(sites)
    callers = from_callers(args.root, names)
    history, err = from_history(args.root, names)
    if history is None:
        sys.stderr.write("gen_decomp_thumb: %s\n" % err)
        return 1
    rest = names - set(callers) - set(history)
    objects = from_objects(args.root, rest, args.object, args.nm) if rest else {}

    text, bad = render(sites, callers, history, objects)
    if text is None:
        sys.stderr.write(
            "gen_decomp_thumb: %d name%s unsettled:\n  %s\n"
            % (len(bad), "" if len(bad) == 1 else "s", "\n  ".join(bad)))
        if not objects and rest:
            sys.stderr.write(
                "  (no built objects found, `make -f pc/Makefile status` "
                "first, so `nm` can say which of these are data)\n")
        return 1
    if args.out:
        with open(args.out, "w") as fh:
            fh.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
