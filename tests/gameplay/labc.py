#!/usr/bin/env python3
"""Compile a gameplay lab recipe (names) to the inline PC_LAB form (numbers).

    labc.py [--game platinum|diamond|pearl|heartgold|soulsilver|emerald|ruby|sapphire] RECIPE           ->   inline:name GQ;party 390 30 0;...
    labc.py [--game platinum|diamond|pearl|heartgold|soulsilver|emerald|ruby|sapphire] --clock RECIPE   ->   the recipe's PC_RTC (empty: none)

Platinum: the recipe language and its name resolution are the save lab's own
(games/platinum/pc/tests/pc_lab.py, pc/src/pc_lab.c); this only points that
resolver at the wasm build's generated headers and joins the lines with ';',
which is how a script reaches the wasm guest (pc/src/pc_text_open.h).

Diamond/Pearl (games/diamond/pc/game/pc_dp_lab.c reads the result): names
come from pokediamond's own headers, every numeric #define and enumerator in
include/constants/*.h plus include/poketch.h's PoketchApp, the field's four
facings (FACE_UP/DOWN/LEFT/RIGHT, global_fieldmap.h's DIR_* numbering), and
tests/gameplay/dp/names.txt, the flag and var names pokediamond does not have.

Ruby/Sapphire/Emerald (tools/gba/gen3_lab.py applies the result to a save):
names come from the decomp's own include/constants/*.h, pokeemerald for
Emerald and pokeruby built as RUBY or SAPPHIRE: every numeric #define and
enumerator, expression defines evaluated, the MAP_* of map_groups.h and the
LOCALID_* of event_objects.h / map_event_ids.h among them
(tools/gba/gen3.py decomp_constants).

HeartGold/SoulSilver: names come from pokeheartgold's include/constants/*.h
(one tree for both versions): MAP_* (maps.h), FLAG_*, VAR_*, BADGE_*,
SPECIES_*, ITEM_*, MOVE_* and every other numeric #define and enumerator.

Every argument may also be a number (decimal or 0x hex) on every game.

One line is the compiler's rather than the guest's: `clock YYYY-MM-DD
HH:MM:SS` sets the game clock, which is the RTC, which the port takes from
PC_RTC at boot (games/platinum/pc/src/pc_rtc.c, shared by Diamond/Pearl).
compile_recipe() returns it as the environment the run needs; mint.sh
passes that on, and later runs from the save carry it themselves.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GAMES = os.path.join(HERE, "..", "..", "games")
DP_NAMES = os.path.join(HERE, "dp", "names.txt")
GBA_TOOLS = os.path.join(HERE, "..", "..", "tools", "gba")
GBA_GAMES = ("emerald", "ruby", "sapphire")
HGSS_GAMES = ("heartgold", "soulsilver")

TEXT_VERBS = {"name"}
DEFINE_LINE = re.compile(r"^\s*#define\s+([A-Z_][A-Z0-9_]*)\s+\(?(-?\d+|0x[0-9a-fA-F]+)\)?\s*(?://.*)?$")
ENUM_NAME = re.compile(r"^\s*([A-Z_][A-Z0-9_]*)\s*(?:=\s*([A-Z_0-9x]+))?\s*,?\s*(?://.*)?$")
CLOCK = re.compile(r"^(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})$")


def dp_constants(root=None, extra=("poketch.h",), names=DP_NAMES):
    """Every numeric #define and enumerator of a DS decomp's include/constants/*.h (and `extra` headers): pokediamond
    by default, plus FACE_* and tests/gameplay/dp/names.txt; HG/SS through hgss_constants."""
    root = root or os.path.join(GAMES, "diamond", "include")
    out = {"FACE_UP": 0, "FACE_DOWN": 1, "FACE_LEFT": 2, "FACE_RIGHT": 3}
    cdir = os.path.join(root, "constants")
    paths = [os.path.join(cdir, n) for n in sorted(os.listdir(cdir)) if n.endswith(".h")]
    paths += [os.path.join(root, n) for n in extra]
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
    if names:
        dp_names(out, names)
    return out


def hgss_constants():
    """HeartGold/SoulSilver: pokeheartgold's include/constants/*.h (MAP_* maps.h, FLAG_* flags.h, VAR_* vars.h,
    BADGE_* badge.h, SPECIES_*, ITEM_*, MOVE_*); one tree builds both versions."""
    return dp_constants(os.path.join(GAMES, "heartgold", "include"), extra=(), names=None)


def dp_names(out, path=DP_NAMES):
    """tests/gameplay/dp/names.txt: `NAME VALUE` lines, `#` comments."""
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].split()
            if not line:
                continue
            where = "%s:%d" % (path, lineno)
            if len(line) != 2 or not re.match(r"^[A-Z_][A-Z0-9_]*$", line[0]):
                raise SystemExit("labc: %s: want NAME VALUE" % where)
            try:
                value = int(line[1], 0)
            except ValueError:
                raise SystemExit("labc: %s: %s is not a number" % (where, line[1]))
            if out.get(line[0], value) != value:
                raise SystemExit("labc: %s: %s is already %d in pokediamond" % (where, line[0], out[line[0]]))
            out[line[0]] = value


def make_resolver(game):
    if game == "platinum":
        sys.path.insert(0, os.path.join(GAMES, "platinum", "pc", "tests"))
        import pc_lab  # noqa: E402
        pc_lab.GENINCLUDE = os.path.join(GAMES, "platinum", "build", "pc-wasm", "geninclude", "generated")
        return pc_lab.resolve
    if game in GBA_GAMES:
        sys.path.insert(0, GBA_TOOLS)
        import gen3  # noqa: E402
        consts = gen3.decomp_constants(game)
    elif game in HGSS_GAMES:
        consts = hgss_constants()
    else:
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


def parse_clock(args, where):
    """`clock YYYY-MM-DD HH:MM:SS` -> the PC_RTC string, checked the way
    pc_rtc_init checks it (2000-2099, a real date)."""
    text = " ".join(args)
    m = CLOCK.match(text)
    bad = m is None
    if not bad:
        y, mo, d, h, mi, s = (int(g) for g in m.groups())
        mdays = [31, 29 if y % 4 == 0 else 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
        bad = not (2000 <= y <= 2099 and 1 <= mo <= 12 and 1 <= d <= mdays[mo - 1]
                   and h <= 23 and mi <= 59 and s <= 59)
    if bad:
        raise SystemExit("labc: %s: clock wants YYYY-MM-DD HH:MM:SS (2000-2099), got %r" % (where, text))
    return text


def compile_recipe(path, game="platinum"):
    """Recipe -> (inline PC_LAB string, environment dict for the run)."""
    resolve = make_resolver(game)
    out, env = [], {}
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            verb, *args = line.split()
            where = "%s:%d" % (path, lineno)
            if verb == "clock":
                env["PC_RTC"] = parse_clock(args, where)
            elif verb in TEXT_VERBS:
                out.append(" ".join([verb] + args))
            else:
                out.append(" ".join([verb] + [str(resolve(a, where)) for a in args]))
    return "inline:" + ";".join(out), env


def compile_inline(path, game="platinum"):
    return compile_recipe(path, game)[0]


if __name__ == "__main__":
    args = sys.argv[1:]
    game, clock = "platinum", False
    while len(args) > 1 and args[0] in ("--game", "--clock"):
        if args[0] == "--clock":
            clock, args = True, args[1:]
        else:
            game, args = args[1], args[2:]
    if len(args) != 1 or game not in ("platinum", "diamond", "pearl") + HGSS_GAMES + GBA_GAMES:
        sys.exit("usage: labc.py [--game platinum|diamond|pearl|heartgold|soulsilver|emerald|ruby|sapphire] [--clock] RECIPE")
    inline, env = compile_recipe(args[0], game)
    print(env.get("PC_RTC", "") if clock else inline)
