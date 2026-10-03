#!/usr/bin/env python3
"""Mint a save from a recipe, by name.

A station in the corpus is a RECIPE, never a committed .sav: a script that
regenerates its save deterministically, so nothing binary is ever checked in
and a save can never drift from the code that made it.

    $ python3 pc/tests/pc_lab.py pc/tests/corpus/jubilife.recipe
    $ python3 pc/tests/pc_lab.py --out build/pc/lab/x.sav --print x.recipe

The recipe is the same verb language the port's --lab reads (pc/src/pc_lab.c
holds the table), except that every argument may be written as the name the
game uses, SPECIES_TURTWIG, MAP_HEADER_JUBILIFE_CITY, FLAG_HIDE_JUBILIFE_
ROWAN. Those names are resolved HERE, out of the metang headers the build just
generated, so the port carries no name table and the recipe stays readable.
Anything that already looks like a number is passed straight through.
"""

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PORT = os.environ.get("PC_BIN") or os.path.join(ROOT, "build", "pc", "pokeplatinum")
GENINCLUDE = os.path.join(ROOT, "build", "pc", "geninclude", "generated")
LABDIR = os.path.join(ROOT, "build", "pc", "lab")

ENUM_LINE = re.compile(r"^\s*([A-Z_][A-Z0-9_]*)\s*=\s*(-?\d+|0x[0-9a-fA-F]+)\s*,?\s*$")
DEFINE_LINE = re.compile(r"^\s*#define\s+([A-Z_][A-Z0-9_]*)\s+(-?\d+|0x[0-9a-fA-F]+)\s*$")
ENUM_NAME = re.compile(
    r"^\s*([A-Z_][A-Z0-9_]*)\s*(?:=\s*(-?\d+|0x[0-9a-fA-F]+))?\s*,?\s*(?://.*)?$")

_CONSTS = None


def constants():
    """Every generated enumerator, plus the hand-written numeric defines.

    metang writes each generated header with its value spelled out, which
    makes the build's own numbering the only one this can read; there is no
    second copy here to fall out of step with the game.
    """
    global _CONSTS
    if _CONSTS is not None:
        return _CONSTS

    out = {}
    if not os.path.isdir(GENINCLUDE):
        raise SystemExit("pc_lab: %s missing, run `make -f pc/Makefile headers`"
                         % GENINCLUDE)
    for name in sorted(os.listdir(GENINCLUDE)):
        if not name.endswith(".h"):
            continue
        with open(os.path.join(GENINCLUDE, name)) as f:
            for line in f:
                m = ENUM_LINE.match(line)
                if m:
                    out.setdefault(m.group(1), int(m.group(2), 0))

    # A handful of the names a recipe wants are plain enums in hand-written
    # headers rather than metang output, facing directions, save-table ids.
    for path in (os.path.join(ROOT, "include", "location.h"),):
        with open(path) as f:
            n = 0
            for line in f:
                m = re.match(r"\s*([A-Z_][A-Z0-9_]*)\s*(?:=\s*(-?\d+))?\s*,?\s*$", line)
                if not m or not m.group(1).startswith("FACE_"):
                    continue
                if m.group(2):
                    n = int(m.group(2), 0)
                out.setdefault(m.group(1), n)
                n += 1

    headers = []
    for sub in ("", "savedata"):
        d = os.path.join(ROOT, "include", "constants", sub)
        if not os.path.isdir(d):
            continue
        headers += [os.path.join(d, name) for name in sorted(os.listdir(d))
                    if name.endswith(".h")]
    # A handful of enums a spec wants live in an ordinary header rather than
    # under constants/, POFFIN_TYPE_FOUL is one, and writing 27 instead
    # is exactly the kind of bare number the plan's tags exist to prevent.
    headers += [os.path.join(ROOT, "include", "poffin_types.h")]
    for path in headers:
        with open(path) as f:
            # An enumerator with no `= value` still has a value, and in these
            # hand-written headers most of them are written that way, only
            # the first member of enum OptionsTextSpeed carries a number, so a
            # reader that wanted the other two had to count. Counting inside
            # the braces is what the compiler does and it is what makes
            # OPTIONS_TEXT_SPEED_FAST usable in a spec.
            depth = 0
            nxt = 0
            for line in f:
                m = DEFINE_LINE.match(line)
                if m:
                    out.setdefault(m.group(1), int(m.group(2), 0))
                    continue
                if depth == 0:
                    if re.match(r"\s*(typedef\s+)?enum\b", line):
                        depth, nxt = 1, 0
                    continue
                if "}" in line:
                    depth = 0
                    continue
                m = ENUM_NAME.match(line)
                if m:
                    if m.group(2):
                        nxt = int(m.group(2), 0)
                    out.setdefault(m.group(1), nxt)
                    nxt += 1
    _CONSTS = out
    return out


