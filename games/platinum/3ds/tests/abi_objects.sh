#!/usr/bin/env bash
#
# 3ds/tests/abi_objects.sh: compile the game again for ARM11, for its DWARF.
#
#   3ds/tests/abi_objects.sh [build/3ds/abi]
#
# The objects are the answer to the question and nothing else: they are
# never linked and never run. `pc/abi_layout.py` reads struct layout out of
# DWARF, so the way to learn what the 3DS compiler decides about the game's
# structs is to compile the game's own translation units with it and read
# them back, rather than to write a probe file, which could disagree with
# the real compile in a way that hides exactly what is being looked for.
#
# What is held constant. Every flag comes from `pc/Makefile`'s own
# GAME_CFLAGS / SDK_CFLAGS: same headers, same include order, same defines,
# same -O1, same -std=gnu99. The only substitution is the target ABI,
# `-m32 -fno-pie` becomes the Old 3DS's `-march=armv6k -mtune=mpcore
# -mfloat-abi=hard -mtp=soft`, so a layout difference is the ABI and cannot
# be anything else. `-D__3DS__` rides along because the port's own headers
# branch on it.
#
# TWO CONCESSIONS, both to the compiler's age and neither to the ABI:
#
#   * devkitARM is GCC 16 and the host is GCC 13. Several conversions this
#     decompilation makes, int from void *, a call to something not yet
#     declared, became errors by default in GCC 14. They are downgraded
#     back to warnings here. Nothing about them moves a struct member.
#   * `tools/armrec/strip_asm.py` runs first, the same as the PC build, so a
#     file with mwcc inline asm in it compiles instead of being skipped.
#
# A file that still does not compile is skipped and counted. This is a survey
# of struct layout, not a build: nothing depends on every file landing.

set -u

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root" || exit 2

out=${1:-build/3ds/abi}
jobs=${JOBS:-$(nproc)}

if [ -z "${DEVKITARM:-}" ]; then
    echo "abi_objects: DEVKITARM is not set. source \$HOME/devkitpro/3ds-env.sh" >&2
    exit 2
fi

pc_obj=build/pc/obj/game
if [ ! -d "$pc_obj" ]; then
    echo "abi_objects: no $pc_obj, run make -f pc/Makefile first" >&2
    exit 2
fi

mkdir -p "$out"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# pc/Makefile is the single source of the flags; ask it rather than copy them.
cat > "$tmp/flags.mk" <<'MK'
include pc/Makefile
printflags:
	@echo "GAME<$(GAME_CFLAGS)>"
	@echo "SDK<$(SDK_CFLAGS)>"
MK
flags=$(make -f "$tmp/flags.mk" printflags 2>/dev/null)
game_flags=$(printf '%s\n' "$flags" | sed -n 's/^GAME<\(.*\)>$/\1/p')
sdk_flags=$(printf '%s\n' "$flags" | sed -n 's/^SDK<\(.*\)>$/\1/p')
if [ -z "$game_flags" ] || [ -z "$sdk_flags" ]; then
    echo "abi_objects: could not read the flags out of pc/Makefile" >&2
    exit 2
fi

ARM='-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -D__3DS__'
# The diagnostics GCC 14 promoted from warnings to errors, asked of the 3DS
# build rather than repeated here: it compiles the same tree with the same
# compiler and a copy would drift the moment one list moved.
PERMIT=$(make -f "$root/3ds/Makefile" permflags 2>/dev/null)
if [ -z "$PERMIT" ]; then
    echo "abi_objects: 3ds/Makefile did not report GAMEPERM" >&2
    exit 2
fi

# EXTRA is how a candidate answer is tried: EXTRA=-fno-short-enums, and so
# on. It goes last so it wins. PRE goes first, which is the only way to try a
# header shadow, -I directories are searched in the order they are given and
# pc/include is already first.
EXTRA=${EXTRA:-}
PRE=${PRE:-}

