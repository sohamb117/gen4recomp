#!/usr/bin/env bash
# tests/bwhgss/parity.sh [GAME...]: the feature-parity checks Black, White,
# HeartGold and SoulSilver (default all four) can pass today, headless, with
# np_headless from $NP_BW_CORE (Black/White core) and $NP_HGSS_CORE
# (HeartGold/SoulSilver core) and the ROMs in $NP_BLACK_ROM, $NP_WHITE_ROM,
# $NP_HG_ROM, $NP_SS_ROM.
#
# Black / White:
#   title     boot to the title: ROM-derived music (audio rms), snapshot round
#             trips (--state-test) at the title; the same title with
#             bgm_volume 0 (the music silent) and se_volume 0 (the music
#             kept): B/W's music players are 0 and 6 (pc/src/pc_bw_snd.c)
#   save      bw-save.sched: NEW GAME to the controllable bedroom, snapshot
#             round trips there, then the X menu SAVE: the game's own save,
#             which np_save5 must verify (both copies, every CRC); again
#             with text_instant from just before the menu: its "save?"
#             message whole at once, and the save still made
#   continue  bw-continue.sched from that save: title, CONTINUE, the bedroom
#   quicksave the same with an F1 quick save in the bedroom (quicksave_seq):
#             saved, the player free again, the file changed and verified
#   camera    the bedroom at camera_zoom 512 / camera_tilt 160 against the
#             plain one, and back at the defaults byte-identical to it
#   mods      bw-menu.sched: the main menu with tests/bwhgss/bw_mod_example.py's
#             package (made from the ROM) in the ROM view: CONTINUE (MOD),
#             the rest of the menu as without it
#   battle    bw-battle.sched: the gift box and Bianca's battle set in_battle,
#             the field after it clears it; PC_NP_RULES_CHECK there: the fix-
#             bugs rule (the 0 damage glitch) off and on
# HeartGold / SoulSilver:
#   intro     hgss-intro.sched: title, the touch-screen tutorial driven by
#             stylus taps, Prof. Oak, the boy, the default name accepted;
#             snapshot round trips at the title and in Oak's introduction;
#             the ROM's music; Oak's first page with and without instant text
#   title     the title screen's music with bgm_volume 0 (silent) and
#             se_volume 0 (kept): HG/SS's music players are Platinum's
#   field     with $NP_HG_FIELD_SAVE / $NP_SS_FIELD_SAVE, a save at New Bark
#             Town's west exit (else SKIPped): CONTINUE into the field
#             (hgss-field.sched); render scale 2 and widescreen frame sizes;
#             camera zoom / tilt change the field and leave nothing behind;
#             an F1 quick save (np_save4 verifies it); the game's own save
#             from the touch menu (hgss-save.sched), np_save4 verifies it and
#             CONTINUE comes back; the main menu with hgss_mod_example.py's
#             package (made from the ROM) in the ROM view: CONTINUE (MOD), the
#             rest unchanged; PC_NP_RULES_CHECK's Rage fix, off and on;
#             hgss-wild.sched: a wild battle in Route 29's grass sets
#             in_battle, RUN clears it
#
# Frames are dumped as PNGs to build/evidence/bwhgss/<game>/ (outside git).
# Exit status: 0 every check passed (games without core or ROM are SKIPped),
# 1 a check failed, 2 usage.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
here=tests/bwhgss
save5=${NP_SAVE5:-build/features/np_save5}
save4=${NP_SAVE4:-build/features/np_save4}
games=("$@")
[ ${#games[@]} -eq 0 ] && games=(black white heartgold soulsilver)

fail=0
check() { # NAME LOG PATTERN...: every pattern must occur in LOG
    local name=$1 log=$2 p
    shift 2
    for p in "$@"; do
        if ! grep -q -- "$p" "$log"; then
            echo "FAIL $name: no \"$p\" (log $log)"
            fail=1
            return 1
        fi
    done
    echo "ok   $name"
}
# The state test's rounds all replayed identically, and the run ended well.
state_ok() { # NAME LOG ROUNDS
    local n
    n=$(grep -c "^state round .* ok$" "$2")
    if [ "$n" -ne "$3" ] || grep -q "^FAILED\|mismatch" "$2"; then
        echo "FAIL $1: $n of $3 snapshot rounds replayed identically (log $2)"
        fail=1
        return 1
    fi
    echo "ok   $1 ($n snapshot round trips)"
}
loud() { # NAME LOG: nonzero audio from the ROM's sound data
    local l
    l=$(sed -n 's/^audio rms from frame [0-9]*: L \([0-9]*\).*/\1/p' "$2")
    if [ -z "$l" ] || [ "$l" -lt 100 ]; then
        echo "FAIL $1: audio rms \"$l\" (log $2)"
        fail=1
        return 1
    fi
    echo "ok   $1 (audio rms L $l)"
}
quiet() { # NAME LOG_DEFAULT LOG_MUTED PERCENT: LOG_MUTED's rms below PERCENT
    # of LOG_DEFAULT's (PERCENT > 0), or at least -PERCENT of it (< 0)
    local d m
    d=$(sed -n 's/^audio rms from frame [0-9]*: L \([0-9]*\).*/\1/p' "$2")
    m=$(sed -n 's/^audio rms from frame [0-9]*: L \([0-9]*\).*/\1/p' "$3")
    if [ -z "$d" ] || [ -z "$m" ] || [ "$d" -lt 100 ] \
        || { [ "$4" -gt 0 ] && [ $((m * 100)) -ge $((d * $4)) ]; } \
        || { [ "$4" -lt 0 ] && [ $((m * 100)) -lt $((d * -$4)) ]; }; then
        echo "FAIL $1: audio rms $m against $d (logs $2, $3)"
        fail=1
        return 1
    fi
    echo "ok   $1 (audio rms L $d -> $m)"
}
size() { # NAME PNG W H: the frame (both screens stacked) is W x H
    local d
    d=$(sips -g pixelWidth -g pixelHeight "$2" 2>/dev/null | awk '/pixelWidth/ {w=$2} /pixelHeight/ {h=$2} END {print w "x" h}')
    if [ "$d" != "$3x$4" ]; then
        echo "FAIL $1: $2 is \"$d\", not $3x$4"
        fail=1
        return 1
    fi
    echo "ok   $1 ($d)"
}
pngs() { # DIR: the dumped frames as PNGs
    local f
    for f in "$1"/*.ppm; do
        [ -f "$f" ] || continue
        sips -s format png "$f" --out "${f%.ppm}.png" > /dev/null 2>&1 && rm -f "$f"
    done
}
instant() { # NAME STEP_OFF STEP_ON A B X0 Y0 X1 Y1: in the box (the
    # message's text, not its blinking arrow) a page still printing between
    # frames A and B without the option, already whole (A == B) with it
    local off=$w/$2 on=$w/$3 n=$1 a=$4 b=$5
    shift 5
    if python3 $here/region_same.py "$off/frame_$a.png" "$off/frame_$b.png" "$@"; then
        echo "FAIL $n: without instant text frames $a and $b show the same text ($off)"
        fail=1
    elif ! python3 $here/region_same.py "$on/frame_$a.png" "$on/frame_$b.png" "$@"; then
        echo "FAIL $n: with instant text frames $a and $b show different text ($on)"
        fail=1
    else
        echo "ok   $n (frame $a: the page printing, whole at once with text_instant)"
    fi
}

for g in "${games[@]}"; do
    case $g in
    black) hl=${NP_BW_CORE:-}/np_headless rom=${NP_BLACK_ROM:-} ;;
    white) hl=${NP_BW_CORE:-}/np_headless rom=${NP_WHITE_ROM:-} ;;
    heartgold) hl=${NP_HGSS_CORE:-}/np_headless rom=${NP_HG_ROM:-} ;;
    soulsilver) hl=${NP_HGSS_CORE:-}/np_headless rom=${NP_SS_ROM:-} ;;
    *) echo "parity: unknown game $g" >&2; exit 2 ;;
    esac
    [ -n "${NP_HEADLESS:-}" ] && hl=$NP_HEADLESS
    if [ ! -x "$hl" ] || [ ! -f "$rom" ]; then
        echo "SKIP $g (core $hl, ROM '$rom')"
        continue
    fi
    w=build/evidence/bwhgss/$g
    rm -rf "$w" && mkdir -p "$w"
    run() { # STEP ARGS...: one np_headless run, its frames in $w/<step>/
        local step=$1
        shift
        mkdir -p "$w/$step"
        "$hl" $g "$rom" --dump "$w/$step" "$@" > "$w/$step.log" 2>&1
        echo "exit $?" >> "$w/$step.log"
        pngs "$w/$step"
    }
    case $g in
    black | white)
        run title --frames 5400 --dump-from 4800 --dump-every 600 --rms-from 4000 \
            --state-test 4800 --state-span 120 --state-rounds 3
        check "$g title" "$w/title.log" "exit 0"
        state_ok "$g title snapshots" "$w/title.log" 3
        loud "$g title music" "$w/title.log"
        run title-bgm0 --frames 5400 --rms-from 4000 -o bgm_volume=0
        quiet "$g title, bgm_volume 0" "$w/title.log" "$w/title-bgm0.log" 10
        run title-se0 --frames 5400 --rms-from 4000 -o se_volume=0
        quiet "$g title, se_volume 0" "$w/title.log" "$w/title-se0.log" -90

        run save --frames 23200 --schedule $here/bw-save.sched --save "$w/game.sav" \
            --dump-from 21410 --dump-every 15 --state-test 21500 --state-span 120 --state-rounds 3
        check "$g new game to the bedroom, in-game save" "$w/save.log" "exit 0"
        state_ok "$g bedroom snapshots" "$w/save.log" 3
        if [ -x "$save5" ]; then
            "$save5" verify "$w/game.sav" > "$w/verify.log" 2>&1
            echo "exit $?" >> "$w/verify.log"
            check "$g save verifies (np_save5)" "$w/verify.log" "exit 0" "all checksums valid"
            "$save5" dump "$w/game.sav" > "$w/dump.json" 2>&1
        else
            echo "note $g: no $save5 (set NP_SAVE5); save not verified"
        fi

        # The X menu's "Would you like to save the game?" prints from ~22097
        # to ~22147; with text_instant it is whole (and YES/NO open) by 22101.
        run save-text --frames 23200 --schedule $here/bw-save.sched --save "$w/game-text.sav" \
            --dump-from 22100 --dump-every 15 -o 22090:text_instant=1
        check "$g in-game save with instant text" "$w/save-text.log" "exit 0"
        instant "$g instant text (the save prompt)" save save-text 022101 022116 4 198 244 228
        if [ -x "$save5" ]; then
            "$save5" verify "$w/game-text.sav" > "$w/verify-text.log" 2>&1
            echo "exit $?" >> "$w/verify-text.log"
            check "$g save with instant text verifies" "$w/verify-text.log" "exit 0" "all checksums valid"
        fi

        cp "$w/game.sav" "$w/continue.sav"
        run continue --frames 7000 --schedule $here/bw-continue.sched --save "$w/continue.sav" \
            --dump-from 5000 --dump-every 1000
        check "$g CONTINUE to the bedroom" "$w/continue.log" "exit 0"

        # F1 in the bedroom (free from ~5300): the game's save, ~230 frames.
        cp "$w/game.sav" "$w/quicksave.sav"
        run quicksave --frames 7000 --schedule $here/bw-continue.sched --save "$w/quicksave.sav" \
            --dump-from 5600 --dump-every 200 -o 5600:quicksave_seq=1
        check "$g F1 quick save in the bedroom" "$w/quicksave.log" "exit 0" \
            "pc-np: quick save 1: saved" "field_ready=1 quicksave_seq=1 quicksave_result=1"
        if cmp -s "$w/game.sav" "$w/quicksave.sav"; then
            echo "FAIL $g quick save: the save file did not change"
            fail=1
        fi
        if [ -x "$save5" ]; then
            "$save5" verify "$w/quicksave.sav" > "$w/verify-quicksave.log" 2>&1
            echo "exit $?" >> "$w/verify-quicksave.log"
            check "$g quick save verifies (np_save5)" "$w/verify-quicksave.log" "exit 0" "all checksums valid"
        fi

        # Camera zoom and tilt (pc/src/pc_bw_camera.c) on the bedroom's 3D,
        # and nothing of the game's camera written: back at the defaults
        # the frame is the plain one.
        for step in camera-plain camera camera-back; do
            cp "$w/game.sav" "$w/$step.sav"
        done
        run camera-plain --frames 7000 --schedule $here/bw-continue.sched --save "$w/camera-plain.sav" \
            --dump-from 6999
        run camera --frames 7000 --schedule $here/bw-continue.sched --save "$w/camera.sav" \
            --dump-from 6999 -o 6000:camera_zoom=512 -o 6000:camera_tilt=160
        run camera-back --frames 7000 --schedule $here/bw-continue.sched --save "$w/camera-back.sav" \
            --dump-from 6999 -o 6000:camera_zoom=512 -o 6000:camera_tilt=160 \
            -o 6990:camera_zoom=256 -o 6990:camera_tilt=0
        if python3 $here/region_same.py "$w/camera-plain/frame_007000.png" "$w/camera/frame_007000.png" \
            0 0 256 192; then
            echo "FAIL $g camera zoom/tilt: the bedroom looks the same ($w/camera)"
            fail=1
        elif ! cmp -s "$w/camera-plain/frame_007000.png" "$w/camera-back/frame_007000.png"; then
            echo "FAIL $g camera back at the defaults: not the plain bedroom ($w/camera-back)"
            fail=1
        else
            echo "ok   $g camera zoom 512 / tilt 10 degrees in the bedroom, back to the plain picture at the defaults"
        fi

        # Content packages as a ROM view (pc/src/pc_bw_romview.c): the
        # example package (tests/bwhgss/bw_mod_example.py, made from this
        # ROM) replaces the main menu's bank; its CONTINUE reads CONTINUE
        # (MOD), and the rest of the menu is the plain one.
        python3 $here/bw_mod_example.py "$rom" "$w/mods" > /dev/null
        for step in menu-plain menu-mod; do
            cp "$w/game.sav" "$w/$step.sav"
        done
        run menu-plain --frames 5501 --schedule $here/bw-menu.sched --save "$w/menu-plain.sav" --dump-from 5500
        run menu-mod --frames 5501 --schedule $here/bw-menu.sched --save "$w/menu-mod.sav" --dump-from 5500 \
            --content "$w/mods" -e PC_MODS=example_bw_menu
        check "$g the example package in the ROM view" "$w/menu-mod.log" "exit 0" \
            "pc_bw_romview: a/0/0/2: .* members" "pc_bw_romview: 1 files moved"
        if python3 $here/region_same.py "$w/menu-plain/frame_005501.png" "$w/menu-mod/frame_005501.png" \
            30 18 220 34; then
            echo "FAIL $g example package: the CONTINUE label is the plain one ($w/menu-mod)"
            fail=1
        elif ! python3 $here/region_same.py "$w/menu-plain/frame_005501.png" "$w/menu-mod/frame_005501.png" \
            30 130 220 192; then
            echo "FAIL $g example package: the rest of the menu changed ($w/menu-mod)"
            fail=1
        else
            echo "ok   $g example package: CONTINUE (MOD) on the main menu, the rest of it unchanged"
        fi

        # NP_STAT_IN_BATTLE: the gift box, then Bianca's battle (overlay 93's
        # POKECON, pc/src/pc_bw_e2e.c) sets it; the field after it clears it.
        cp "$w/game.sav" "$w/battle.sav"
        # PC_NP_RULES_CHECK: on the battle's first frame the fix-bugs hook
        # (pc/src/pc_bw_rules.c) runs on the game's own fx multiply, bit off
        # and on.
        run battle --frames 15000 --schedule $here/bw-battle.sched --save "$w/battle.sav" \
            --dump-from 9000 --dump-every 1000 -e PC_NP_RULES_CHECK=1
        check "$g Bianca's battle sets in_battle, the field after it clears it" "$w/battle.log" "exit 0" \
            "in_battle 0 -> 1" "in_battle 1 -> 0"
        check "$g fix-bugs rule: the 0 damage glitch with the bit off, fixed with it on" "$w/battle.log" \
            "rules check: 1 damage x other 1/2: off 0, on 1" "rules check: PASS"
        ;;
    heartgold | soulsilver)
        run intro --frames 13900 --schedule $here/hgss-intro.sched --dump-from 1600 --dump-every 1500 \
            --rms-from 1300 --state-test 6000 --state-span 120 --state-rounds 3
        check "$g title, touch tutorial, Oak, naming" "$w/intro.log" "exit 0"
        state_ok "$g intro snapshots" "$w/intro.log" 3
        loud "$g intro music" "$w/intro.log"
        run title --frames 1700 --state-test 1400 --state-span 120 --state-rounds 2 --dump-from 1700 \
            --rms-from 1300
        state_ok "$g title snapshots" "$w/title.log" 2
        loud "$g title music" "$w/title.log"
        run title-bgm0 --frames 1700 --rms-from 1300 -o bgm_volume=0
        quiet "$g title, bgm_volume 0" "$w/title.log" "$w/title-bgm0.log" 10
        run title-se0 --frames 1700 --rms-from 1300 -o se_volume=0
        quiet "$g title, se_volume 0" "$w/title.log" "$w/title-se0.log" -90
        # Oak's first page ("Huh? It's already become so bright outside!")
        # starts printing at ~7283; text_instant from 7284 fills it at once.
        run text-off --frames 7302 --schedule $here/hgss-intro.sched --dump-from 7284 --dump-every 16
        run text-on --frames 7302 --schedule $here/hgss-intro.sched --dump-from 7284 --dump-every 16 \
            -o 7284:text_instant=1
        instant "$g instant text (Oak)" text-off text-on 007285 007301 8 150 236 186

        # The field, from a save at New Bark Town's west exit ($NP_HG_FIELD_SAVE
        # / $NP_SS_FIELD_SAVE, e.g. tests/e2e heartgold 02's start save).
        [ $g = heartgold ] && fsave=${NP_HG_FIELD_SAVE:-} || fsave=${NP_SS_FIELD_SAVE:-}
        if [ ! -f "$fsave" ]; then
            echo "SKIP $g field checks (no field save; set NP_HG_FIELD_SAVE / NP_SS_FIELD_SAVE)"
            continue
        fi
        field() { # STEP ARGS...: CONTINUE into the field on a copy of the save
            local step=$1
            shift
            cp "$fsave" "$w/$step.sav"
            run "$step" --save "$w/$step.sav" "$@"
        }
        field field --frames 2400 --schedule $here/hgss-field.sched --dump-from 2399
        check "$g CONTINUE into the field" "$w/field.log" "exit 0" "field_ready=1 .* map_id=60"
        field field-scale2 --frames 2400 --schedule $here/hgss-field.sched --dump-from 2399 -o render_scale=2
        size "$g field at render scale 2" "$w/field-scale2/frame_002400.png" 512 768
        field field-wide --frames 2400 --schedule $here/hgss-field.sched --dump-from 2399 -o widescreen=1
        size "$g field in widescreen" "$w/field-wide/frame_002400.png" 342 384
        # Camera zoom and tilt on the field's 3D only, and nothing of the game's
        # camera written: back at the defaults the frame is the plain one.
        field field-camera --frames 2400 --schedule $here/hgss-field.sched --dump-from 2399 \
            -o 2300:camera_zoom=512 -o 2300:camera_tilt=160
        field field-camera-back --frames 2400 --schedule $here/hgss-field.sched --dump-from 2399 \
            -o 2300:camera_zoom=512 -o 2300:camera_tilt=160 -o 2390:camera_zoom=256 -o 2390:camera_tilt=0
        if python3 $here/region_same.py "$w/field/frame_002400.png" "$w/field-camera/frame_002400.png" 0 0 256 192; then
            echo "FAIL $g camera zoom/tilt: the field looks the same ($w/field-camera)"
            fail=1
        elif ! cmp -s "$w/field/frame_002400.png" "$w/field-camera-back/frame_002400.png"; then
            echo "FAIL $g camera back at the defaults: not the plain field ($w/field-camera-back)"
            fail=1
        else
            echo "ok   $g camera zoom 512 / tilt 10 degrees in the field, back to the plain picture at the defaults"
        fi
        field field-quicksave --frames 2400 --schedule $here/hgss-field.sched -o 2300:quicksave_seq=1
        check "$g F1 quick save in the field" "$w/field-quicksave.log" "exit 0" \
            "pc-np: quick save 1: saved" "field_ready=1 quicksave_seq=1 quicksave_result=1"
        if cmp -s "$fsave" "$w/field-quicksave.sav"; then
            echo "FAIL $g quick save: the save file did not change"
            fail=1
        fi
        if [ -x "$save4" ]; then
            "$save4" verify "$w/field-quicksave.sav" > "$w/verify-quicksave.log" 2>&1
            echo "exit $?" >> "$w/verify-quicksave.log"
            check "$g quick save verifies (np_save4)" "$w/verify-quicksave.log" "exit 0" "all checksums valid"
        fi
        field save --frames 3700 --schedule $here/hgss-save.sched
        check "$g the game's own save from the touch menu" "$w/save.log" "exit 0" "stored" \
            "field_ready=1 .* map_id=60"
        if [ -x "$save4" ]; then
            "$save4" verify "$w/save.sav" > "$w/verify-save.log" 2>&1
            echo "exit $?" >> "$w/verify-save.log"
            check "$g its save verifies (np_save4)" "$w/verify-save.log" "exit 0" "all checksums valid"
        fi
        cp "$w/save.sav" "$w/save-continue.sav"
        run save-continue --frames 2400 --schedule $here/hgss-field.sched --save "$w/save-continue.sav"
        check "$g CONTINUE from that save" "$w/save-continue.log" "exit 0" "field_ready=1 .* map_id=60"
        # Content packages as a ROM view (games/ndsrec/pc/src/pc_bw_romview.c,
        # linked by HG/SS too): tests/bwhgss/hgss_mod_example.py's package
        # (made from this ROM) replaces the main menu's text bank (a/0/2/7
        # member 442); its CONTINUE reads CONTINUE (MOD), the rest of the menu
        # is the plain one.
        python3 $here/hgss_mod_example.py "$rom" "$w/mods" > /dev/null
        field menu-plain --frames 2060 --schedule $here/hgss-menu.sched --dump-from 2060
        field menu-mod --frames 2060 --schedule $here/hgss-menu.sched --dump-from 2060 \
            --content "$w/mods" -e PC_MODS=example_hgss_menu
        check "$g the example package in the ROM view" "$w/menu-mod.log" "exit 0" \
            "pc_bw_romview: a/0/2/7: .* members" "pc_bw_romview: 1 files moved"
        if python3 $here/region_same.py "$w/menu-plain/frame_002060.png" "$w/menu-mod/frame_002060.png" \
            20 198 140 216; then
            echo "FAIL $g example package: the CONTINUE label is the plain one ($w/menu-mod)"
            fail=1
        elif ! python3 $here/region_same.py "$w/menu-plain/frame_002060.png" "$w/menu-mod/frame_002060.png" \
            20 216 214 384; then
            echo "FAIL $g example package: the rest of the menu changed ($w/menu-mod)"
            fail=1
        else
            echo "ok   $g example package: CONTINUE (MOD) on the main menu, the rest of it unchanged"
        fi

        # PC_NP_RULES_CHECK: the fix-bugs rule's Rage case through the patched
        # before-turn pass, with the bit off (the cartridge's bug) and on.
        field rules --frames 2300 --schedule $here/hgss-field.sched -e PC_NP_RULES_CHECK=1
        check "$g fix-bugs rule: Rage (PC_NP_RULES_CHECK)" "$w/rules.log" "exit 0" \
            "status2 off 0x800000, on 0x1" "pc-np: rules check: PASS"

        field wild --frames 9000 --schedule $here/hgss-wild.sched --dump-from 3000 --dump-every 250
        check "$g NP_STAT_IN_BATTLE: a wild battle on Route 29, RUN, back in the field" "$w/wild.log" \
            "exit 0" "in_battle 0 -> 1" "in_battle 1 -> 0" "field_ready=1 .* in_battle=0"
        ;;
    esac
done
exit $fail
