#!/usr/bin/env python3
"""
3ds/tests/one_cpu.py: one virtual CPU, kept as a check rather than a claim.

Every guest thread in this port runs on one host thread. `OS_CreateThread` is
the SDK's own scheduler compiled from the cartridge's C, keeping its queues in
plain memory; a context switch is that scheduler saving registers into an
`OSContext`, which is what 7.10 answers on this console. Nothing in the game
may ask the operating system for a thread, because the SDK's critical sections
are `OS_DisableInterrupts` (a variable in this port) so real concurrency
would race everything a DS was entitled to treat as atomic.

7.9 asked for the record of any SDK file that starts a real host thread. There
are none, and this is that record: no object in the link references
`threadCreate`, `svcCreateThread`, `pthread_create` or `thrd_create`. It is a
check and not a sentence because the case worth catching is the one that
arrives later, a new SDK library, or a port file that reaches for a thread
to make something asynchronous.

libctru's own threads are not in scope and never were. `apt` has a service
thread, `gsp` an event thread, and NDSP will add an audio thread at 8.4. None
of them runs guest C, and `cpu_assert_main()` in 3ds/src/3ds_cpu.c is what
holds that at run time; a grep cannot see a callback.

Needs `make -f 3ds/Makefile game`.

    3ds/tests/one_cpu.py            verify, print the summary
    3ds/tests/one_cpu.py --list     one line per object that references one
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OBJ = os.path.join(ROOT, "build", "3ds", "obj")
PORT = os.path.join(ROOT, "build", "3ds")

# The host's ways of getting a thread. libctru's threadCreate, the kernel call
# under it, newlib's pthread and C11's. A name here that turns up in an object
# is a second CPU that the SDK's scheduler does not know about.
HOST_THREAD = ("threadCreate", "svcCreateThread", "pthread_create",
               "thrd_create", "_beginthread", "_beginthreadex")

PREFIX = os.environ.get("DEVKITARM", "") + "/bin/arm-none-eabi-"


def objects():
    """Every object the game link takes. build/3ds/abi is the measuring
    probe and build/3ds/*.o under a name the link does not use are not in it,
    so the walk is the two directories the link's own find(1) walks."""
    out = []
    for base, depth in ((OBJ, 99), (PORT, 1)):
        for dirpath, _dirs, files in os.walk(base):
            if os.path.relpath(dirpath, PORT).split(os.sep)[0] == "abi":
                continue
            if dirpath.count(os.sep) - base.count(os.sep) >= depth:
                continue
            for f in files:
                if f.endswith(".o"):
                    out.append(os.path.join(dirpath, f))
    return sorted(set(out))


def undefined(paths):
    """{object: set(undefined names)}, in batches, one nm per object over
    1,400 objects is most of this test's run time."""
    found = {}
    for i in range(0, len(paths), 200):
        batch = paths[i:i + 200]
        out = subprocess.run([PREFIX + "nm", "-u"] + batch,
                             capture_output=True, text=True).stdout
        cur = batch[0] if len(batch) == 1 else None
        for line in out.splitlines():
            if line.endswith(":") and not line.startswith(" "):
                cur = line[:-1]
            elif line.strip() and cur is not None:
                found.setdefault(cur, set()).add(line.split()[-1])
    return found


def main():
    if not os.path.isdir(OBJ):
        print("one_cpu: no objects, make -f 3ds/Makefile game", file=sys.stderr)
        return 2
    if not os.environ.get("DEVKITARM"):
        print("one_cpu: DEVKITARM is not set", file=sys.stderr)
        return 2

    bad = []
    objs = objects()
    refs = undefined(objs)

    hits = []
    for path, names in refs.items():
        for n in sorted(names):
            if any(h in n for h in HOST_THREAD):
                hits.append((os.path.relpath(path, ROOT), n))
    for path, n in hits:
        bad.append("%s references %s, a second host CPU the SDK's scheduler "
                   "does not know about" % (path, n))

    # And the source, for a file that is not compiled into this link yet. The
    # port's own directories only: the SDK ships arm7 sources this build never
    # touches and pc/src carries another operating system's file that this port
    # dropped.
    src_hits = []
    for d in ("3ds/src", "pc/hw"):
        for dirpath, _dirs, files in os.walk(os.path.join(ROOT, d)):
            for f in files:
                if not f.endswith((".c", ".h")):
                    continue
                p = os.path.join(dirpath, f)
                text = open(p, errors="replace").read()
                for h in HOST_THREAD:
                    # In code, not in the prose that explains why there is none.
                    if re.search(r"^[^*/]*\b%s\s*\(" % re.escape(h), text,
                                 re.MULTILINE):
                        src_hits.append((os.path.relpath(p, ROOT), h))
    for p, h in src_hits:
        bad.append("%s calls %s" % (p, h))

    # OS_CreateThread stays the SDK's compiled C: defined once in the link, by
    # the SDK's own object, with nothing strong overriding it. A port file that
    # replaced it would be the moment this model changed shape.
    defs = []
    for i in range(0, len(objs), 200):
        batch = objs[i:i + 200]
        out = subprocess.run([PREFIX + "nm", "--defined-only"] + batch,
                             capture_output=True, text=True).stdout
        cur = batch[0] if len(batch) == 1 else None
        for line in out.splitlines():
            if line.endswith(":") and not line.startswith(" "):
                cur = line[:-1]
            elif re.search(r"\s[TW]\sOS_CreateThread$", line):
                defs.append((os.path.relpath(cur, ROOT), line.split()[-2]))
    defs = sorted(set(defs))
    if len(defs) != 1:
        bad.append("OS_CreateThread is defined in %d object(s): %s"
                   % (len(defs), ", ".join(d[0] for d in defs)))
    elif not defs[0][0].endswith("os/src/os_thread.o"):
        bad.append("OS_CreateThread comes from %s, not the SDK's os_thread.o"
                   % defs[0][0])

    if "--list" in sys.argv:
        for path, n in hits:
            print("%-58s %s" % (path, n))
        for path, kind in defs:
            print("%-58s OS_CreateThread (%s)" % (path, kind))

    if bad:
        for b in bad:
            print("one_cpu: " + b, file=sys.stderr)
        return 1

    print("one_cpu: %d object(s), none asks the host for a thread; "
          "OS_CreateThread is the SDK's os_thread.o" % len(objs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