# MODE=answer, the default, is the measured result: the three things that
# have to be true for ARM11 to lay the game's structs out the way the DS
# compiler did.
#
#   -fno-short-enums    devkitARM defaults to short enums, so an enum member
#                       is one byte and every struct after it slides. 147 of
#                       the 251 disagreements were this.
#   u64/s64 4-aligned   AAPCS aligns 8-byte types to 8; the DS aligned them
#                       to 4. An `aligned(4)` on the typedef is enough, and
#                       unlike -fpack-struct=4 it leaves bitfields alone,
#                       that flag repacks `u32 x:20` and moves nine save
#                       structures, UndergroundRecord from 60 bytes to 40.
#   _USE_LONG_TIME_T    newlib's time_t is 64-bit here and 32-bit on the DS.
#
# The types.h shadow is generated from pc/include's, not copied, so it cannot
# drift from it. MODE=stock measures the ABI as devkitARM ships it.
MODE=${MODE:-answer}

if [ "$MODE" = "answer" ]; then
    mkdir -p "$tmp/shadow/nitro"
    sed -e 's/^typedef uint64_t u64;$/typedef uint64_t __attribute__((aligned(4))) u64;/' \
        -e 's/^typedef int64_t s64;$/typedef int64_t __attribute__((aligned(4))) s64;/' \
        pc/include/nitro/types.h > "$tmp/shadow/nitro/types.h"
    if ! grep -q 'aligned(4))) u64' "$tmp/shadow/nitro/types.h" \
       || ! grep -q 'aligned(4))) s64' "$tmp/shadow/nitro/types.h"; then
        echo "abi_objects: pc/include/nitro/types.h no longer declares u64/s64" \
             "the way this expects, fix the sed above rather than trust it" >&2
        exit 2
    fi
    PRE="-I$tmp/shadow $PRE"
    EXTRA="-fno-short-enums -D_USE_LONG_TIME_T $EXTRA"
fi

retarget() { printf '%s' "$1" | sed 's/-m32 -fno-pie/'"$ARM"'/'; }
GAME_ARM="$PRE $(retarget "$game_flags") $EXTRA"
SDK_ARM="$PRE $(retarget "$sdk_flags") $EXTRA"

export GAME_ARM SDK_ARM PERMIT out root

one() {
    src=$1
    obj=$out/${src%.c}.o
    mkdir -p "$(dirname "$obj")"

    use=$GAME_ARM
    case $src in
        subprojects/*) use=$SDK_ARM ;;
    esac

    # Same transform the PC build applies, and the same fallback if it
    # declines the file.
    cc_src=$src
    if python3 tools/armrec/strip_asm.py "$src" "$obj.stripped.c" 2> /dev/null; then
        cc_src=$obj.stripped.c
    fi

    if arm-none-eabi-gcc $use $PERMIT -c "$cc_src" -o "$obj" 2> "$obj.log"; then
        rm -f "$obj.log" "$obj.stripped.c"
        echo "ok $src"
    else
        rm -f "$obj" "$obj.stripped.c"
        echo "SKIP $src"
    fi
}
export -f one

# The translation units are exactly the ones the PC build compiled, so the
# two DWARF sets are drawn from the same list.
find "$pc_obj" -name '*.o' \
    | sed "s|^$pc_obj/||; s|\.o$|.c|" \
    | sort > "$tmp/sources"

total=$(wc -l < "$tmp/sources")
echo "abi_objects: $total translation units, -j$jobs"

xargs -a "$tmp/sources" -P "$jobs" -I{} bash -c 'one "$@"' _ {} > "$tmp/result"

built=$(grep -c '^ok ' "$tmp/result")
skipped=$(grep -c '^SKIP ' "$tmp/result")
grep '^SKIP ' "$tmp/result" | sed 's/^SKIP /  skipped: /' | sort > "$out/skipped.txt"
echo "abi_objects: $built objects in $out, $skipped skipped (listed in $out/skipped.txt)"
[ "$built" -gt 0 ]
