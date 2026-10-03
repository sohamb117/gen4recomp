#!/usr/bin/env python3
"""
Reconcile `static` declarations mwcc accepted but GCC rejects.

The decomp is full of the same constraint violation in two costumes:

  * A header declares a symbol without a storage class and the `.c` declares
    or defines it `static`.
  * The `.c` forward-declares a data table without `static` (mwcc needed the
    early declaration for ordering) and defines it `static` further down.

C11 6.2.2p7 makes both undefined, and GCC rejects them with no flag to demote
the error. mwcc accepted them, so the decomp never noticed. The sources stay
pristine; this rewrites only the build-tree copy of the file.

The two costumes want opposite fixes, and the compiler's own diagnostics say
which is which. Every error names the static declaration's location and the
"previous declaration" it collides with:

  * previous declaration in another file (a header we must not edit): the
    symbol is meant to be visible, so drop `static` from the `.c` lines GCC
    pointed at.
  * previous declaration in the same file: the symbol is file-local, so add
    `static` to the forward declaration instead.

Both edits touch exactly the lines GCC reported, for exactly the symbols GCC
reported, so no list of names is kept here. Fixing one collision can reveal
the next, so the fix-and-recompile loop runs until the file is clean of this
error class or stops making progress. Once a name has collided with a header
it is locked to the drop-`static` fix, so the two edits cannot chase each
other in a circle.

Usage:
    unstatic.py --cc "gcc -m32 ..." INPUT.c OUTPUT.c

Exits non-zero, having written nothing, when the input does not have this
problem or when the rewritten file still does not compile.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

# GCC quotes identifiers with U+2018/U+2019 in a UTF-8 locale and with plain
# apostrophes under LC_ALL=C. The probe forces the latter, but accept both;
# a wrong locale should not turn this into a silent no-op.
Q = "[‘'\"]([A-Za-z_]\\w*)[’'\"]"
DIAG = re.compile(
    r"^(.+?):(\d+):\d+: error: static declaration of " + Q +
    r" follows non-static declaration", re.M)
NOTE = re.compile(
    r"^(.+?):(\d+):\d+: note: previous declaration of " + Q, re.M)

MAX_ROUNDS = 10


def compile_stderr(cc, path):
    env = dict(os.environ, LC_ALL="C")
    r = subprocess.run(cc + ["-fsyntax-only", path],
                       capture_output=True, text=True, env=env)
    return r.returncode, r.stderr


def conflicts(stderr):
    """
    (name, err_file, err_line, prev_file, prev_line) for every
    static-follows-non-static error whose "previous declaration" note GCC
    printed.  The note always follows its error; pair each error with the
    next note that names the same identifier.
    """
    events = []   # (pos, kind, file, line, name)
    for m in DIAG.finditer(stderr):
        events.append((m.start(), "err", m.group(1), int(m.group(2)),
                       m.group(3)))
    for m in NOTE.finditer(stderr):
        events.append((m.start(), "note", m.group(1), int(m.group(2)),
                       m.group(3)))
    events.sort()

    out = []
    for i, (_, kind, efile, eline, name) in enumerate(events):
        if kind != "err":
            continue
        for _, k2, pfile, pline, n2 in events[i + 1:]:
            if k2 == "note" and n2 == name:
                out.append((name, efile, eline, pfile, pline))
                break
    return out


def same_file(a, b):
    try:
        return os.path.realpath(a) == os.path.realpath(b)
    except OSError:
        return False


def word_on_line(name, line):
    return re.search(r"\b%s\b" % re.escape(name), line) is not None


def apply_fixes(lines, found, src, policy):
    """
    Edit `lines` (of file `src`) in place per the conflicts in `found`.
    `policy` maps name -> "drop", sticky across rounds: a name that ever
    collided with another file keeps dropping `static` even when a later
    round shows a same-file collision (the forward declaration it was just
    reconciled with).  Returns the number of edits made.
    """
    for name, _, _, pfile, _ in found:
        if not same_file(pfile, src):
            policy[name] = "drop"

    hits = 0
    for name, efile, eline, pfile, pline in found:
        if policy.get(name) == "drop":
            # The symbol must stay external: strip `static` from the
            # declaration GCC pointed at, which must be in our file.
            if not same_file(efile, src):
                continue
            line = lines[eline - 1]
            if not word_on_line(name, line):
                continue
            stripped, n = re.subn(r"\bstatic\b\s*", "", line, count=1)
            if n:
                lines[eline - 1] = stripped
                hits += 1
        else:
            # File-local symbol: make the earlier declaration `static` too.
            if not same_file(pfile, src):
                continue
            line = lines[pline - 1]
            if re.match(r"\s*static\b", line) or not word_on_line(name, line):
                continue
            lines[pline - 1] = "static " + line
            hits += 1
    return hits


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cc", required=True,
                    help="the compiler command line, as one string")
    ap.add_argument("input")
    ap.add_argument("output")
    args = ap.parse_args()

    cc = args.cc.split()
    _, stderr = compile_stderr(cc, args.input)
    if not conflicts(stderr):
        print("%s: no static/non-static conflict" % args.input,
              file=sys.stderr)
        return 1

    with open(args.input, "r", errors="replace") as fh:
        lines = fh.readlines()

    # Fix, recompile, and fix what the first fix uncovered.  The probe file
    # lives beside the output so it sees the same include environment.
    fd, probe = tempfile.mkstemp(
        suffix=".c", dir=os.path.dirname(args.output) or ".")
    os.close(fd)
    policy = {}
    fixed_names = set()
    try:
        total = 0
        for _ in range(MAX_ROUNDS):
            with open(probe, "w") as fh:
                fh.writelines(lines)
            rc, stderr = compile_stderr(cc, probe)
            found = conflicts(stderr)
            if not found:
                break
            hits = apply_fixes(lines, found, probe, policy)
            if not hits:
                print("%s: GCC objects to %s, but no line was found to "
                      "change" % (args.input,
                                  ", ".join(sorted({f[0] for f in found}))),
                      file=sys.stderr)
                return 1
            total += hits
            fixed_names.update(f[0] for f in found)
        else:
            print("%s: static/non-static conflicts kept appearing after %d "
                  "rounds; giving up" % (args.input, MAX_ROUNDS),
                  file=sys.stderr)
            return 1

        if not total:
            print("%s: no static/non-static conflict" % args.input,
                  file=sys.stderr)
            return 1

        # Only hand back a source that actually compiles.  These edits change
        # linkage, and if the file is broken in some further way the caller
        # should see the original error rather than one from a rewritten file.
        if rc != 0:
            print("%s: still does not compile after reconciling %s"
                  % (args.input, ", ".join(sorted(fixed_names))),
                  file=sys.stderr)
            sys.stderr.write(stderr)
            return 1
    finally:
        os.unlink(probe)

    with open(args.output, "w") as fh:
        fh.writelines(lines)
    for name in sorted(fixed_names):
        how = ("dropped `static` to match the outside declaration"
               if policy.get(name) == "drop"
               else "made the forward declaration `static`")
        print("%s: %s: %s" % (args.input, name, how), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
