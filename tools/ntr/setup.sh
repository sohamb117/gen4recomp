#!/usr/bin/env bash
# Stand in for the proprietary pieces the pret DS decomps' INSTALL.md files ask
# for (pret's mwccarm.zip and NitroSDK tools/bin), using only open tools plus
# the metroskrew compilers Platinum's build fetched.
#
#   tools/ntr/setup.sh diamond      (pokediamond: Diamond and Pearl)
#   tools/ntr/setup.sh heartgold    (pokeheartgold: HeartGold and SoulSilver)
#
# Run inside the romtools container (tools/rom_build.sh does this). Everything
# written lands in paths the game's .gitignore already ignores:
#
#   tools/mwccarm/<ver>/mw{cc,ld,asm}arm.exe  -> metroskrew relinked binaries
#   tools/bin/makelcf.exe                     <- NitroSDK makelcf (ntrtwl source)
#   tools/bin/makerom.exe                     -> tools/ntr/makerom.py
#   tools/bin/makebanner.exe                  -> tools/ntr/makebanner.py
#   tools/bin/ntrcomp.exe                     -> tools/ntr/ntrcomp.sh (nitrogfx LZ)
#   SDK specfiles (lcf templates, mwldarm response template) <- tools/ntr/specfiles/
#   heartgold only: tools/mwasmarm_patcher/mwasmarm_patcher -> no-op (see below)
#   diamond only: tools/bin/rom_header.template.sbin <- makerom.py --header-template
#                 (pokeheartgold commits its own <build>/rom_header_template.sbin)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SKREW="$ROOT/games/platinum/tools/metroskrew"
NITROSDK="$ROOT/games/platinum/subprojects/NitroSDK-4.2.30001"
SPEC="$HERE/specfiles"

GAME="${1:-}"
case "$GAME" in
    diamond)
        # pokediamond links against NitroSDK 3.2, whose makelcf defaults the
        # IRQ stack to 0x400 (4.x: 0x800); the value reaches arm9.sbin through
        # SDK_IRQ_STACKSIZE in crt0/OS_arena.
        IRQ_STACK=0x400
        SPECFILES="ARM9-TS.lcf.template:arm9/ ARM7-TS.lcf.template:arm7/"
        HEADER_TEMPLATE=tools/bin/rom_header.template.sbin ;;
    heartgold)
        # pokeheartgold uses NitroSDK 4.2 (071210), the version ntrtwl's
        # makelcf sources reproduce, so its defaults stand.
        IRQ_STACK=0x800
        SPECFILES="ARM9-TS.lcf.template:./ ARM7-TS.lcf.template:sub/ mwldarm.response.template:./"
        HEADER_TEMPLATE= ;;
    *)
        echo "usage: $0 diamond|heartgold" >&2
        exit 2 ;;
esac
DEST="$ROOT/games/$GAME"
BIN="$DEST/tools/bin"

[ -x "$SKREW/bin/skrewrap" ] || { echo "setup: metroskrew missing at $SKREW (run Platinum's ROM build once)" >&2; exit 1; }

install_if_changed() { # tmpfile, dest
    if cmp -s "$1" "$2"; then rm -f "$1"; else mv "$1" "$2"; fi
}

wrapper() { # path, command...
    local path="$1"; shift
    mkdir -p "$(dirname "$path")"
    printf '#!/bin/sh\nexec %s "$@"\n' "$*" > "$path.tmp"
    chmod +x "$path.tmp"
    install_if_changed "$path.tmp" "$path"
}

# CodeWarrior for NITRO release -> mwccarm / mwldarm / mwasmarm builds, as
# metroskrew's share/metroskrew/sdk/ds/<ver>/<tool>.exe.txt maps them.
for dir in "$SKREW"/share/metroskrew/sdk/ds/*/*/; do
    ver="${dir%/}"; ver="${ver#"$SKREW/share/metroskrew/sdk/ds/"}"
    for tool in mwccarm mwldarm mwasmarm; do
        [ -f "$dir/$tool.exe.txt" ] || continue
        wrapper "$DEST/tools/mwccarm/$ver/$tool.exe" "$SKREW/bin/$(tr -d '[:space:]' < "$dir/$tool.exe.txt")"
    done
done

mkdir -p "$BIN"

