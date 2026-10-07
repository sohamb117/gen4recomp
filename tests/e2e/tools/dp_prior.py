#!/usr/bin/env python3
"""Write the cumulative story state into every Diamond/Pearl story lab.recipe.

    python3 tests/e2e/tools/dp_prior.py [--check]

A D/P story milestone N's lab.recipe is: header, lab party, the state milestones 01..N-1 set, the start
(AUTHORING.md, Recipes). The state is what each earlier milestone's [expect] says it leaves behind: its
flags, cleared flags, vars, badges, the Pokedex, Poketch apps and bag items its `save` checks name. This tool
derives that block from diamond/chain.txt and the milestone.tomls and writes it between

    # -- prior: tests/e2e/tools/dp_prior.py --
    ...
    # -- start --

in each recipe (a `# @@PRIOR@@` line is replaced the first time), so changing what milestone K sets is one
edit to K's [expect] plus a rerun. Every line cites the script that sets it in play (`dp_script.py grep-flag`
/ `grep-var`: the SetFlag/ClearFlag/SetVar site named in K's refs if any, else the first one); a line no
script sets is marked [INFERENCE]. Items that a later milestone checks are gone (`not any(... == N ...)`)
are left out of the recipes after it. `--check` exits 1 when a recipe is stale.
"""
import argparse
import contextlib
import io
import os
import re
import sys
import tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
E2E = os.path.dirname(HERE)
GAME_DIR = os.path.join(E2E, "diamond")
BEGIN = "# -- prior: tests/e2e/tools/dp_prior.py --"
START = "# -- start --"

sys.path.insert(0, HERE)
import dp_script  # noqa: E402

# State a milestone sets in play that a recipe must not: the Journal's system flag (set by ScrCmd Unk01CC,
# games/diamond/arm9/src/scrcmd.c:3534-3543, which also attaches the journal data no lab verb writes). With the flag
# and no data, CONTINUE stops on a blank Journal page and never reaches the field (seen minting 13).
NOT_MINTED = {0x963}

_grep_cache = {}


def grep(kind, value):
    """dp_script grep-flag/grep-var output lines for a flag or var."""
    key = (kind, value)
    if key not in _grep_cache:
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            dp_script.main([kind, hex(value)])
        _grep_cache[key] = buf.getvalue().splitlines()[1:]
    return _grep_cache[key]


def cite(lines, pattern, refs):
    """The `scr_seq NNNN @0xOFF` of the first line matching pattern, preferring one the milestone's refs name."""
    sites = []
    for line in lines:
        m = re.match(r"\s*(scr_seq \d{4} @0x[0-9A-F]{4}) (.*)", line)
        if m and re.search(pattern, m.group(2)):
            sites.append((m.group(1), m.group(2).split("   [")[-1].rstrip("]").join("[]")))
    if not sites:
        return None
    joined = " ".join(refs)
    site = next((s for s in sites if s[0] in joined), sites[0])
    return "%s %s" % site


def system(lines, how):
    """A flag no script sets or clears directly: an object's hidden flag (RemoveEvent/AddEvent) or a system flag."""
    for line in lines:
        m = re.match(r"\s*(zone_event \d{4} object \d+) \(id \d+, (\w+)\) hidden_flag \[(\w+)\]", line)
        if m:
            return "%s (%s) hidden flag, %s by its Remove/AddEvent [%s]" % (m.group(1), m.group(2), how, m.group(3))
    return None

def item_checks(expr):
    """(item, qty, present) for a bag check in a `save` expression."""
    m = re.search(r'i\["item"\] == (\d+)(?: and i\["qty"\] >= (\d+))?', expr)
    if m:
        return [(int(m.group(1)), int(m.group(2) or 1), not expr.lstrip().startswith("not "))]
    m = re.search(r'i\["item"\] in \(([\d, ]+)\)', expr)
    if m and expr.lstrip().startswith("not "):
        return [(int(x), 0, False) for x in m.group(1).split(",") if x.strip()]
    return []


