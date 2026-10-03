#!/usr/bin/env python3
"""Read a CIA back and say whether it is the title this port meant to build.

    3ds/tests/cia_check.py build/3ds/pokeplatinum.cia

makerom is quiet when it succeeds and it is quiet in the same way when a
field in the RSF is spelled wrong; an unknown key is skipped, not an error,
so a typo in SystemMode or FileSystemAccess produces a CIA that installs and
then behaves like a different application. Everything checked here is a field
that would fail on the console and nowhere earlier:

  title id    the id the RSF derives, in the ticket AND in the TMD
  64MB        the Old 3DS application memory region; 9.3 measured the need
  DirectSdmc  the port's cartridge, save, input script and reports all live
              on the SD card, and an installed title cannot read it without
  SMDH        the icon, or the Home Menu shows the title as a blank tile

The structures are the ones 3dbrew documents. Nothing here needs a key: the
CIA's contents are unencrypted (the RSF asks for that) and the checks are all
plain reads.
"""

import os
import struct
import sys

TITLE_ID = 0x000400000FF58200

SIG_RSA2048_SHA256 = 0x00010004
SIG_BLOCK = 0x140            # 4 type + 0x100 signature + 0x3C padding
TICKET_TITLE_ID = 0x9C       # into the ticket body
TMD_TITLE_ID = 0x4C          # into the TMD body

NCCH_MAGIC = 0x100
EXHEADER = 0x200             # NCCH header is 0x200 bytes
ACI = 0x200                  # into the extended header, after the SCI
ACI_PROGRAM_ID = 0x00
ACI_FLAG0 = 0x0E             # ideal processor, affinity, Old 3DS system mode
ACI_FS_ACCESS = 0x48
FS_DIRECT_SDMC = 1 << 7
SYSTEM_MODE_64MB = 0


def align(n):
    return (n + 63) & ~63


def check(path):
    with open(path, "rb") as f:
        d = f.read()

    bad = []

    def want(held, what):
        if not held:
            bad.append(what)

    hdr, _type, _ver, cert, tik, tmd, meta = struct.unpack("<IHHIIII", d[:24])
    want(hdr == 0x2020, "header size 0x%X, not 0x2020" % hdr)
    want(meta > 0, "no meta region, so no icon")

    tik_at = align(hdr) + align(cert)
    tmd_at = tik_at + align(tik)
    content = tmd_at + align(tmd)

    for name, at, into in (("ticket", tik_at, TICKET_TITLE_ID),
                           ("TMD", tmd_at, TMD_TITLE_ID)):
        sig, = struct.unpack(">I", d[at:at + 4])
        want(sig == SIG_RSA2048_SHA256,
             "%s signature type 0x%X" % (name, sig))
        at += SIG_BLOCK + into
        got, = struct.unpack(">Q", d[at:at + 8])
        want(got == TITLE_ID,
             "%s title id 0x%016X, not 0x%016X" % (name, got, TITLE_ID))

    want(d[content + NCCH_MAGIC:content + NCCH_MAGIC + 4] == b"NCCH",
         "content is not an NCCH")

    aci = content + EXHEADER + ACI
    got, = struct.unpack("<Q", d[aci + ACI_PROGRAM_ID:aci + ACI_PROGRAM_ID + 8])
    want(got == TITLE_ID,
         "exheader program id 0x%016X, not 0x%016X" % (got, TITLE_ID))

    mode = d[aci + ACI_FLAG0] >> 4
    want(mode == SYSTEM_MODE_64MB,
         "Old 3DS system mode %d, not %d (64MB)" % (mode, SYSTEM_MODE_64MB))

    fs, = struct.unpack("<Q", d[aci + ACI_FS_ACCESS:aci + ACI_FS_ACCESS + 8])
    want(fs & FS_DIRECT_SDMC, "no DirectSdmc: the SD card is unreachable")

    want(d[len(d) - meta + 0x400:len(d) - meta + 0x404] == b"SMDH",
         "no SMDH in the meta region")

    return bad


def main():
    if len(sys.argv) != 2:
        print("usage: cia_check.py <file.cia>", file=sys.stderr)
        return 2
    path = sys.argv[1]
    try:
        bad = check(path)
    except (OSError, struct.error) as e:
        print("cia_check: %s: %s" % (path, e), file=sys.stderr)
        return 1
    for line in bad:
        print("cia_check: %s" % line, file=sys.stderr)
    if bad:
        return 1
    print("  cia     : 0x%016X, 64MB, DirectSdmc, icon, %d bytes"
          % (TITLE_ID, os.path.getsize(path)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
