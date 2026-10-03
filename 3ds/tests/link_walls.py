#!/usr/bin/env python3
"""
3ds/tests/link_walls.py: every symbol the game link cannot resolve, named.

`make -f 3ds/Makefile link` compiles all 1,392 game translation units, the
SDK, the host layer and the port, and tries to link them. It does not link
yet, and the plan says so: the log is the deliverable and the walls are
recorded rather than papered over. This is what keeps that list honest,
each unresolved symbol matches a rule that says who owns it, and one that
matches nothing is a failure.

    3ds/tests/link_walls.py            verify, print the summary
    3ds/tests/link_walls.py --list     one line per wall

Why it does not read link.log for the count. It used to, and the number was
wrong in the direction that matters. The link runs with `--gc-sections`, so
the linker only complains about symbols reachable from `main`, and nothing
calls `NitroMain` yet, so most of the game is collected before symbol
resolution ever reaches it. The scheduler's `OS_SaveContext`,
`OS_LoadContext` and `OS_InitContext` were undefined for weeks and `link.log`
never said so; they surfaced only when someone read
`build/3ds/unresolved.txt`, which is defined-vs-referenced over the objects
and does not move with what the linker decides to keep. That file is the
count now.

What `unresolved.txt` adds is every name the LIBRARIES resolve, newlib,
libgcc, libctru. Those are not walls, and rather than a hand-written list
that drifts, this asks the libraries themselves: `nm --defined-only` over
the archives the link actually names, libcitro3d, libctru, newlib, libgcc
and devkitARM's libsysbase, which is where clock_gettime and mkdir live,
cached beside the build. The cache is part of the answer and it can go stale:
11.2 added citro3d to the link and not to the list below, and a cache written
before that hid every C3D_ name for a session. Delete build/3ds/libsyms.txt
after a library is added. A name
that no library defines and no rule owns is a wall nobody has claimed.

link.log is still read for multiple definitions, which is never a wall.
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOG = os.path.join(ROOT, "build", "3ds", "link.log")
UNRESOLVED = os.path.join(ROOT, "build", "3ds", "unresolved.txt")
LIBCACHE = os.path.join(ROOT, "build", "3ds", "libsyms.txt")

# prefix (ending in *) or exact name, who owns it, why.
RULES = [
    # No armrec_* rule any more, and that is deliberate. 3ds/src/armrec_rt_3ds.c
    # answers the whole of that runtime's non-memory half, so an armrec name
    # turning up here again means the PC port grew an export this console does
    # not have; which needs a task, not a rule that was already written.
    # Rules are DELETED once the name is answered, so a name that comes back
    # arrives unowned and fails. The two input names, the digest scan,
    # the pread/pwrite, the image bounds and the sound driver's thirteen
    # arm7_SND_* were all rules here and are all gone: 3ds/src answers the
    # first three, the sound driver is compiled and renamed by 3ds/Makefile
    # now, and the image bounds are the link itself, which linker_defined()
    # reads back from `make linkflags` rather than a rule pretending they are
    # unresolved. The list being empty is the point: every name in the build
    # is answered by something.
]


# A weak undefined symbol is not a wall and is not resolved either: the link
# succeeds and the linker answers every call to one with a nop. That is quiet
# when the call is a `bl` (the caller returns on the next instruction) and
# fatal when it is a tail `b`, which -O2 emits, because then the function
# never returns at all and falls into whatever the linker put next. It cost a
# day: the VRAM remap hooks were weak here for three phases, doing nothing on
# every bank switch, and the build that finally noticed did it by hanging.
#
# So every one of them is named, with what happens when it is called. A new
# one fails this check.
WEAK_OK = [
    ("__register_frame_info", "libgcc's unwinder, null-tested by crtstuff"),
    ("__deregister_frame_info", "same pair"),
    ("userAppInit", "libctru's optional hook, null-tested by __libctru_init"),
    ("userAppExit", "same pair"),
    # The six below are GameSpy's, and they are weak because objcopy --weaken
    # weakens an object's undefined references as well as its definitions,
    # and a weak undefined reference does not pull a member out of an archive,
    # so newlib's copy is never linked. The port does not run NitroDWC (the
    # Windows build hides it outright), so these are nop'd calls in code that
    # never executes. Anything here that the game DOES reach needs a real
    # definition, not a rule.
    ("atof", "NitroDWC GameSpy only"),
    ("atoi", "NitroDWC GameSpy only"),
    ("rand", "NitroDWC GameSpy only"),
    ("srand", "NitroDWC GameSpy only"),
    ("stpcpy", "NitroDWC GameSpy only"),
    ("strcspn", "NitroDWC GameSpy only"),
]


def weak_undefined():
    """(accounted, unaccounted) over the linked ELF. nm prints a lower-case
    `w` with no address for a weak symbol nothing defines."""
    dkp = os.environ.get("DEVKITPRO", os.path.expanduser("~/devkitpro"))
    dka = os.environ.get("DEVKITARM", os.path.join(dkp, "devkitARM"))
    nm = os.path.join(dka, "bin", "arm-none-eabi-nm")
    elf = os.path.join(ROOT, "build", "3ds", "pokeplatinum.elf")
    if not os.path.exists(elf) or not os.path.exists(nm):
        return None, None
    out = subprocess.run([nm, elf], capture_output=True, text=True).stdout
    syms = sorted({line.split()[1] for line in out.split("\n")
                   if line.startswith(" ") and line.split()[:1] == ["w"]})
    known = {name for name, _ in WEAK_OK}
    return [s for s in syms if s in known], [s for s in syms if s not in known]


def owner(sym):
    for pat, who, why in RULES:
        if pat.endswith("*"):
            if sym.startswith(pat[:-1]):
                return who, why
        elif sym == pat:
            return who, why
    return None


def linker_defined():
    """Names the LINK defines rather than any object, the --defsym pairs the
    Makefile passes. No object defines `_end`; the link aliases it to the 3dsx
    script's __end__, so it is resolved and not a wall. Read back from
    `make linkflags` so the list lives in one place."""
    mk = subprocess.run(["make", "-f", os.path.join(ROOT, "3ds", "Makefile"),
                         "linkflags"], capture_output=True, text=True,
                        cwd=ROOT).stdout
    return set(re.findall(r"--defsym=([A-Za-z_.$][\w.$]*)=", mk))


def library_symbols():
    """Every symbol the four linked archives define, cached. Asking the
    libraries beats a hand-written list of libc names, which drifts the first
    time the port calls something new."""
    if os.path.exists(LIBCACHE):
        return set(open(LIBCACHE).read().split())

    dkp = os.environ.get("DEVKITPRO", os.path.expanduser("~/devkitpro"))
    dka = os.environ.get("DEVKITARM", os.path.join(dkp, "devkitARM"))
    gcc = os.path.join(dka, "bin", "arm-none-eabi-gcc")
    nm = os.path.join(dka, "bin", "arm-none-eabi-nm")
    arch = ["-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard"]

    archives = [os.path.join(dkp, "libctru", "lib", a)
                for a in ("libctru.a", "libcitro3d.a")]
    for lib in ("libc.a", "libm.a", "libgcc.a", "libsysbase.a"):
        out = subprocess.run([gcc] + arch + ["-print-file-name=" + lib],
                             capture_output=True, text=True).stdout.strip()
        if out and os.path.exists(out):
            archives.append(out)

    syms = set()
    for a in archives:
        out = subprocess.run([nm, "--defined-only", a],
                             capture_output=True, text=True).stdout
        for line in out.split("\n"):
            f = line.split()
            if len(f) >= 3 and len(f[-2]) == 1 and f[-2] in "ATDBRWVGCi":
                syms.add(f[-1])
    if syms:
        try:
            os.makedirs(os.path.dirname(LIBCACHE), exist_ok=True)
            open(LIBCACHE, "w").write("\n".join(sorted(syms)))
        except OSError:
            pass
    return syms


def main():
    if not os.path.exists(UNRESOLVED):
        print("link_walls: no build/3ds/unresolved.txt, "
              "run `make -f 3ds/Makefile link` first", file=sys.stderr)
        return 2

    if os.path.exists(LOG):
        text = open(LOG, errors="replace").read()
        dup = sorted(set(re.findall(r"multiple definition of `([^']+)'", text)))
        if dup:
            print("link_walls: %d multiple definition(s), which is never a "
                  "wall: %s" % (len(dup), " ".join(dup[:8])), file=sys.stderr)
            return 1

    lib = library_symbols()
    if not lib:
        print("link_walls: no library symbols found, is DEVKITPRO set?",
              file=sys.stderr)
        return 2
    lib |= linker_defined()

    syms = sorted({s for s in open(UNRESOLVED).read().split()
                   if s and s not in lib})
    unowned = []
    rows = []
    for s in syms:
        r = owner(s)
        if r is None:
            unowned.append(s)
            rows.append((s, "NOBODY", "no rule"))
        else:
            rows.append((s, r[0], r[1]))

    if "--list" in sys.argv:
        for s, who, why in rows:
            print("%-32s %-8s %s" % (s, who, why))

    if unowned:
        print("link_walls: %d undefined symbol(s) with no rule: %s"
              % (len(unowned), " ".join(unowned)), file=sys.stderr)
        return 1

    weak, strays = weak_undefined()
    if strays:
        print("link_walls: %d weak undefined symbol(s) nothing accounts for: "
              "%s. The link succeeds and every call to one of these is a nop "
              "-- a tail call to one never returns. Define it, or name it in "
              "WEAK_OK with what happens when it is called."
              % (len(strays), " ".join(strays)), file=sys.stderr)
        return 1

    if "--list" in sys.argv and weak:
        why = dict(WEAK_OK)
        for s in weak:
            print("%-32s %-8s %s" % (s, "WEAK", why[s]))

    by = {}
    for _, who, _ in rows:
        by[who] = by.get(who, 0) + 1
    print("link_walls: %d wall(s), all owned (%s); %d library name(s) "
          "resolved; %s weak undefined name(s), all accounted for"
          % (len(rows),
             ", ".join("%s:%d" % (k, v) for k, v in sorted(by.items())),
             len(open(UNRESOLVED).read().split()) - len(rows),
             len(weak) if weak is not None else "no ELF, 0"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
