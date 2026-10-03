#!/usr/bin/env bash
# Stand in for the proprietary pieces pokediamond's INSTALL.md asks for, using
# only open tools plus the metroskrew compilers Platinum's build fetched.
# Run inside the romtools container (tools/rom_build.sh does this). Everything
# written here lands in paths games/diamond/.gitignore already ignores:
#
#   tools/mwccarm/<ver>/mw{cc,ld,asm}arm.exe  -> metroskrew relinked binaries
#   tools/bin/makelcf.exe                     <- NitroSDK makelcf (ntrtwl source)
#   tools/bin/makerom.exe                     -> tools/diamond/makerom.py
#   tools/bin/ntrcomp.exe                     -> tools/diamond/ntrcomp.sh (nitrogfx LZ)
#   tools/bin/rom_header.template.sbin        <- makerom.py --header-template
#   arm9/ARM9-TS.lcf.template, arm7/ARM7-TS.lcf.template <- tools/diamond/
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DIAMOND="$ROOT/games/diamond"
SKREW="$ROOT/games/platinum/tools/metroskrew/bin"
NITROSDK="$ROOT/games/platinum/subprojects/NitroSDK-4.2.30001"

[ -x "$SKREW/skrewrap" ] || { echo "setup: metroskrew missing at $SKREW (run Platinum's ROM build once)" >&2; exit 1; }

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

# CodeWarrior for NITRO release -> mwccarm / mwldarm / mwasmarm builds
# (the same table as metroskrew's share/metroskrew/sdk/ds/<ver>/*.txt).
while read -r ver cc ld as; do
    for tool in "mwccarm $cc" "mwldarm $ld" "mwasmarm $as"; do
        set -- $tool
        wrapper "$DIAMOND/tools/mwccarm/$ver/$1.exe" "$SKREW/$1-$2.exe"
    done
done <<'EOF'
1.2/sp2p3 2.0-82  2.0-76 1.0-19
1.2/sp3   2.0-84  2.0-77 1.0-19
2.0/base  3.0-114 2.0-82 1.0-20
2.0/sp1   3.0-123 2.0-84 1.0-23
EOF

BIN="$DIAMOND/tools/bin"
mkdir -p "$BIN"

# makelcf from ntrtwl's decompiled NitroSDK 4.2 sources. The only behavioural
# difference from the 3.2 tool that pokediamond's link depends on is the
# default IRQ stack size (SDK 3.x: 0x400, 4.x: 0x800), which reaches
# arm9.sbin through SDK_IRQ_STACKSIZE in crt0/OS_arena.
LCF_BUILD="$DIAMOND/build/nativeplat/makelcf"
LCF_SRC="$NITROSDK/tools/makelcf"
if [ ! -x "$BIN/makelcf.exe" ] || [ "$LCF_SRC/createlcf.c" -nt "$BIN/makelcf.exe" ] || [ "$0" -nt "$BIN/makelcf.exe" ]; then
    rm -rf "$LCF_BUILD"
    mkdir -p "$LCF_BUILD"
    cp "$LCF_SRC"/*.[chly] "$LCF_BUILD/"
    sed -i 's/^#define[[:space:]]*DEFAULT_IRQSTACKSIZE[[:space:]]*0x800$/#define DEFAULT_IRQSTACKSIZE 0x400/' "$LCF_BUILD/makelcf.h"
    grep -q 'DEFAULT_IRQSTACKSIZE 0x400$' "$LCF_BUILD/makelcf.h" || { echo "setup: makelcf.h default not patched" >&2; exit 1; }
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
wrapper "$BIN/ntrcomp.exe" "$HERE/ntrcomp.sh"
python3 "$HERE/makerom.py" --header-template "$BIN/rom_header.template.sbin.tmp"
install_if_changed "$BIN/rom_header.template.sbin.tmp" "$BIN/rom_header.template.sbin"

cp -p "$HERE/ARM9-TS.lcf.template" "$DIAMOND/arm9/ARM9-TS.lcf.template"
cp -p "$HERE/ARM7-TS.lcf.template" "$DIAMOND/arm7/ARM7-TS.lcf.template"
