#!/usr/bin/env python3
"""Cook Diamond/Pearl runtime content packages (pc/mods/README.md).

A package's authored sources live under <pkg>/content/ (and frozen ids, if
any, under <pkg>/records/); this writes what the port loads into
<pkg>/.cooked/ and the input digest the port checks at boot
(games/platinum/pc/src/pc_modfs.c: a .cooked/ whose digest does not match
content/ + records/ is a boot error, never served stale).

Recipes (content/ path -> cooked file):

  content/<narc path>/<index>.gmm  ->  .cooked/narc/<narc path>/<index>
      A message bank (pret's GMM XML) replacing member <index> of that NARC,
      encoded by D's own tools/msgenc with D's charmap.txt; e.g.
      content/msgdata/msg.narc/341.gmm is msg.narc member 341. The bank's
      key comes from msgenc (from the output name); the game reads the key
      from the bank's header, so any key decodes.

Anything else under content/ is an error. Raw files need no cook: put them
in <pkg>/replace/<nitro path> (whole files) or <pkg>/narc/<narc path>/<index>
(members) and the port loads them as they are.

Run from games/diamond:

  python3 pc/modcook.py --mods example_rowan_text

The cook needs only this repository: msgenc is built from tools/msgenc with
the host C++ compiler into build/pc-wasm/tools/ on first use.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent  # games/diamond

FNV64_OFFSET = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3

MSGENC_SRCS = ["msgenc.cpp", "Options.cpp", "MessagesConverter.cpp",
               "MessagesDecoder.cpp", "MessagesEncoder.cpp", "Gmm.cpp",
               "pugixml.cpp"]


def fnv1a(h, data):
    for b in data:
        h ^= b
        h = (h * FNV64_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def digest_line(pkg):
    """Same digest as pc_modfs.c's hash_cook_inputs (and Platinum's
    pc/modcook.py): FNV-1a-64 over every file of content/ and records/,
    sorted by path relative to the package, each as path NUL bytes NUL."""
    files = []
    for top in ("content", "records"):
        base = pkg / top
        if not base.is_dir():
            continue
        for dirpath, _dirs, names in os.walk(base):
            for name in names:
                p = Path(dirpath) / name
                files.append((p.relative_to(pkg).as_posix(), p))
    files.sort(key=lambda t: t[0].encode("utf-8"))
    h = FNV64_OFFSET
    for rel, path in files:
        h = fnv1a(h, rel.encode("utf-8"))
        h = fnv1a(h, b"\0")
        h = fnv1a(h, path.read_bytes())
        h = fnv1a(h, b"\0")
    return "v1 %016x" % h


def find_msgenc(explicit):
    if explicit:
        return Path(explicit)
    src = ROOT / "tools" / "msgenc"
    out = ROOT / "build" / "pc-wasm" / "tools" / "msgenc"
    srcs = [src / s for s in MSGENC_SRCS]
    newest = max(p.stat().st_mtime for p in srcs + list(src.glob("*.h")))
    if not out.exists() or out.stat().st_mtime < newest:
        out.parent.mkdir(parents=True, exist_ok=True)
        cxx = os.environ.get("CXX", "c++")
        print("modcook: building msgenc with %s" % cxx, file=sys.stderr)
        subprocess.run([cxx, "-std=c++17", "-O2", "-DNDEBUG", "-w", "-o", str(out)]
                       + [str(s) for s in srcs], check=True)
    return out


def cook_pkg(pkg, msgenc, charmap):
    if not (pkg / "mod.toml").is_file():
        sys.exit("modcook: %s has no mod.toml" % pkg)
    cooked = pkg / ".cooked"
    if cooked.exists():
        shutil.rmtree(cooked)
    content = pkg / "content"
    n = 0
    with tempfile.TemporaryDirectory() as tmp:
        for dirpath, _dirs, names in sorted(os.walk(content)):
            for name in sorted(names):
                src = Path(dirpath) / name
                rel = src.relative_to(content)
                stem, ext = os.path.splitext(rel.name)
                if ext != ".gmm" or not stem.isdigit() or len(rel.parts) < 2:
                    sys.exit("modcook: no recipe for content/%s (want "
                             "<narc path>/<index>.gmm)" % rel.as_posix())
                dst = cooked / "narc" / rel.parent / str(int(stem))
                dst.parent.mkdir(parents=True, exist_ok=True)
                subprocess.run([str(msgenc), "-e", "--gmm", "-c", str(charmap),
                                "-H", os.path.join(tmp, "bank.h"),
                                str(src), str(dst)], check=True)
                n += 1
    cooked.mkdir(exist_ok=True)
    (cooked / "digest").write_text(digest_line(pkg) + "\n")
    print("modcook: %s: %d cooked, %s" % (pkg.name, n, digest_line(pkg)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--mods-dir", default=str(ROOT / "pc" / "mods"))
    ap.add_argument("--mods", required=True,
                    help="package directory names, space or comma separated")
    ap.add_argument("--msgenc", help="msgenc binary (default: build it)")
    ap.add_argument("--charmap", default=str(ROOT / "charmap.txt"))
    args = ap.parse_args()

    names = [s for s in args.mods.replace(",", " ").split() if s]
    msgenc = find_msgenc(args.msgenc)
    for name in names:
        cook_pkg(Path(args.mods_dir) / name, msgenc, Path(args.charmap))


if __name__ == "__main__":
    main()
