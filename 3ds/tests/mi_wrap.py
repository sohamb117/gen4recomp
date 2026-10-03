#!/usr/bin/env python3
"""The wrapped memory primitives: the link's list against the wrappers.

`--wrap` has one failure mode and it is silent. A name in MIWRAPNAMES with no
`__wrap_` behind it links exactly as if the flag were not there, so the calls
keep reaching the real function with an untranslated pointer and nothing says
so; a `__wrap_` with no flag is dead code that looks like coverage. Neither
shows up as an error, a warning or a missing symbol.

So the two lists are compared, both ways, against the objects rather than
against each other's source. The Makefile's list is read from the Makefile and
the wrappers from the compiled object, so a wrapper that failed to compile is
a failure here too.

Also checks that the file the wrappers live in does not define the real names:
a strong definition next to a wrapper is a multiple-definition error waiting
for someone to drop the flag.
"""

import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
MAKEFILE = os.path.join(ROOT, "3ds", "Makefile")
OBJ = os.path.join(ROOT, "build", "3ds", "3ds_mi_host.o")


def makefile_names():
    text = open(MAKEFILE, encoding="utf-8").read()
    m = re.search(r"^MIWRAPNAMES\s*:=\s*((?:.*\\\n)*.*)$", text, re.M)
    if not m:
        sys.exit("3ds/Makefile: no MIWRAPNAMES assignment")
    body = m.group(1).replace("\\\n", " ")
    return set(body.split())


def object_symbols():
    nm = os.environ.get("NM", "arm-none-eabi-nm")
    try:
        out = subprocess.run([nm, "--defined-only", OBJ], check=True,
                             capture_output=True, text=True).stdout
    except FileNotFoundError:
        sys.exit("%s not on PATH; source 3ds-env.sh" % nm)
    except subprocess.CalledProcessError as exc:
        sys.exit("%s: %s" % (OBJ, exc.stderr.strip()))
    wrapped, defined = set(), set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 3 or parts[1] != "T":
            continue
        name = parts[2]
        if name.startswith("__wrap_"):
            wrapped.add(name[len("__wrap_"):])
        else:
            defined.add(name)
    return wrapped, defined


def main():
    if not os.path.exists(OBJ):
        sys.exit("no %s; build the port first" % OBJ)

    listed = makefile_names()
    wrapped, defined = object_symbols()

    problems = []
    for name in sorted(listed - wrapped):
        problems.append("%s is wrapped in the link and has no __wrap_%s"
                        % (name, name))
    for name in sorted(wrapped - listed):
        problems.append("__wrap_%s exists and %s is not wrapped in the link"
                        % (name, name))
    for name in sorted(listed & defined):
        problems.append("%s is defined beside its own wrapper" % name)

    if problems:
        for line in problems:
            print("  FAILED " + line)
        return 1

    print("  %d wrapped memory primitive(s), list and wrappers agree"
          % len(listed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