def group(name, ms, state):
    """The recipe lines milestone `name` contributes, given the state so far (updated in place)."""
    ex = ms.get("expect", {})
    refs = [str(r) for r in ms.get("refs", [])]
    out = []
    for f in ex.get("flags", []):
        v = int(f, 16)
        if state["flags"].get(v) is True or v in NOT_MINTED:
            continue
        state["flags"][v] = True
        lines = grep("grep-flag", v)
        c = cite(lines, r"^(SetFlag %d|SetTrainerFlag \d+)\b" % v, refs) or system(lines, "set")
        out.append(("flag 0x%X" % v, c or "[INFERENCE] set in play by %s (no script SetFlag; see its refs)" % name))
    for f in ex.get("flags_clear", []):
        v = int(f, 16)
        if state["flags"].get(v) is False:
            continue
        state["flags"][v] = False
        lines = grep("grep-flag", v)
        c = cite(lines, r"^ClearFlag %d\b" % v, refs) or system(lines, "cleared")
        out.append(("clear-flag 0x%X" % v, c or "[INFERENCE] cleared in play by %s (see its refs)" % name))
    for var, want in ex.get("vars", {}).items():
        v = int(var, 16)
        val = want if isinstance(want, int) else want
        if state["vars"].get(v) == val:
            continue
        state["vars"][v] = val
        num = val if isinstance(val, int) else None
        pat = r"^SetVar 0x%04X, %s\b" % (v, num if num is not None else r"\S+")
        c = cite(grep("grep-var", v), pat, refs)
        out.append(("var 0x%X %s" % (v, val), c or "[INFERENCE] set in play by %s (see its refs)" % name))
    for b in ex.get("badge", []):
        if b in state["badges"]:
            continue
        state["badges"].add(b)
        c = next((r for r in refs if "GiveBadge" in r or "badge" in r.lower()), None)
        out.append(("badge %s" % b, c or "%s's leader script (see its refs)" % name))
    for expr in ex.get("save", []):
        if 'pokedex"]["obtained"]' in expr and not state["pokedex"]:
            state["pokedex"] = True
            out.append(("pokedex 1", "%s: GiveSinnohDex (see its refs)" % name))
        m = re.search(r"\{([\d, ]+)\} <= set\(s\[\"poketch\"\]\[\"apps\"\]\)", expr)
        apps = [int(x) for x in m.group(1).split(",")] if m else []
        m = re.match(r"\s*(\d+) in s\[\"poketch\"\]\[\"apps\"\]", expr)
        apps += [int(m.group(1))] if m else []
        for a in apps:
            if a not in state["apps"]:
                state["apps"].add(a)
                out.append(("poketch %d" % a, "%s: the Poketch app it checks (see its refs)" % name))
        for item, qty, present in item_checks(expr):
            if present and item not in state["items"]:
                state["items"].add(item)
                out.append(("item %s %d" % (dp_script.item_name(item), qty),
                            "%s: given in play (see its refs)" % name, item))
            elif not present:
                state["items"].discard(item)
                state["gone"].add(item)
    return out


def blocks(chain, mss):
    """{milestone: prior block text} for every chain entry."""
    state = {"flags": {}, "vars": {}, "badges": set(), "items": set(), "gone": set(), "apps": set(),
             "pokedex": False}
    groups = []
    result = {}
    for name in chain:
        prior = []
        for gname, title, lines in groups:
            prior.append("# -- %s %s --" % (gname[:3].rstrip("-"), title))
            prior += ["%-28s # %s" % (ln[0], ln[1]) for ln in lines]
        result[name] = "\n".join([BEGIN] + prior)
        lines = group(name, mss[name], state)
        # items this milestone checks gone vanish from every earlier group in the recipes after it
        for item in state["gone"]:
            for _, _, glines in groups:
                glines[:] = [ln for ln in glines if not (len(ln) == 3 and ln[2] == item)]
        state["gone"].clear()
        groups.append((name, mss[name].get("title", ""), lines))
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="exit 1 if a recipe is stale; write nothing")
    args = ap.parse_args()
    chain = [ln.strip() for ln in open(os.path.join(GAME_DIR, "chain.txt"))
             if ln.strip() and not ln.startswith("#")]
    mss = {n: tomllib.load(open(os.path.join(GAME_DIR, n, "milestone.toml"), "rb")) for n in chain}
    stale = []
    for name, block in blocks(chain, mss).items():
        lab = mss[name].get("start", {}).get("lab")
        if not lab:
            continue
        path = os.path.join(GAME_DIR, name, lab)
        text = open(path).read()
        if "# @@PRIOR@@" in text:
            new = text.replace("# @@PRIOR@@", block)
        elif BEGIN in text:
            head, rest = text.split(BEGIN, 1)
            new = head + block + "\n" + START + rest.split(START, 1)[1]
        else:
            sys.exit("%s: no '# @@PRIOR@@' or '%s' line" % (path, BEGIN))
        if new != text:
            stale.append(path)
            if not args.check:
                with open(path, "w") as f:
                    f.write(new)
    for p in stale:
        print(("stale " if args.check else "wrote ") + os.path.relpath(p, os.path.dirname(os.path.dirname(E2E))))
    sys.exit(1 if args.check and stale else 0)


if __name__ == "__main__":
    main()
