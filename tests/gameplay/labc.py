#!/usr/bin/env python3
"""Compile a gameplay lab recipe (names) to the inline PC_LAB form (numbers).

    labc.py tests/gameplay/recipes/roark.recipe   ->   inline:name GQ;party 390 30 0;...

The recipe language and its name resolution are the save lab's own
(games/platinum/pc/tests/pc_lab.py, pc/src/pc_lab.c); this only points that
resolver at the wasm build's generated headers and joins the lines with ';',
which is how a script reaches the wasm guest (pc/src/pc_text_open.h).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PLAT = os.path.join(HERE, "..", "..", "games", "platinum")
sys.path.insert(0, os.path.join(PLAT, "pc", "tests"))
import pc_lab  # noqa: E402

pc_lab.GENINCLUDE = os.path.join(PLAT, "build", "pc-wasm", "geninclude", "generated")


def compile_inline(path):
    out = []
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            verb, *args = line.split()
            where = "%s:%d" % (path, lineno)
            if verb in pc_lab.TEXT_VERBS:
                out.append(" ".join([verb] + args))
            else:
                out.append(" ".join([verb] + [str(pc_lab.resolve(a, where)) for a in args]))
    return "inline:" + ";".join(out)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: labc.py RECIPE")
    print(compile_inline(sys.argv[1]))
