#!/usr/bin/env python3
"""3ds/tests/libc_shadow.py: game symbols that shadow the host's C library.

The game and the host libraries are linked into one binary here, and the game
came from a console with its own C library. Where both define a name, the
game's object wins: an object's definition satisfies the reference before the
linker ever looks in an archive, and that is true of a WEAK definition too, so
the weaken pass does not change it and neither does `-u`.

That is not a hypothetical. NitroDWC's GameSpy port defines `time()` and
answers `OS_GetTick()` after asserting `OS_IsTickAvailable()`. libctru's
`romfsInit()` reads the clock while mounting, so with the game in the link
that assert fired before the SDK's OS existed and the process was torn down
with no screen and nothing in any log. The build renames that one in its
object.

So this derives the set and requires every name in it to be accounted for:

    3ds/tests/libc_shadow.py            verify, print the summary
    3ds/tests/libc_shadow.py --list     one line per name

A name that is not in the table below fails, whether or not anything calls it
today, because whether it is reached is a property of the host libraries and
not of this tree.

Needs `make -f 3ds/Makefile game`.
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OBJ = os.path.join(ROOT, "build", "3ds", "obj")
PREFIX = os.environ.get("DEVKITARM", "") + "/bin/arm-none-eabi-"
ARCH = ["-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard"]

# Names the build MOVES out of the way, old -> new. The check is the other way
# round from the list below: the old name must no longer be defined by any
# game object, and the new one must be, which is what says the objcopy ran.
RENAMED = {
    "time": "dwc_time",
}

# Names that stay and are safe because nothing in this build reaches them.
# GameSpy's BSD socket layer: libctru has the same names in its socket
# service, the wireless libraries are excluded, ppwlobby is stubbed, and no
# host file here opens a socket.
INERT = {
    "accept": "GameSpy socket, unreachable: no socket service in this build",
    "bind": "GameSpy socket, unreachable",
    "closesocket": "GameSpy socket, unreachable",
    "connect": "GameSpy socket, unreachable",
    "getsockname": "GameSpy socket, unreachable",
    "getsockopt": "GameSpy socket, unreachable",
    "inet_addr": "GameSpy socket, unreachable",
    "listen": "GameSpy socket, unreachable",
    "recv": "GameSpy socket, unreachable",
    "recvfrom": "GameSpy socket, unreachable",
    "send": "GameSpy socket, unreachable",
    "sendto": "GameSpy socket, unreachable",
    "setsockopt": "GameSpy socket, unreachable",
    "shutdown": "GameSpy socket, unreachable",
    "socket": "GameSpy socket, unreachable",
}


def defined(paths):
    out = set()
    for chunk in [paths[i:i + 40] for i in range(0, len(paths), 40)]:
        r = subprocess.run([PREFIX + "nm", "--defined-only"] + chunk,
                           capture_output=True, text=True)
        for line in r.stdout.splitlines():
            p = line.split()
            if len(p) == 3 and p[1] in "TDBWVRG":
                out.add(p[2])
    return out


def archive(name):
    r = subprocess.run([PREFIX + "gcc"] + ARCH + ["-print-file-name=" + name],
                       capture_output=True, text=True)
    return r.stdout.strip()


def main():
    if not os.path.isdir(OBJ):
        print("libc_shadow: no objects, make -f 3ds/Makefile game")
        return 1

    objs = []
    for base, _dirs, files in os.walk(OBJ):
        objs += [os.path.join(base, f) for f in files if f.endswith(".o")]
    if not objs:
        print("libc_shadow: no objects, make -f 3ds/Makefile game")
        return 1

    libs = [archive("libc.a"), archive("libg.a"), archive("libsysbase.a"),
            os.path.join(os.environ.get("DEVKITPRO", ""), "libctru/lib/libctru.a")]
    libs = [l for l in libs if l and os.path.exists(l)]
    if not libs:
        print("libc_shadow: no host libraries found, source 3ds-env.sh")
        return 1

    game = defined(objs)
    shadowed = sorted(game & defined(libs))

    unknown = [s for s in shadowed if s not in INERT]
    stale = [s for s in INERT if s not in shadowed]
    unmoved = [old for old in RENAMED if old in game]
    missing = [new for new in RENAMED.values() if new not in game]

    if "--list" in sys.argv:
        for s in shadowed:
            print(f"  {s:<14} {INERT.get(s, 'UNACCOUNTED')}")
        for old, new in RENAMED.items():
            print(f"  {old:<14} moved to {new} in its object")

    for s in unknown:
        print(f"  {s} shadows a host library symbol and nothing accounts for it")
    for s in stale:
        print(f"  {s} is in the table but the game no longer defines it")
    for s in unmoved:
        print(f"  {s} was supposed to be renamed in its object and still is not")
    for s in missing:
        print(f"  {s} is missing: the rename that produces it did not run")

    if unknown or stale or unmoved or missing:
        print(f"libc_shadow: {len(unknown) + len(stale) + len(unmoved) + len(missing)}"
              f" problem(s) over {len(shadowed)} shadowed name(s)")
        return 1

    print(f"libc_shadow: {len(shadowed)} game symbol(s) shadow a host library "
          f"name and are unreachable, {len(RENAMED)} moved out of the way")
    return 0


if __name__ == "__main__":
    sys.exit(main())
