#!/usr/bin/env python3
"""Stage compile-time plugin headers into $(BUILD)/modinclude.

Whole files under <mod>/include/ and <mod>/plugins/include/ are copied
in MODS order (later wins). Header patches under <mod>/patches/ and
<mod>/plugins/patches/ apply on top with patch --fuzz=0. Files this
script did not write (cook's generated tables) are left alone.

Vanilla build/pc/geninclude is never the dest.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


def die(msg: str) -> None:
    sys.stderr.write("modinclude: %s\n" % msg)
    sys.exit(1)


def dest_for_header_patch(dest_root: Path, rel: str) -> Path:
    if rel.startswith("include/"):
        return dest_root / rel[len("include/"):]
    if rel.startswith("pc/include/"):
        return dest_root / rel[len("pc/include/"):]
    return dest_root / rel


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mods-dir", required=True)
    ap.add_argument("--mods", default="")
    ap.add_argument("--dest", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--gen", required=True)
    args = ap.parse_args()

    dest = Path(args.dest)
    root = Path(args.root)
    gen = Path(args.gen)
    if dest.resolve() == (root / "build" / "pc" / "geninclude").resolve():
        die("refusing to write vanilla build/pc/geninclude")

    dest.mkdir(parents=True, exist_ok=True)
    staged_path = dest / ".staged-list"
    if staged_path.is_file():
        for line in staged_path.read_text().splitlines():
            p = Path(line)
            if p.is_file():
                p.unlink()

    staged: list[Path] = []
    mods_dir = Path(args.mods_dir)
    for name in args.mods.split():
        if not name:
            continue
        mod = mods_dir / name
        for srcroot in ("include", "plugins/include"):
            src = mod / srcroot
            if not src.is_dir():
                continue
            for f in sorted(p for p in src.rglob("*") if p.is_file()):
                out = dest / f.relative_to(src)
                out.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(f, out)
                staged.append(out)
        for sub in ("patches", "plugins/patches"):
            pdir = mod / sub
            if not pdir.is_dir():
                continue
            for mp in sorted(pdir.rglob("*.h.patch")):
                rel = str(mp.relative_to(pdir))[:-len(".patch")]
                out = dest_for_header_patch(dest, rel)
                vanilla = root / rel
                if not vanilla.is_file():
                    vanilla = gen / rel
                if not out.is_file():
                    if not vanilla.is_file():
                        die("mod '%s' header patch has no source: %s"
                            % (name, rel))
                    out.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(vanilla, out)
                r = subprocess.run(
                    ["patch", "--silent", "--forward", "--fuzz=0",
                     str(out), str(mp)],
                    capture_output=True, text=True,
                )
                if r.returncode != 0:
                    err = (r.stderr or r.stdout or "").strip()
                    die("mod '%s' header patch no longer applies: %s%s"
                        % (name, rel, ("\n" + err) if err else ""))
                staged.append(out)

    staged_path.write_text("".join(str(p) + "\n" for p in staged))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
