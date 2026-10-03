#!/usr/bin/env python3
"""Generate the headers the decompiled C includes but the tree does not carry.

Three sources, in decreasing order of self-sufficiency:

1. generated/*.h, metang (a pure-Python meson subproject) over
   generated/*.txt, with the type/tag/extra arguments parsed out of
   generated/meson.build so this script cannot drift from the ROM build.
2. nitro/fx/fx_const.h, the NitroSDK wrap's gen_fx_const.py over its CSV.
3. res/**/*.naix and res/**/*.h; the asset pipeline. These need the full
   meson ROM configure (metroskrew + knarc + nitrogfx); this script only
   *copies* them out of an existing ROM build dir (build/rom by default).
   If that dir is absent the copies are skipped with a warning and the ~235
   files that include them will land in the .skipped count, honest, not
   fatal. To produce them:
       meson setup -Drevision=1 --wrap-mode=nopromote \
           --native-file=meson/native.ini --cross-file=meson/cross_unix.ini build/rom
       ninja -C build/rom <every *.naix / res *.h target>

Everything lands under the single include root passed as argv[1]
(build/pc/geninclude), which pc/Makefile puts on the include path. Nothing is
written into the source tree.
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def gen_metang(outroot: Path) -> int:
    src = (ROOT / "generated" / "meson.build").read_text()
    body = re.search(r"metang_generators = \{(.*?)\n\}", src, re.S).group(1)
    entries = re.findall(
        r"'([\w]+)':\s*\{\s*'type':\s*'(\w+)',\s*'tag':\s*'(\w+)'(.*?)\}", body
    )
    outdir = outroot / "generated"
    outdir.mkdir(parents=True, exist_ok=True)
    fails = 0
    for key, typ, tag, rest in entries:
        extra = ["--no-auto"] if "--no-auto" in rest else []
        txt = ROOT / "generated" / f"{key}.txt"
        out = outdir / f"{key}.h"
        if out.exists() and out.stat().st_mtime >= txt.stat().st_mtime:
            continue
        r = subprocess.run(
            [sys.executable, str(ROOT / "subprojects/metang/metang.py"), typ,
             "--tag-name", tag, "--guard", "POKEPLATINUM_GENERATED",
             "--output", str(out)] + extra + [str(txt)],
            capture_output=True, text=True)
        if r.returncode:
            fails += 1
            print(f"metang FAILED on {key}: {r.stderr.strip()[:200]}", file=sys.stderr)
    return fails


def gen_fx_const(outroot: Path) -> int:
    gen = ROOT / "subprojects/NitroSDK-4.2.30001/gen/nitro/fx"
    if not gen.is_dir():
        print("NitroSDK wrap not downloaded (run `meson subprojects download`)",
              file=sys.stderr)
        return 1
    out = outroot / "nitro/fx/fx_const.h"
    out.parent.mkdir(parents=True, exist_ok=True)
    r = subprocess.run([sys.executable, str(gen / "gen_fx_const.py"),
                        str(gen / "fx_const.csv"), str(out)],
                       capture_output=True, text=True)
    if r.returncode:
        print(f"gen_fx_const FAILED: {r.stderr.strip()[:200]}", file=sys.stderr)
        return 1
    return 0


def copy_res_headers(outroot: Path, rombuild: Path) -> bool:
    src = rombuild / "res"
    if not src.is_dir():
        print(f"warning: {src} not found, .naix/asset headers not copied; "
              "files including them will be SKIPped",
              file=sys.stderr)
        return False
    n = 0
    for p in src.rglob("*"):
        if p.suffix not in (".naix", ".h"):
            continue
        rel = p.relative_to(rombuild)
        # Most sources include these as "res/..."; a handful (title_screen.c,
        # end_credits/strings.c, healthbox.c, frontier_scenes.c) drop the
        # res/ prefix, the ROM build resolves both, so mirror each file at
        # both paths. Verified collision-free against include/ and pc/include.
        for dst in (outroot / rel, outroot / p.relative_to(src)):
            if dst.exists() and dst.stat().st_mtime >= p.stat().st_mtime:
                continue
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, dst)
            n += 1
    if n:
        print(f"copied {n} asset headers from {src}")
    return True


def main() -> int:
    outroot = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/pc/geninclude")
    rombuild = Path(sys.argv[2] if len(sys.argv) > 2 else ROOT / "build/rom")
    rc = gen_metang(outroot)
    rc += gen_fx_const(outroot)
    copy_res_headers(outroot, rombuild)
    return 1 if rc else 0


if __name__ == "__main__":
    sys.exit(main())
