#!/usr/bin/env python3
"""Generate weak trap stubs for the PC build from pc/stubs.list.

Usage: gen_stubs.py <stubs.list> <out.c>

Reads ONLY the checked-in list, never unresolved.txt or link.log. That is
the whole design: stubbing a symbol is a deliberate act of adding a line to
pc/stubs.list, and any symbol NOT on the list still breaks the link, so the
link gate keeps meaning something.

List format, one entry per line (blank lines and '#' comments skipped):

    SYMBOL<TAB>func|data<TAB>one-line reason

func entries become weak void(void) functions whose body calls
pc_trap_unreached(symbol, reason) and never returns. data entries become
weak zero-initialized storage: a type-correct declaration where the SDK
declaration is known (DATA_DECLS below, transcribed from the declaring
header/source because including the SDK headers here would collide with the
trap prototypes; see the generated file's comment), otherwise a
conservatively sized, 8-aligned byte array.

Output is deterministic: entries are emitted sorted by symbol name and the
file carries no timestamps.
"""

import sys

# Data symbols with a known SDK declaration, transcribed to plain C types
# valid under this build's ABI (-m32: int/long/pointer are 4 bytes, long
# long is 8). Each value is (declarator, provenance comment).
DATA_DECLS = {
    "gTrainerAITable": (
        "unsigned int gTrainerAITable[1]",
        "extern u32 gTrainerAITable[] (battle_controller_player.c); interpreted bytecode, not code",
    ),
    "_binary_digit_chrData_bin": (
        "unsigned char _binary_digit_chrData_bin[4]",
        "extern u8 [] (os_china.c); dead China-region blob, content never read",
    ),
    "_binary_digit_chrData_bin_end": (
        "unsigned char _binary_digit_chrData_bin_end[4]",
        "extern u8 [] (os_china.c); dead China-region blob, content never read",
    ),
    "_binary_logo_forChina_chrData_bin": (
        "unsigned char _binary_logo_forChina_chrData_bin[4]",
        "extern u8 [] (os_china.c); dead China-region blob, content never read",
    ),
    "_binary_logo_forChina_chrData_bin_end": (
        "unsigned char _binary_logo_forChina_chrData_bin_end[4]",
        "extern u8 [] (os_china.c); dead China-region blob, content never read",
    ),
    "_binary_logo_forChina_palData_bin": (
        "unsigned char _binary_logo_forChina_palData_bin[4]",
        "extern u8 [] (os_china.c); dead China-region blob, content never read",
    ),
    "_binary_logo_forChina_palData_bin_end": (
        "unsigned char _binary_logo_forChina_palData_bin_end[4]",
        "extern u8 [] (os_china.c); dead China-region blob, content never read",
    ),
    "_binary_logo_forChina_scrData_bin": (
        "unsigned char _binary_logo_forChina_scrData_bin[4]",
        "extern u8 [] (os_china.c); dead China-region blob, content never read",
    ),
    "_binary_logo_forChina_scrData_bin_end": (
        "unsigned char _binary_logo_forChina_scrData_bin_end[4]",
        "extern u8 [] (os_china.c); dead China-region blob, content never read",
    ),
    "_binary_notes_forChina_chrData_bin": (
        "unsigned char _binary_notes_forChina_chrData_bin[4]",
        "extern u8 [] (os_china.c); dead China-region blob, content never read",
    ),
    "_binary_notes_forChina_chrData_bin_end": (
        "unsigned char _binary_notes_forChina_chrData_bin_end[4]",
        "extern u8 [] (os_china.c); dead China-region blob, content never read",
    ),
    "_binary_notes_forChina_scrData_bin": (
        "unsigned char _binary_notes_forChina_scrData_bin[4]",
        "extern u8 [] (os_china.c); dead China-region blob, content never read",
    ),
    "_binary_notes_forChina_scrData_bin_end": (
        "unsigned char _binary_notes_forChina_scrData_bin_end[4]",
        "extern u8 [] (os_china.c); dead China-region blob, content never read",
    ),
    "CPSDnsIp": (
        "unsigned int CPSDnsIp[2]",
        "extern CPSInAddr CPSDnsIp[2] (nitroWiFi/cps.h); CPSInAddr is typedef u32, 4 bytes each under -m32",
    ),
    "CPSGatewayIp": (
        "unsigned int CPSGatewayIp",
        "extern CPSInAddr CPSGatewayIp (nitroWiFi/cps.h); CPSInAddr is typedef u32",
    ),
    "CPSMyIp": (
        "unsigned int CPSMyIp",
        "extern CPSInAddr CPSMyIp (nitroWiFi/cps.h); CPSInAddr is typedef u32",
    ),
    "CPSNetMask": (
        "unsigned int CPSNetMask",
        "extern CPSInAddr CPSNetMask (nitroWiFi/cps.h); CPSInAddr is typedef u32",
    ),
    "DWCauthhttpparam": (
        "unsigned char DWCauthhttpparam[32] __attribute__((aligned(4)))",
        "DWCHttpParam (auth/dwc_http.h): const char *url, enum action, unsigned long len_recvbuf, "
        "two function pointers, BOOL, int = 7 x 4 = 28 bytes under -m32, rounded up to 32",
    ),
    "DWCauthingamesncheckresult": (
        "int DWCauthingamesncheckresult",
        "extern DWCAuthIngamesnCheckResult DWCauthingamesncheckresult (auth/dwc_auth.h); plain enum, int-sized",
    ),
    "DWCnastimediff": (
        "long long DWCnastimediff",
        "extern s64 DWCnastimediff (NitroDWC base/src/dwc_nasfunc.c); s64 is long long, 8 bytes",
    ),
    "DWCnastimediffbase": (
        "long long DWCnastimediffbase",
        "extern s64 DWCnastimediffbase (NitroDWC base/src/dwc_nasfunc.c); s64 is long long, 8 bytes",
    ),
    "DWCnastimediffvalid": (
        "int DWCnastimediffvalid",
        "extern BOOL DWCnastimediffvalid (NitroDWC base/src/dwc_nasfunc.c); BOOL is int",
    ),
}

