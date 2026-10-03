#!/bin/sh
# pc/wasm/check_arm7ns.sh OBJ MAP
#
# The wasm build renames the ARM7 sound driver's symbols with a force-
# included `#define sym arm7_sym` map where ELF uses objcopy --redefine-syms.
# A macro can miss what objcopy cannot (a name formed by token pasting, a
# declaration the preprocessor never sees), so the object is checked against
# the map: every symbol it defines or references must either carry the arm7_
# prefix or be one the map deliberately leaves alone (pc/Makefile's keep-list:
# __*, mem{cpy,set,move,cmp}, pc_spu_keyon_note). Anything else is a leak
# between the two processors' namespaces and fails the build.
set -e
obj=$1
map=$2
NM=${NM:-nm}
bad=$($NM "$obj" | awk 'NF >= 2 { print $NF }' | sort -u | \
      grep -vE '^(arm7_.*|__.*|mem(cpy|set|move|cmp)|pc_spu_keyon_note)$' || true)
if [ -n "$bad" ]; then
    echo "check_arm7ns: $obj has symbols outside the arm7_ namespace:" >&2
    echo "$bad" | sed 's/^/  /' >&2
    exit 1
fi
exit 0
