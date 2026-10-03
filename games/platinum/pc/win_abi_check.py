#!/usr/bin/env python3
"""Check a built Windows exe against the oldest Windows it claims to run on.

The port ships a 32-bit exe and the machines it is asked to run on include
Windows XP and a stock Windows 7. Two things silently take that away, both at
link time and neither visible in the source:

  * A C library out of api-ms-win-crt-*.dll (the UCRT), which is a Windows 10
    component, on 7 it needs a redistributable and on XP it does not exist;

  * An import that arrived after XP. The loader resolves every import before
    main() runs, so one such name is the difference between a game and a
    dialog box saying the entry point could not be located.

Both are properties of the finished file, so they are read off the finished
file. The list below is the imports this port has actually acquired by
accident, not an attempt to enumerate the Win32 API: anything newer that
arrives will show up as a failure to start, and belongs here when it does.
"""

import argparse
import re
import subprocess
import sys

# Names outside Windows XP SP3's kernel32/user32. Kept short on purpose;
# each one is here because a real build imported it.
POST_XP = {
    "GetTickCount64",                 # winpthread's clock_gettime (Vista)
    "InitializeConditionVariable",    # Vista
    "SleepConditionVariableCS",
    "WakeConditionVariable",
    "WakeAllConditionVariable",
    "InitializeSRWLock",              # Vista
    "AcquireSRWLockExclusive",
    "ReleaseSRWLockExclusive",
    "InitOnceExecuteOnce",            # Vista
    "GetSystemTimePreciseAsFileTime", # Windows 8
    "QueryFullProcessImageNameA",     # Vista
    "QueryFullProcessImageNameW",
    "GetFinalPathNameByHandleA",      # Vista
    "GetFinalPathNameByHandleW",
    "SetDefaultDllDirectories",       # Windows 8 (XP even with KB2533623)
    "AddDllDirectory",
    "CancelIoEx",                     # Vista
    "GetLogicalProcessorInformationEx",
    "SetThreadStackGuarantee",        # Vista
    "CreateThreadpoolWork",           # Vista
    "InetPtonA",                      # Vista
    "inet_pton",
}

# Two spellings, because binutils changed the column layout between the
# version on this machine's PATH and the one in /usr/bin:
#   02dac270  <none>  0015  AddVectoredExceptionHandler
#   2dac4a4        21  AddVectoredExceptionHandler
# An address, then an ordinal column that may or may not carry "<none>",
# then the name. Getting this wrong is silent, the parse finds nothing and
# the check passes everything, so the floor below is part of the check.
IMPORT = re.compile(
    r"^\s+[0-9a-f]+\s+(?:<none>\s+)?[0-9a-f]+\s+([A-Za-z_][A-Za-z0-9_@?$.]*)\s*$")
DLLNAME = re.compile(r"^\s*DLL Name:\s*(\S+)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("exe")
    ap.add_argument("--objdump", default="objdump")
    args = ap.parse_args()

    try:
        out = subprocess.run([args.objdump, "-p", args.exe],
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        print("win-abi: cannot read %s: %s" % (args.exe, e), file=sys.stderr)
        return 2

    dlls, names = set(), set()
    for line in out.splitlines():
        m = DLLNAME.match(line)
        if m:
            dlls.add(m.group(1).lower())
            continue
        m = IMPORT.match(line)
        if m:
            names.add(m.group(1))

    bad = []
    # A parse that found almost nothing has not proved anything. This port's
    # exe imports well over a hundred names; a couple of dozen is already
    # implausible and means the column layout moved again.
    if len(names) < 20:
        print("win-abi: read only %d import name(s) from %s, the import "
              "table did not parse, so nothing was checked" %
              (len(names), args.exe), file=sys.stderr)
        return 2
    ucrt = sorted(d for d in dlls if d.startswith("api-ms-win-crt"))
    if ucrt:
        bad.append("links the C library out of the UCRT (%s); Windows XP has "
                   "none and a stock Windows 7 needs a redistributable" %
                   ", ".join(ucrt[:3]))
    late = sorted(names & POST_XP)
    if late:
        bad.append("imports %s, which Windows XP does not have" %
                   ", ".join(late))

    if bad:
        print("win-abi: %s" % args.exe, file=sys.stderr)
        for b in bad:
            print("win-abi:   %s" % b, file=sys.stderr)
        print("win-abi: see the toolchain notes at the top of pc/Makefile.win",
              file=sys.stderr)
        return 1

    print("  WINABI  %s: msvcrt, %d imports, nothing newer than XP"
          % (args.exe.rsplit("/", 1)[-1], len(names)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