# Fallback for a data entry the table does not know: zeroed bytes, sized and
# aligned generously enough for any plausible scalar or small struct.
FALLBACK_DATA_SIZE = 64

HEADER = """\
/* Generated by pc/gen_stubs.py from pc/stubs.list. Do not edit.
 *
 * Weak trap stubs for symbols that are dead code for a single-player PC
 * boot. Weak on purpose: a future real implementation anywhere in the link
 * overrides a stub without touching pc/stubs.list.
 *
 * WEAK ON ELF ONLY. On PE, GCC lowers a weak DEFINITION to a weak
 * external with a default body, and mingw's ld.bfd does not resolve a
 * plain call against that form, measured 2026-08-27 with a two-file
 * hello (strong call in one object, weak definition in the other:
 * undefined reference, on two binutils vintages). Every past Windows
 * link worked off warm object trees; the first cold PE build failed on
 * all thirty-seven of these at once. Nothing overrides these stubs on
 * Windows, so there they are STRONG, and if something ever does
 * provide one, the duplicate-symbol error is the loud, honest signal to
 * come and split this per-host.
 */
#if defined(_WIN32)
#define PC_STUB_WEAK /* strong on PE; see above */
#else
#define PC_STUB_WEAK __attribute__((weak))
#endif
/*
 * Every function stub is spelled void(void) regardless of the symbol's
 * real signature. That is acceptable because a stub is only an address
 * until it is called: the linker binds calls by name, the i386 cdecl
 * caller both pushes and pops its own arguments, and the body aborts
 * before ever returning, so no argument is read and no return value is
 * produced under any real signature.
 *
 * Data stubs are transcribed to plain C types rather than #including the
 * declaring SDK headers: those headers also carry real prototypes for the
 * stubbed functions (cps.h alone declares the whole CPS_* surface), which
 * would collide with the void(void) traps in this translation unit.
 */
#include "pc_trap.h"
"""


def parse(path):
    entries = {}
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 3:
                sys.exit("%s:%d: expected SYMBOL<TAB>func|data<TAB>reason" % (path, lineno))
            sym, kind, reason = parts
            sym, kind, reason = sym.strip(), kind.strip(), reason.strip()
            if not sym or kind not in ("func", "data") or not reason:
                sys.exit("%s:%d: bad entry %r" % (path, lineno, line))
            if sym in entries:
                sys.exit("%s:%d: duplicate symbol %s" % (path, lineno, sym))
            entries[sym] = (kind, reason)
    return entries


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    argv = sys.argv[1:]
    exclude = set()
    # --exclude-from FILE: symbol names (one per line, PE's leading
    # underscore tolerated) that the LINK already defines for real. On PE
    # the stubs are strong (see the generated header), so emitting a stub
    # for a name a real object defines is a duplicate-definition error,
    # the caller derives this set from the built objects with nm, which
    # keeps stubs.list the single checked-in source and the exclusion a
    # measurement.
    if argv and argv[0] == "--exclude-from":
        with open(argv[1]) as f:
            for line in f:
                name = line.strip()
                if name.startswith("_"):
                    name = name[1:]
                if name:
                    exclude.add(name)
        argv = argv[2:]
    if len(argv) != 2:
        sys.exit("usage: gen_stubs.py [--exclude-from FILE] <stubs.list>"
                 " <out.c>")
    entries = parse(argv[0])
    dropped = sorted(set(entries) & exclude)
    for sym in dropped:
        del entries[sym]

    out = [HEADER]
    if dropped:
        out.append("/* Defined for real by this link's own objects, so no"
                   " stub is emitted: %s */" % ", ".join(dropped))
    for sym in sorted(entries):
        kind, reason = entries[sym]
        if kind == "func":
            out.append(
                "PC_STUB_WEAK void %s(void) { pc_trap_unreached(%s, %s); }"
                % (sym, cstr(sym), cstr(reason))
            )
        else:
            if sym in DATA_DECLS:
                decl, prov = DATA_DECLS[sym]
                out.append("/* %s: %s. %s */" % (sym, reason, prov))
                out.append("PC_STUB_WEAK %s;" % decl)
            else:
                out.append(
                    "/* %s: %s. No known declaration in gen_stubs.py's table; "
                    "%d zeroed bytes, 8-aligned, covers any scalar or small struct. */"
                    % (sym, reason, FALLBACK_DATA_SIZE)
                )
                out.append(
                    "PC_STUB_WEAK unsigned char %s[%d] __attribute__((aligned(8)));"
                    % (sym, FALLBACK_DATA_SIZE)
                )

    with open(argv[1], "w") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
