#!/usr/bin/env python3
"""
3ds/tests/newlib_inventory.py: what the host layer asks of a C library, and
whether this console's C library has it.

The PC port's host layer is 52 objects of glibc-facing C. Some of it is
portable and will compile here untouched, some names a POSIX facility the 3DS
does not have at all, and some exists in newlib under a name that only looks
different. Guessing which is which by grepping for `mmap` finds the famous ones
and misses the quiet ones, so this asks the object files instead.

    3ds/tests/newlib_inventory.py           verify, print the summary
    3ds/tests/newlib_inventory.py --list    one line per symbol
    3ds/tests/newlib_inventory.py --list gap

The question: take every undefined symbol in the PC build's host objects and
subtract everything the build defines anywhere, game, SDK and host alike. What
is left is exactly the set of names glibc supplies today, derived from the
build rather than typed, so a host file that starts calling something new shows
up here.

Then ask devkitARM whether it has each one, by reading the archives the 3DS
link line really uses. A name found there needs no rule, a name not found needs
one, and a name with neither is a failure.

Every gap is one of:

  spelling  the facility exists, glibc spells the symbol its own way, and the
            3DS headers produce the newlib spelling from the same source line
  link      the linker script supplies it, and 3dsx.ld uses different names
  rewrite   a 3ds/src file supplies the behaviour

Weak undefined symbols are not gaps and are counted separately: a `w` entry
resolves to zero and cannot fail a link, which is the point of it.

What this does not answer: a symbol newlib defines can still be a stub that
sets ENOSYS and returns -1. That is a run-time question. This script's claim is
narrower and worth having on its own: nothing in the host layer will fail to
link for a reason nobody wrote down.
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OBJ = os.path.join(ROOT, "build", "pc", "obj")

# The host layer, as directories of the PC build's object tree. `pc` holds
# pc/src plus armrec_rt and the generated C; `pchw` is the melonDS-derived
# hardware models; `arm7snd` is the ARM7 sound tree. `game` is deliberately
# absent: game and SDK code reaches the C library through the DS SDK, which is
# this port's business elsewhere, and mixing them in would bury the 90 names
# that matter under newlib's own string functions.
HOST_DIRS = ["pc", "pchw", "arm7snd"]

ARCH = ["-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard", "-mtp=soft"]

# symbol (exact) or prefix ending in *, kind, why. First match wins.
RULES = [
    # ---- glibc spellings of things newlib has ------------------------------
    ("__errno_location", "spelling",
     "glibc's errno. <errno.h> here expands the same source line to __errno(), "
     "which libc.a has"),
    ("__isoc23_*", "spelling",
     "glibc versions strtol and sscanf by C standard; newlib does not, and the "
     "plain names are in libc.a"),
    ("__isoc99_*", "spelling", "the same versioning, one standard earlier"),
    ("__*_chk", "spelling",
     "_FORTIFY_SOURCE. The 3DS build does not define it, so the header emits "
     "the plain call"),
    ("stdout", "spelling",
     "a macro over _impure_ptr in newlib, so there is no symbol to find and "
     "nothing to port"),
    ("stderr", "spelling", "the same"),

    # ---- the linker script's own symbols -----------------------------------
    ("__executable_start", "link",
     "armrec_rt.c reads the image bounds to decide whether a pointer is host "
     "code. 3dsx.ld names them __start__, __text_start and __end__; the 3DS "
     "backend from 4.1 owns this and does not use the glibc names"),
    ("_etext", "link", "the same, and 3dsx.ld calls it __text_end"),
    ("_edata", "link", "the same"),
    ("_end", "link",
     "pc_spu.c takes it as a heap floor. 3dsx.ld provides __end__"),

    # ---- facilities this console does not have -----------------------------
    ("mmap", "rewrite",
     "identity-mapped guest memory, the port's central wall. 4.1 already "
     "replaced it: 3ds/src/armrec_mem_3ds.c owns the slab and no 3DS object "
     "links armrec_rt.c's mmap half"),
    ("munmap", "rewrite", "the same, and there is no <sys/mman.h> here"),
    ("mprotect", "rewrite",
     "armrec_rt.c write-protects the ROM window. The slab has no page "
     "permissions; the fault report from 4.4 is what a bad store gets"),
    ("shm_open", "rewrite",
     "the PC frontend's shared framebuffer, for a separate viewer process. "
     "There is no second process here, 8.2 hands 3ds_view.c the frame"),
    ("shm_unlink", "rewrite", "the same"),
    ("getcontext", "rewrite",
     "OSContext switching. The SDK scheduler is compiled C and really does "
     "switch threads, so this cannot be dropped: pc_os_context.c has a POSIX "
     "ucontext half and a Windows fiber half, and devkitARM has neither "
     "<ucontext.h> nor fibers. A third backend under 3ds/src, on setjmp or on "
     "hand-written ARM save/restore, is translator work"),
    ("makecontext", "rewrite", "the same"),
    ("setcontext", "rewrite", "the same"),
    ("sigaction", "rewrite",
     "pc_main.c installs the crash handlers. 4.4's fault report is this "
     "port's answer, and it is reached from a libctru exception handler "
     "rather than from a signal"),
    ("backtrace", "rewrite",
     "the crash report's stack walk. Same file, same replacement"),
    ("backtrace_symbols_fd", "rewrite", "the same"),
    ("alarm", "rewrite",
     "the PC watchdog's timeout. 9.7 owns the 3DS equivalent"),
    ("pread", "rewrite",
     "pc_card_rom.c reads the ROM at an offset. 6.2 reads it from RomFS "
     "instead, where the file is mapped and the read is a memcpy"),
    ("pwrite", "rewrite",
     "the same file's save write-back. 9.1 writes to sdmc"),
]


def rule_for(sym):
    for pat, kind, why in RULES:
        if pat == "__*_chk":
            if sym.startswith("__") and sym.endswith("_chk"):
                return kind, why
        elif pat.endswith("*"):
            if sym.startswith(pat[:-1]):
                return kind, why
        elif sym == pat:
            return kind, why
    return None


def nm(args, paths):
    out = subprocess.run(
        ["nm"] + args + ["--format=posix"] + paths,
        capture_output=True, text=True,
    ).stdout
    return out


def host_needed():
    """Undefined in the host objects, defined nowhere in the build."""
    host = []
    for d in HOST_DIRS:
        p = os.path.join(OBJ, d)
        if os.path.isdir(p):
            host += [os.path.join(p, f) for f in sorted(os.listdir(p))
                     if f.endswith(".o")]
    if not host:
        return None, None, None

    allobj = []
    for dirpath, _, files in os.walk(OBJ):
        allobj += [os.path.join(dirpath, f) for f in files if f.endswith(".o")]

    undef = {}
    weak = set()
    for o in host:
        for line in nm([], [o]).splitlines():
            f = line.split()
            if len(f) < 2:
                continue
            if f[1] == "U":
                undef.setdefault(f[0].split("@")[0], set()).add(
                    os.path.basename(o)[:-2])
            elif f[1] == "w":
                weak.add(f[0].split("@")[0])

    defined = set()
    for i in range(0, len(allobj), 400):
        for line in nm(["--defined-only"], allobj[i:i + 400]).splitlines():
            f = line.split()
            if len(f) >= 2 and f[1] not in ("U", "u"):
                defined.add(f[0])

    return ({s: v for s, v in undef.items() if s not in defined},
            len(host), weak - defined)


def toolchain_symbols():
    """Everything the real 3DS link line could resolve, asked of the archives."""
    dka = os.environ.get("DEVKITARM")
    dkp = os.environ.get("DEVKITPRO") or (dka and os.path.dirname(dka))
    if not dka:
        return None
    gcc = os.path.join(dka, "bin", "arm-none-eabi-gcc")
    libgcc = subprocess.run(
        [gcc] + ARCH + ["-print-libgcc-file-name"],
        capture_output=True, text=True).stdout.strip()
    lib = os.path.join(dka, "arm-none-eabi", "lib", "armv6k", "fpu")
    archives = [os.path.join(lib, a) for a in
                ("libc.a", "libm.a", "libsysbase.a")]
    archives.append(os.path.join(dkp, "libctru", "lib", "libctru.a"))
    if libgcc:
        archives.append(libgcc)
    archives = [a for a in archives if os.path.exists(a)]
    if not archives:
        return None

    have = set()
    out = subprocess.run(
        [os.path.join(dka, "bin", "arm-none-eabi-nm"),
         "--defined-only", "--format=posix"] + archives,
        capture_output=True, text=True).stdout
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 2 and f[1] not in ("U", "u"):
            have.add(f[0])
    return have


def main():
    want_list = "--list" in sys.argv
    only = None
    for a in sys.argv[1:]:
        if not a.startswith("-"):
            only = a

    needed, nobj, weak = host_needed()
    if needed is None:
        print("newlib_inventory: no build/pc/obj, run the PC build first",
              file=sys.stderr)
        return 2

    have = toolchain_symbols()
    if have is None:
        print("newlib_inventory: DEVKITARM is not set", file=sys.stderr)
        return 2

    counts = {"newlib": 0, "spelling": 0, "link": 0, "rewrite": 0}
    rows = []
    unclassified = []
    for sym in sorted(needed):
        if sym in have:
            kind, why = "newlib", "in the 3DS libraries"
        else:
            r = rule_for(sym)
            if r is None:
                unclassified.append(sym)
                kind, why = "gap", "NO RULE"
            else:
                kind, why = r
        if kind in counts:
            counts[kind] += 1
        rows.append((sym, kind, why, sorted(needed[sym])))

    if want_list:
        for sym, kind, why, files in rows:
            if only and kind != only:
                continue
            print("%-24s %-9s %s" % (sym, kind, ", ".join(files[:6])))
            if kind != "newlib":
                print("%26s%s" % ("", why))

    if unclassified:
        print("newlib_inventory: %d symbol(s) with no rule: %s"
              % (len(unclassified), " ".join(unclassified)), file=sys.stderr)
        return 1

    print("newlib_inventory: %d host symbols, %d in newlib, %d spelling, "
          "%d linker script, %d rewritten here, %d weak"
          % (len(rows), counts["newlib"], counts["spelling"],
             counts["link"], counts["rewrite"], len(weak)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