def resolve(token, where):
    try:
        return int(token, 0)
    except ValueError:
        pass
    value = constants().get(token)
    if value is None:
        raise SystemExit("pc_lab: %s: unknown name %s" % (where, token))
    return value


TEXT_VERBS = {"name"}


def compile_recipe(path, out_path):
    """Recipe (names) -> .lab (numbers). Returns the compiled path."""
    lines = []
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            verb, args = parts[0], parts[1:]
            where = "%s:%d" % (path, lineno)
            if verb in TEXT_VERBS:
                lines.append("%s %s" % (verb, " ".join(args)))
            else:
                lines.append("%s %s" % (verb, " ".join(
                    str(resolve(a, where)) for a in args)))

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w") as f:
        f.write("# generated from %s, edit the recipe, not this\n"
                % os.path.relpath(path, ROOT))
        f.write("\n".join(lines) + "\n")
    return out_path


def mint(recipe, out_sav, at=None, rom=None, env=None, quiet=True):
    """Run the port's save lab over a recipe. Returns the .sav path."""
    os.makedirs(os.path.dirname(out_sav) or ".", exist_ok=True)
    stem = os.path.splitext(os.path.basename(recipe))[0]
    lab = compile_recipe(recipe, os.path.join(LABDIR, stem + ".lab"))

    # The lab clears whatever it finds, but a stale file left behind would
    # still be the thing a failed run leaves for the next reader to believe.
    if os.path.exists(out_sav):
        os.unlink(out_sav)

    run_env = dict(os.environ)
    run_env.update(env or {})
    run_env["PC_LAB"] = lab
    run_env["PC_SAVE"] = out_sav
    run_env["PC_LAB_AT"] = str(at if at is not None else 1800)
    if rom is not None:
        run_env["PC_ROM"] = rom
    # A lab run boots into the middle of the bedroom TV scene, with a text box
    # waiting for A. Nothing the lab does is safe while a printer is open, so
    # the run drives the same kind of input script every other run here does.
    run_env.setdefault("PC_INPUT", os.path.join(ROOT, "pc", "replays", "lab-settle.txt"))
    run_env.pop("PC_FRAMES", None)

    proc = subprocess.run([PORT], env=run_env, capture_output=True, text=True)
    if proc.returncode != 0 or not os.path.exists(out_sav):
        sys.stderr.write(proc.stderr)
        raise SystemExit("pc_lab: minting %s failed (exit %d)" % (recipe, proc.returncode))
    if not quiet:
        sys.stderr.write(proc.stderr)
    return out_sav


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("recipe")
    ap.add_argument("--out", help="save path (default build/pc/lab/<recipe>.sav)")
    ap.add_argument("--at", type=int, help="frame the recipe is applied at")
    ap.add_argument("--print", dest="show", action="store_true",
                    help="read the minted save back and print it")
    args = ap.parse_args()

    stem = os.path.splitext(os.path.basename(args.recipe))[0]
    out = args.out or os.path.join(LABDIR, stem + ".sav")
    mint(args.recipe, out, at=args.at, quiet=False)
    print(out)

    if args.show:
        import pc_save
        save = pc_save.Save(out)
        s = save.summary()
        print("%s: %s" % (out, s["result"]))
        print("  %s $%d %d badge(s) map %d (%d,%d)" % (
            s["name"], s["money"], s["badges"],
            s["position"]["map"], s["position"]["x"], s["position"]["z"]))
        for i, mon in enumerate(s["party"]):
            print("  party %d: species %d level %d" % (i, mon["species"], mon["level"]))
    return 0


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    sys.exit(main())
