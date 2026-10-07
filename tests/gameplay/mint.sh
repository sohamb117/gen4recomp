#!/bin/sh
# mint.sh RECIPE OUT.sav: mint a save from a gameplay recipe with the real core.
#
# Platinum (NP_GAME unset or platinum): the lab boots a new game, clears the
# bedroom TV text with A (the save lab's own lab-settle input), applies the
# recipe at frame 1800, saves and exits.
#
# Diamond/Pearl: D's lab (games/diamond/pc/game/pc_dp_lab.c) works on a save
# being continued, so the base is the game's new-game save, NP_BASE_SAVE
# (run.sh makes it with tests/dp/<game>_first_save.sched); the run CONTINUEs
# it (dp/schedules/continue.press) and the recipe applies once the player is
# free in the field from frame 1800.
#
# A recipe `clock YYYY-MM-DD HH:MM:SS` line becomes the run's PC_RTC (labc.py
# --clock), so the save is minted at that time.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
game=${NP_GAME:-platinum}
rom=${NP_ROM:-$root/games/platinum/build/rom/pokeplatinum.us.nds}
gp=${NP_GP:-$root/build/gameplay/np_gp}
rtc=$(python3 "$here/labc.py" --game "$game" --clock "$1")
if [ -n "$rtc" ]; then export PC_RTC="$rtc"; fi
rm -f "$2"
case $game in
platinum)
    settle="inline:$(awk 'BEGIN { for (f = 120; f < 1500; f += 40) printf "%s%d keys A;%d keys none", (f > 120 ? ";" : ""), f, f + 12 }')"
    PC_INPUT="$settle" PC_LAB="$(python3 "$here/labc.py" --game platinum "$1")" PC_LAB_AT=1800 \
        "$gp" "$rom" --frames 6000 --save "$2" >"$2.log" 2>&1
    ;;
diamond | pearl)
    [ -f "${NP_BASE_SAVE:-}" ] || { echo "mint.sh: NP_BASE_SAVE (the new-game save) is not set or missing" >&2; exit 1; }
    cp "$NP_BASE_SAVE" "$2"
    PC_LAB="$(python3 "$here/labc.py" --game "$game" "$1")" PC_LAB_AT=1800 \
        "$gp" "$rom" --game "$game" --frames 9000 --save "$2" --schedule "$here/dp/schedules/continue.press" \
        >"$2.log" 2>&1 || true
    ;;
*) echo "mint.sh: unknown NP_GAME $game" >&2; exit 2 ;;
esac
grep -q 'pc_lab: applied .* save ok' "$2.log" || { cat "$2.log"; echo "mint.sh: minting $1 failed" >&2; exit 1; }
