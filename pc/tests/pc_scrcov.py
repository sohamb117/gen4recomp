#!/usr/bin/env python3
"""Field-script coverage: what fraction of the game's bytecode has ever run here.

    $ python3 pc/tests/pc_scrcov.py build/pc/cov/*.cov          # merged report
    $ python3 pc/tests/pc_scrcov.py --never build/pc/cov/*.cov  # what never ran
    $ python3 pc/tests/pc_scrcov.py --baseline build/pc/cov/*.cov   # ratchet check
    $ python3 pc/tests/pc_scrcov.py --write-baseline build/pc/cov/*.cov

The port writes a `.cov` file per run when PC_SCRIPT_COV names one: one line
per executed opcode with the number of times it was dispatched. This sums them
and turns the numbers back into names.

The names come from the tree, not from a table here. `include/data/scripts/
scrcmd.h` and `frscrcmd.h` ARE the dispatch tables, the game builds its
`ScrCmdFunc[]` from those same lines through `cmd_table.h`'s macro, so
reading them in order gives exactly the opcode numbering the port just used.
A table restated here would be one upstream commit away from mislabelling
every command after the insertion point, silently.

Why the BASELINE is names and not numbers. Same reason, pointing the other
way: an inserted command shifts every id after it, so a numeric baseline would
read as a hundred regressions the day pret adds one. `SCRCMD_*` constants are
stable across that, and a renamed command is a real change worth a line in a
diff.

Why the BASELINE is per source. The suite does not drive everything on every
run (`test_corpus` drives three stations unless PC_TEST_CORPUS=all) so a
single merged set would either have to be the small one (and never notice the
other nine regressing) or the big one (and fail every ordinary run). A row
names the run that produced it, taken from the `.cov` file's stem, and a check
holds each source to its own row. What a partial run cannot say, it does not.

A command with no name is not an error here: the table has slots whose handler
is the placeholder `ScrCmd_Unused_NNN`, and they are reported separately from
the real ones because "800 of 840" and "800 of 800" answer different questions.
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HEADERS = {
    "field": os.path.join(ROOT, "include", "data", "scripts", "scrcmd.h"),
    "frontier": os.path.join(ROOT, "include", "data", "scripts", "frscrcmd.h"),
}
BASELINE = os.path.join(ROOT, "pc", "tests", "script-coverage.txt")
COVDIR = os.path.join(ROOT, "build", "pc", "cov")

ROW = re.compile(r"^ScriptCommand\(\s*([A-Za-z0-9_]+)\s*,\s*([A-Za-z0-9_]+)\s*\)")
# The placeholder handler pret gives a slot no command uses. The CONSTANT is
# not the test: ten real, implemented commands carry an _UNUSED suffix on their
# name (SCRCMD_GETPARTYMONGENDER_UNUSED and friends) and they dispatch.
PLACEHOLDER = re.compile(r"^ScrCmd_Unused_[0-9A-Fa-f]+$")

_TABLES = {}


def table(engine):
    """[(constant, handler, is_placeholder)] in dispatch order."""
    if engine in _TABLES:
        return _TABLES[engine]
    out = []
    with open(HEADERS[engine]) as f:
        for line in f:
            m = ROW.match(line.strip())
            if m:
                out.append((m.group(1), m.group(2),
                            bool(PLACEHOLDER.match(m.group(2)))))
    if not out:
        sys.exit("pc_scrcov: %s carries no ScriptCommand rows" % HEADERS[engine])
    _TABLES[engine] = out
    return out


def source_of(path):
    """The run that produced a .cov file, which is its stem."""
    return os.path.basename(path).rsplit(".", 1)[0]


def read(paths):
    """{source: {engine: {opcode: count}}}, summing repeats within a source."""
    out = {}
    for path in paths:
        per = out.setdefault(source_of(path), {})
        with open(path) as f:
            for line in f:
                cols = line.split("#", 1)[0].split()
                if len(cols) != 3 or cols[0] not in HEADERS:
                    continue
                engine, op, count = cols
                e = per.setdefault(engine, {})
                e[int(op)] = e.get(int(op), 0) + int(count)
    return out


def merge(per_source):
    """One {engine: {opcode: count}} across every source."""
    total = {}
    for per in per_source.values():
        for engine, ops in per.items():
            e = total.setdefault(engine, {})
            for op, n in ops.items():
                e[op] = e.get(op, 0) + n
    return total


def names(counts):
    """{engine: set(constant)} for one {engine: {opcode: count}}."""
    out = {}
    for engine in HEADERS:
        rows = table(engine)
        out[engine] = {rows[op][0] for op, n in counts.get(engine, {}).items()
                       if n and 0 <= op < len(rows)}
    return out


def summarise(counts):
    """[(engine, run, real, slots, dispatches)] for the report and progress.sh."""
    out = []
    for engine in ("field", "frontier"):
        rows = table(engine)
        got = counts.get(engine, {})
        real = [i for i, r in enumerate(rows) if not r[2]]
        out.append((engine, sum(1 for i in real if got.get(i)), len(real),
                    len(rows), sum(got.values())))
    return out


def read_baseline(path=BASELINE):
    """{source: {engine: set(constant)}}."""
    have = {}
    if not os.path.exists(path):
        return have
    with open(path) as f:
        for line in f:
            cols = line.split("#", 1)[0].split()
            if len(cols) == 3:
                have.setdefault(cols[0], {}).setdefault(cols[1], set()).add(cols[2])
    return have


def baseline_names(have):
    """The union of a baseline's rows, as {engine: set(constant)}."""
    out = {}
    for per in have.values():
        for engine, cmds in per.items():
            out.setdefault(engine, set()).update(cmds)
    return out


