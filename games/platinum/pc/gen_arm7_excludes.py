#!/usr/bin/env python3
"""Which NitroSDK-family C sources belong to the ARM7, read out of the SDK's own
meson files rather than guessed from their names.

Keeping ARM7 code out of the port's link matters because this is a
single-namespace ELF and the other processor's symbols collide with the ARM9's.
That used to cost nothing to enforce, because the ARM7 arrived as a prebuilt
blob and pc/Makefile simply never looked there. Upstream changed that: the ARM7
became a compiled binary, so its C moved into `libraries/`, which is exactly
where the port's `find` looks. 77 files began failing to compile in one merge,
every one of them ARM7, and the errors were the collision itself.

Why not match on the name. Most of them do say `_arm7`, and it would work for
44 of 77. It would silently keep `rtc/src/control.c`, `snd/src/snd_seq.c`,
`pad/src/pad.c` and `exi/src/exi.c`, ARM7 sources whose names say nothing, two
of them in libraries that are ARM7-only. A filename heuristic here is a rule
that is 57% right and looks 100% right.

The oracle. Each library declares its halves explicitly:

    librtc      = library('rtc',      sources: [librtc_srcs, ...])
    librtc_arm7 = library('rtc_arm7', sources: [librtc_arm7_srcs, ...],
                          dependencies: [nitrosdk_arm7_args_dep])

so a source-list variable is ARM7 if every `library()` that consumes it is an
ARM7 library. That is read here, so when upstream bumps the SDK wrap and moves
more code across, this follows without being edited.

The ambiguity is per file, not per variable, and getting that wrong is what the
first draft did. The SDK compiles much of its C twice, once per processor with
different flags: `libos_arm7_srcs` lists 20-odd of the same file names as
`libos_srcs`, because os_alarm.c is real code on both sides. A rule that
excluded every file named by an ARM7 source list therefore proposed dropping
all of os/, mi/, card/ and snd/, 140 files instead of 77, including the memory
and interrupt cores the ARM9 cannot boot without.

So a file is excluded only when no non-ARM7 library names it. Excluding a file
the ARM9 needs breaks the link loudly; including an ARM7 file fails its own
compile and lands in the skip count. Both are visible, the first is worse, and
ambiguity resolves toward keeping.

Usage: pc/gen_arm7_excludes.py <sdk-root-dir> [<sdk-root-dir> ...]
Prints one path per line, as found, so pc/Makefile can $(filter-out) them from
its own `find` output directly.
"""

import os
import re
import sys

# `library('name', ... )` up to the closing paren that sits at the start of a
# line. The SDK formats every one of these that way; a block that is not
# formatted that way simply is not recognised, and its sources stay in the
# build, which is the safe direction.
LIBRARY_RE = re.compile(r"^\w*\s*=?\s*library\(\s*'([^']+)'(.*?)^\)",
                        re.M | re.S)
SRCVAR_RE = re.compile(r"\b(\w+_srcs)\b")
FILES_RE = re.compile(r"^\s*(\w+)\s*=\s*files\((.*?)\)", re.M | re.S)
QUOTED_RE = re.compile(r"'([^']+)'")


def is_arm7_library(name, body):
    """An ARM7 library says so twice: in its name and in its arm7 args dep."""
    return name.endswith("_arm7") or "arm7_args_dep" in body


def collect(root):
    """Paths named only by ARM7 libraries in one SDK-family wrap."""
    libdir = os.path.join(root, "libraries")
    if not os.path.isdir(libdir):
        return set()

    arm7_vars, host_vars = set(), set()
    files_by_var = {}

    for dirpath, _dirs, names in os.walk(libdir):
        if "meson.build" not in names:
            continue
        path = os.path.join(dirpath, "meson.build")
        try:
            text = open(path, errors="replace").read()
        except OSError:
            continue

        for name, body in LIBRARY_RE.findall(text):
            srcs = body.split("sources:", 1)
            if len(srcs) != 2:
                continue
            target = arm7_vars if is_arm7_library(name, body) else host_vars
            target.update(SRCVAR_RE.findall(srcs[1]))

        # `files()` paths are relative to the meson.build that names them.
        for var, inner in FILES_RE.findall(text):
            # NOT normpath'd. pc/Makefile spells its SDK roots through
            # $(ROOT), which is ".../pc/..", and enumerates sources with `find`
            # over those same roots, so its paths carry the "/pc/.." too, and
            # $(filter-out) is a STRING match. Normalizing here made this script
            # emit 86 correct paths that matched none of them, so the exclusion
            # silently did nothing and all 448 SDK sources stayed in the build.
            # Paths are echoed in the spelling of the root we were handed.
            files_by_var.setdefault(var, []).extend(
                os.path.join(dirpath, q) for q in QUOTED_RE.findall(inner))

    # Resolved to FILES before differencing: a variable can be ARM7-only while
    # every path in it is also named by an ARM9 list, which is the normal case
    # for the libraries that build the same C for both processors.
    def paths(vars_):
        out = set()
        for v in vars_:
            out.update(files_by_var.get(v, []))
        return out

    arm7 = paths(arm7_vars)

    # A `*_arm7/` DIRECTORY is the ARM7 half of a library by construction, and
    # the meson lists inside one do not necessarily name everything in it:
    # wl_arm7/src/spiEeprom.c is in no library at all, so the SDK's own build
    # never compiles it, and the port's `find` picked it up and failed on it.
    # Directory-scoped rather than a global "no library names it" rule, which was
    # measured and rejected: it would also have dropped 53 NitroDWC GameSpy
    # sources that this port does compile, declared through a pattern the parser
    # above does not read.
    for dirpath, _dirs, names in os.walk(libdir):
        parts = dirpath.replace("\\", "/").split("/")
        if not any(p.endswith("_arm7") for p in parts):
            continue
        arm7.update(os.path.join(dirpath, n)
                    for n in names if n.endswith(".c"))

    return arm7 - paths(host_vars)


def main(argv):
    if len(argv) < 2:
        print("usage: pc/gen_arm7_excludes.py <sdk-root-dir> [<sdk-root-dir> ...]",
              file=sys.stderr)
        return 2
    out = []
    for root in argv[1:]:
        out.extend(collect(root))
    # Sorted and de-duplicated: this feeds $(filter-out), and a stable list
    # keeps the Makefile's own output diffable between SDK bumps.
    for p in sorted(set(out)):
        print(p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
