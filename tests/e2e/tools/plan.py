#!/usr/bin/env python3
"""plan.py - regenerate the per-game milestone tables in tests/e2e/PLAN.md from the milestone dirs.

    python3 tests/e2e/tools/plan.py          rewrite PLAN.md between its `<!-- plan.py:begin GAME -->` /
                                             `<!-- plan.py:end GAME -->` markers (story chain + side systems)
    python3 tests/e2e/tools/plan.py --check  exit 1 if PLAN.md is stale

Everything in a generated section comes from the milestone itself: the header comment (what it proves,
start -> end), title/priority/version/estimate/status, refs (the decomp citations), the lab or start
recipe (party, cumulative story state, start warp), and [expect]. Edit the milestone, then rerun.
"""

import sys
import tomllib
from pathlib import Path

E2E = Path(__file__).resolve().parents[1]
PLAN = E2E / "PLAN.md"
GAMES = ("platinum", "diamond", "pearl", "heartgold", "soulsilver", "emerald", "ruby", "sapphire")
STATE_VERBS = ("flag", "clear-flag", "var", "badge", "item", "poketch", "pokedex", "national-dex",
               "story-cleared", "register-item")


def entries(game, listname):
    path = E2E / game / listname
    if not path.exists():
        return []
    out = []
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            out.append((E2E / game / line).resolve())
    return out


def header(text):
    lines = []
    for line in text.splitlines():
        if not line.startswith("#"):
            break
        lines.append(line.lstrip("# ").rstrip())
    return " ".join(lines)


def recipe_lines(path):
    if not path.exists():
        return []
    return [l.split("#", 1)[0].strip() for l in path.read_text().splitlines() if l.split("#", 1)[0].strip()]


def summarize_recipe(lines):
    party, start, state = [], [], {}
    for l in lines:
        verb = l.split()[0]
        if verb == "party":
            party.append(" ".join(l.split()[1:3]))
        elif verb == "party-move" and l.split()[2] == "0" and party:
            party[-1] += f" ({l.split()[3]})"
        elif verb in ("warp", "map"):
            start.append(l)
        elif verb in STATE_VERBS:
            state[verb] = state.get(verb, 0) + 1
    return party, start, state


def fmt_expect(ex):
    parts = []
    if "map" in ex:
        parts.append(f"map {ex['map']}")
    if "position" in ex:
        parts.append(f"at {tuple(ex['position'])}")
    if "badges" in ex:
        parts.append(f"{ex['badges']} badges")
    if ex.get("badge"):
        parts.append("badge " + ", ".join(ex["badge"]))
    if "battles" in ex:
        parts.append(f">= {ex['battles']} battles")
    if ex.get("party"):
        parts.append("party " + ", ".join(ex["party"]))
    if "party_size" in ex:
        parts.append(f"party size {ex['party_size']}")
    if ex.get("flags"):
        parts.append("flags set " + ", ".join(ex["flags"]))
    if ex.get("flags_clear"):
        parts.append("flags clear " + ", ".join(ex["flags_clear"]))
    vs = dict(ex.get("vars", {}))
    if vs:
        parts.append("vars " + ", ".join(f"{k}={v}" for k, v in vs.items()))
    if ex.get("save"):
        parts.append(f"{len(ex['save'])} save check(s)")
    if ex.get("log"):
        parts.append("log /" + "/, /".join(ex["log"]) + "/")
    return "; ".join(parts) or "-"


def trainers(refs):
    return [r for r in refs if "TRAINER_" in r]


def section(game, dirs, kind):
    rows, details = [], []
    total = 0
    for d in dirs:
        toml = d / "milestone.toml"
        text = toml.read_text()
        m = tomllib.loads(text)
        rel = d.relative_to(E2E)
        st = m.get("start", {})
        recipe = st.get("lab") or st.get("recipe")
        lines = recipe_lines(d / recipe) if recipe else []
        party, start, state = summarize_recipe(lines)
        ex = m.get("expect", {})
        est = m.get("estimate", 0)
        total += est
        version = m.get("version", "")
        src = "blank chip" if st.get("blank") else (
            f"prev + `{recipe}`" if st.get("from") else f"`{recipe}`")
        rows.append(f"| [{d.name}]({rel}/milestone.toml) | {m['title']} | {m['priority']}"
                     + (f" | {version}" if game != "platinum" else "")
                     + f" | {est} | {src} | {ex.get('map', '-')} | {m.get('status', 'passing')} |")
        st_txt = ", ".join(f"{n} {v}" for v, n in sorted(state.items())) or "none"
        tr = trainers(m.get("refs", []))
        details.append("\n".join([
            f"#### {game}/{d.name} — {m['title']}",
            f"- proves: {header(text) or '-'}",
            f"- start: {src}; {' / '.join(start) or '-'}; lab state lines: {st_txt}",
            f"- party: {'; '.join(party) or 'the continued save'}",
            f"- trainers: {'; '.join(tr) or 'none'}",
            f"- end state: {fmt_expect(ex)}",
            f"- frames: estimate {est}, budget {m.get('run', {}).get('frames', '-')}",
            f"- refs: {'; '.join(m.get('refs', [])) or '-'}",
            *( [f"- notes: {m['notes']}"] if m.get("notes") else [] ),
        ]))
    head = ("| milestone | title | P" + (" | version" if game != "platinum" else "")
            + " | est. frames | start | end map | status |\n|---|---|---" + ("|---" if game != "platinum" else "")
            + "|---|---|---|---|")
    table = "\n".join([head, *rows])
    return "\n\n".join([f"### {kind}: {len(dirs)} milestones, ~{total} frames estimated", table, *details])


def generate(game):
    chain = entries(game, "chain.txt")
    systems = entries(game, "systems.txt")
    parts = []
    if chain:
        parts.append(section(game, chain, "Story chain"))
    if systems:
        parts.append(section(game, systems, "Side systems"))
    return "\n\n".join(parts) if parts else "(no milestones yet)"


def render(text):
    for game in GAMES:
        b, e = f"<!-- plan.py:begin {game} -->", f"<!-- plan.py:end {game} -->"
        if b not in text:
            continue
        pre, rest = text.split(b, 1)
        _, post = rest.split(e, 1)
        text = f"{pre}{b}\n{generate(game)}\n{e}{post}"
    return text


def main():
    old = PLAN.read_text()
    new = render(old)
    if "--check" in sys.argv[1:]:
        if new != old:
            sys.exit("PLAN.md is stale: run python3 tests/e2e/tools/plan.py")
        return
    PLAN.write_text(new)


if __name__ == "__main__":
    main()
