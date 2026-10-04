#!/bin/sh
# mint.sh RECIPE OUT.sav: mint a save from a gameplay recipe with the real core.
# The lab boots a new game, clears the bedroom TV text with A (the save lab's
# own lab-settle input), applies the recipe at frame 1800, saves and exits.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
rom=${NP_ROM:-$root/games/platinum/build/rom/pokeplatinum.us.nds}
gp=${NP_GP:-$root/build/gameplay/np_gp}
settle="inline:$(awk 'BEGIN { for (f = 120; f < 1500; f += 40) printf "%s%d keys A;%d keys none", (f > 120 ? ";" : ""), f, f + 12 }')"
rm -f "$2"
PC_INPUT="$settle" PC_LAB="$(python3 "$here/labc.py" "$1")" PC_LAB_AT=1800 \
    "$gp" "$rom" --frames 6000 --save "$2" >"$2.log" 2>&1
grep -q 'pc_lab: applied .* save ok' "$2.log" || { cat "$2.log"; echo "mint.sh: minting $1 failed" >&2; exit 1; }
