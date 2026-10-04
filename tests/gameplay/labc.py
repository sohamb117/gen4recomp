#!/usr/bin/env python3
"""Compile a gameplay lab recipe (names) to the inline PC_LAB form (numbers).

    labc.py [--game platinum|diamond|pearl] RECIPE   ->   inline:name GQ;party 390 30 0;...

Platinum: the recipe language and its name resolution are the save lab's own
(games/platinum/pc/tests/pc_lab.py, pc/src/pc_lab.c); this only points that
resolver at the wasm build's generated headers and joins the lines with ';',
which is how a script reaches the wasm guest (pc/src/pc_text_open.h).

Diamond/Pearl (games/diamond/pc/game/pc_dp_lab.c reads the result): names
come from pokediamond's own headers, every numeric #define and enumerator in
include/constants/*.h plus include/poketch.h's PoketchApp, and the field's
four facings (FACE_UP/DOWN/LEFT/RIGHT, global_fieldmap.h's DIR_* numbering).
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GAMES = os.path.join(HERE, "..", "..", "games")

TEXT_VERBS = {"name"}
DEFINE_LINE = re.compile(r"^\s*#define\s+([A-Z_][A-Z0-9_]*)\s+\(?(-?\d+|0x[0-9a-fA-F]+)\)?\s*(?://.*)?$")
ENUM_NAME = re.compile(r"^\s*([A-Z_][A-Z0-9_]*)\s*(?:=\s*([A-Z_0-9x]+))?\s*,?\s*(?://.*)?$")


def dp_constants():
    root = os.path.join(GAMES, "diamond", "include")
    out = {"FACE_UP": 0, "FACE_DOWN": 1, "FACE_LEFT": 2, "FACE_RIGHT": 3}
    cdir = os.path.join(root, "constants")
    paths = [os.path.join(cdir, n) for n in sorted(os.listdir(cdir)) if n.endswith(".h")]
    paths.append(os.path.join(root, "poketch.h"))
    for path in paths:
        depth, nxt = 0, 0
        with open(path, errors="replace") as f:
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
                        v = m.group(2)
                        nxt = out[v] if v in out else int(v, 0)
                    out.setdefault(m.group(1), nxt)
                    nxt += 1
    return out


def make_resolver(game):
    if game == "platinum":
        sys.path.insert(0, os.path.join(GAMES, "platinum", "pc", "tests"))
        import pc_lab  # noqa: E402
        pc_lab.GENINCLUDE = os.path.join(GAMES, "platinum", "build", "pc-wasm", "geninclude", "generated")
        return pc_lab.resolve
    consts = dp_constants()

    def resolve(token, where):
        try:
            return int(token, 0)
        except ValueError:
            pass
        if token not in consts:
            raise SystemExit("labc: %s: unknown name %s" % (where, token))
        return consts[token]
    return resolve


def compile_inline(path, game="platinum"):
    resolve = make_resolver(game)
    out = []
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            verb, *args = line.split()
            where = "%s:%d" % (path, lineno)
            if verb in TEXT_VERBS:
                out.append(" ".join([verb] + args))
            else:
                out.append(" ".join([verb] + [str(resolve(a, where)) for a in args]))
    return "inline:" + ";".join(out)


if __name__ == "__main__":
    args = sys.argv[1:]
    game = "platinum"
    if len(args) == 3 and args[0] == "--game":
        game, args = args[1], args[2:]
    if len(args) != 1 or game not in ("platinum", "diamond", "pearl"):
        sys.exit("usage: labc.py [--game platinum|diamond|pearl] RECIPE")
    print(compile_inline(args[0], game))