# makelcf from ntrtwl's decompiled NitroSDK 4.2 sources, with the game's SDK
# default IRQ stack size.
LCF_BUILD="$DEST/build/nativeplat/makelcf"
LCF_SRC="$NITROSDK/tools/makelcf"
if [ ! -x "$BIN/makelcf.exe" ] || [ "$LCF_SRC/createlcf.c" -nt "$BIN/makelcf.exe" ] || [ "$0" -nt "$BIN/makelcf.exe" ]; then
    rm -rf "$LCF_BUILD"
    mkdir -p "$LCF_BUILD"
    cp "$LCF_SRC"/*.[chly] "$LCF_BUILD/"
    sed -i "s/^#define[[:space:]]*DEFAULT_IRQSTACKSIZE[[:space:]]*0x800\$/#define DEFAULT_IRQSTACKSIZE $IRQ_STACK/" "$LCF_BUILD/makelcf.h"
    grep -q "DEFAULT_IRQSTACKSIZE[[:space:]]*$IRQ_STACK\$" "$LCF_BUILD/makelcf.h" || { echo "setup: makelcf.h IRQ stack default not $IRQ_STACK" >&2; exit 1; }
    for g in spec tlcf; do
        flex --nowarn -P${g}_yy -o "$LCF_BUILD/$g.yy.c" "$LCF_BUILD/$g.l"
        bison -d -p ${g}_yy -o "$LCF_BUILD/$g.tab.c" "$LCF_BUILD/$g.y"
    done
    gcc -O2 -w -DSDK_WIN32 -I"$LCF_BUILD" \
        -I"$NITROSDK/include" -I"$NITROSDK/include/nitro" \
        "$LCF_BUILD"/{container,createlcf,defval,makelcf,misc}.c \
        "$LCF_BUILD"/{spec,tlcf}.{yy,tab}.c -o "$BIN/makelcf.exe"
fi

wrapper "$BIN/makerom.exe" python3 "$HERE/makerom.py"
wrapper "$BIN/makebanner.exe" python3 "$HERE/makebanner.py"
wrapper "$BIN/ntrcomp.exe" env NITROGFX="$DEST/tools/nitrogfx/nitrogfx" "$HERE/ntrcomp.sh"
if [ -n "$HEADER_TEMPLATE" ]; then
    python3 "$HERE/makerom.py" --header-template "$DEST/$HEADER_TEMPLATE.tmp"
    install_if_changed "$DEST/$HEADER_TEMPLATE.tmp" "$DEST/$HEADER_TEMPLATE"
fi

# pokeheartgold's `patch_mwasmarm` step (run by every sub-make's `all`) patches
# pret's mwasmarm.exe for its line-ending and 0x400-byte .incbin bugs, and
# rejects any other file -- such as the metroskrew wrapper above. metroskrew's
# assemblers need no binary patch (the ROM matches without one; pokediamond's
# build skips it via MWASMARM_PATCHER=true), so stand in a no-op, newer than
# mwasmarm_patcher.c so its Makefile never rebuilds the real one.
if [ "$GAME" = heartgold ]; then
    wrapper "$DEST/tools/mwasmarm_patcher/mwasmarm_patcher" true
    touch "$DEST/tools/mwasmarm_patcher/mwasmarm_patcher"
fi

# The ARM7 template's sub-processor private arena is Diamond's (3.2):
# MAIN's bss end plus the size of the autoload pokediamond calls EXT. HG/SS
# name that autoload EXT_WRAM, and pokeheartgold's common.mk appends its
# size after `} > check.WORKRAM` itself (SDK 4.2's template starts from
# MAIN's bss end alone), so the HG/SS copy keeps only the MAIN term; with
# Diamond's line the link fails on SDK_AUTOLOAD.EXT.* not found.
for f in $SPECFILES; do
    src="$SPEC/${f%%:*}" dst="$DEST/${f#*:}${f%%:*}"
    if [ "$GAME" = heartgold ] && [ "${f%%:*}" = ARM7-TS.lcf.template ]; then
        sed 's/^\([[:space:]]*SDK_SUBPRIV_ARENA_LO = SDK_AUTOLOAD\.MAIN\.BSS_END\) + SDK_AUTOLOAD\.EXT\.BSS_END - SDK_AUTOLOAD\.EXT\.START;/\1;/' \
            "$src" > "$dst.tmp"
        grep -q 'SDK_SUBPRIV_ARENA_LO = SDK_AUTOLOAD.MAIN.BSS_END;' "$dst.tmp" ||
            { echo "setup: ARM7 template's SDK_SUBPRIV_ARENA_LO line not found" >&2; exit 1; }
        install_if_changed "$dst.tmp" "$dst"
    else
        cp -p "$src" "$dst"
    fi
done