def write_baseline(per_source, path=BASELINE):
    with open(path, "w") as f:
        f.write("# Field-script commands each run of the suite is known to\n"
                "# execute. One line per source, engine and command, sorted.\n"
                "#\n"
                "# This is a RATCHET: a test fails when a run stops reaching a\n"
                "# command listed against it, because a station that quietly\n"
                "# stops running the code it was authored to run still passes\n"
                "# its digest. The source is the .cov file's stem, so each test\n"
                "# holds only the runs it actually drove to their own rows.\n"
                "#\n"
                "# Names, not opcode numbers, so an upstream insertion does not\n"
                "# read as a hundred regressions. Regenerate with\n"
                "# pc/tests/pc_scrcov.py --write-baseline <cov files>.\n")
        for source in sorted(per_source):
            for engine, cmds in sorted(names(per_source[source]).items()):
                for cmd in sorted(cmds):
                    f.write("%s %s %s\n" % (source, engine, cmd))


def check(per_source, require=(), path=BASELINE):
    """[complaint]; what a run reached last time and does not reach now."""
    was = read_baseline(path)
    bad = []
    for source in require:
        if source not in per_source:
            bad.append("%s produced no coverage file at all" % source)
    for source, counts in sorted(per_source.items()):
        if source not in was:
            continue
        now = names(counts)
        for engine, cmds in sorted(was[source].items()):
            lost = sorted(cmds - now.get(engine, set()))
            if lost:
                bad.append("%s stopped executing %d %s command(s): %s"
                           % (source, len(lost), engine, ", ".join(lost[:6])))
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("cov", nargs="*", help=".cov files written by PC_SCRIPT_COV")
    ap.add_argument("--never", action="store_true",
                    help="list the real commands nothing has executed")
    ap.add_argument("--baseline", action="store_true",
                    help="fail if a run stopped reaching what it used to")
    ap.add_argument("--write-baseline", action="store_true")
    ap.add_argument("--short", action="store_true",
                    help="one line from the committed baseline, for `make progress`")
    args = ap.parse_args()

    if args.short:
        # From the baseline, not from build/, so it answers cold on a tree
        # nobody has run the suite in yet.
        rows = summarise({e: {i: 1 for i, r in enumerate(table(e))
                              if r[0] in cmds}
                          for e, cmds in baseline_names(read_baseline()).items()})
        print("  ".join("%s %d/%d" % (e, run, real)
                        for e, run, real, _s, _d in rows))
        return 0

    paths = [p for p in args.cov if os.path.exists(p)]
    per_source = read(paths)
    counts = merge(per_source)

    print("script coverage over %d run(s)" % len(paths))
    for engine, run, real, slots, dispatches in summarise(counts):
        print("  %-9s %4d of %4d real commands (%3d%%), %d table slot(s), "
              "%d dispatch(es)"
              % (engine, run, real, 100 * run // max(real, 1), slots, dispatches))

    if args.never:
        got = names(counts)
        for engine in ("field", "frontier"):
            missing = [c for c, _h, placeholder in table(engine)
                       if not placeholder and c not in got[engine]]
            print("\n%s: %d never executed" % (engine, len(missing)))
            for c in missing:
                print("  %s" % c)

    if args.write_baseline:
        write_baseline(per_source)
        print("baseline written: %d source(s), %d row(s)"
              % (len(per_source),
                 sum(len(v) for s in per_source.values()
                     for v in names(s).values())))

    if args.baseline:
        bad = check(per_source)
        for b in bad:
            print("  %s" % b)
        return 1 if bad else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
