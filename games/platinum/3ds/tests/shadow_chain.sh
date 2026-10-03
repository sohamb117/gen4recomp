#!/usr/bin/env bash
#
# 3ds/tests/shadow_chain.sh: prove the shadows are actually shadowing.
#
# Reading the -I list tells you the order make INTENDED. This asks the
# compiler which file it opened. `gcc -H` prints the include tree on stderr,
# one line per header, `.` per level of depth followed by the resolved path,
# so the answer is the compiler's own and not an argument about search rules.
#
# The invariant. For every header opened while compiling a game translation
# unit: if a file of the same relative name exists under 3ds/include or
# pc/include, the file that was opened must BE one of those. A shadow that is
# second on the search path is not a shadow, and the failure mode is silent,
# the SDK header compiles fine, it just describes the DS instead of this port.
#
#   3ds/tests/shadow_chain.sh          one line per shadowed name, then a count
#   VERBOSE=1 3ds/tests/shadow_chain.sh    every header the probe opened
#
# Exit 0 if every shadowed name resolved to a shadow, 1 otherwise, 2 if the
# probe would not compile at all.

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cd "$root" || exit 2

if [ -z "${DEVKITARM:-}" ]; then
    echo "shadow_chain: DEVKITARM is not set. source \$HOME/devkitpro/3ds-env.sh" >&2
    exit 2
fi

PROBE=3ds/tests/shadow_probe.c

# The flags the build really uses, asked of the build rather than repeated
# here. Repeating them is how a check ends up passing against a compile line
# that no longer exists.
flags=$(make -f 3ds/Makefile shadowflags 2>/dev/null)
if [ -z "$flags" ]; then
    echo "shadow_chain: 3ds/Makefile did not report GAME_CFLAGS" >&2
    exit 2
fi

# Every relative name the two shadow directories claim: "nitro/types.h",
# "nitro/hw/ARM9/ioreg_G3.h", and so on. A name claimed by neither is not this
# check's business, most of the tree is not shadowed and never will be.
claimed=$(
    for d in 3ds/include pc/include; do
        [ -d "$d" ] && (cd "$d" && find . -type f -name '*.h' | sed 's|^\./||')
    done | sort -u
)

# -H writes the tree to stderr and the (empty) object to nowhere.
trace=$($DEVKITARM/bin/arm-none-eabi-gcc -H -fsyntax-only $flags "$PROBE" 2>&1 >/dev/null)
rc=$?
if [ $rc -ne 0 ]; then
    echo "shadow_chain: the probe does not compile" >&2
    echo "$trace" | grep -v '^\.* ' | head -20 >&2
    exit 2
fi

# Header lines look like "... /abs/path/to/header.h", one dot per level of
# nesting. Multiple-include notes and the trailing summary do not start with a
# dot. Order is kept, and it is the whole method: a shadow opens the file it
# shadows with #include_next, so the SDK's copy appears too, one level deeper
# and immediately after. What decides the question is which of them the
# ORIGINAL #include reached; that is the first line naming that file.
opened=$(echo "$trace" | sed -n 's/^\.\{1,\} //p')

declare -A first
checked=0
bad=0
for path in $opened; do
    for name in $claimed; do
        case $path in
            */$name)
                if [ -z "${first[$name]:-}" ]; then
                    first[$name]=$path
                fi
                ;;
        esac
    done
done

for name in "${!first[@]}"; do
    path=${first[$name]}
    checked=$((checked + 1))
    case $path in
        "$root"/3ds/include/*|"$root"/pc/include/*)
            [ "${VERBOSE:-0}" = 1 ] &&
                printf '  %-40s %s\n' "$name" "${path#$root/}"
            ;;
        *)
            bad=$((bad + 1))
            printf '  %-40s NOT SHADOWED: %s\n' "$name" "${path#$root/}"
            ;;
    esac
done

if [ "${VERBOSE:-0}" = 1 ]; then
    echo "$opened" | sort -u | sed "s|^$root/|  |;s|^/|  /|"
fi

# And the line decision 16 draws: a game translation unit must not see
# libctru. The include chain leaves it out, but leaving something out of a
# list is not the same as it staying out, a shadow that includes <3ds.h> for
# convenience would put it back and nothing else would notice until an enum
# crossed the boundary at a size the other side did not expect.
ctru=$(echo "$opened" | grep -c "/libctru/" || true)
if [ "$ctru" -ne 0 ]; then
    echo "shadow_chain: the game chain opened $ctru libctru header(s):"
    echo "$opened" | grep "/libctru/" | head -5
    exit 1
fi

if [ "$bad" -ne 0 ]; then
    echo "shadow_chain: $bad name(s) resolved past the shadow directories"
    exit 1
fi

# A zero here is a pass by accident: it means the probe included nothing that
# either directory shadows, so the check proved nothing. Say so.
if [ "$checked" -eq 0 ]; then
    echo "shadow_chain: the probe opened no shadowed header"
    exit 1
fi

echo "shadow_chain: $checked shadowed name(s) resolved to a shadow, no libctru"
