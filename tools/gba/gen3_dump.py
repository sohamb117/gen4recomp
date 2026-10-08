#!/usr/bin/env python3
"""Ruby/Sapphire/Emerald save and ROM data as JSON, in np_save4's shapes.

    tools/gba/gen3_dump.py dump ROM SAV [--elf ELF]
        The save's newest slot (features/tools/np_save4.c cmd_dump's keys):
        game, load_result, rom, slot, save_counter,
        trainer {name, tid, sid, gender, money, coins, badges, badge_mask,
                 play_time "h:mm:ss", national_dex},
        location {map (group << 8 | num), x, y, z (= y), continue_warp, saved {map, x, y}}
            (the CONTINUE location: the continue-game warp when its flag is set),
        flags [set flag ids, system flags included],
        vars {"<decimal var id>": value} (nonzero, ids from VARS_START 0x4000),
        party [{slot (1-based), species, species_name, nickname, is_egg, checksum_ok,
                level, exp, pid "0x%08X", shiny, nature, ability {id, num},
                held_item {id, name}, moves [{id, name, pp, pp_ups}], ivs, evs
                (both HP ATK DEF SPEED SPATK SPDEF), friendship, ot_name, tid, sid,
                met_location {id}, met_level, met_game, ball {id}, ot_gender,
                language, pokerus, hp, stats [maxhp, atk, def, spe, spa, spd], status}],
        bag {items, key_items, balls, tms_hms, berries: [{item, name, qty}], registered},
        pokedex {seen, caught, obtained, national, seen_list, caught_list (species ids)},
        hall_of_fame {total, latest: null | {party: [{species, species_name, level, nickname}]}},
        game_stats [GAME_STAT_* values by id].

    tools/gba/gen3_dump.py gamedata ROM [--elf ELF]
        np_save4 gamedata's shape (tests/e2e/bots.py auto_battle):
        species[i] = [type1, type2, ability1, ability2, hp, atk, def, spe, spa, spd, growth],
        moves[i] = [effect, class, power, type, accuracy, pp, priority, range],
        type_chart[atk][def] (x10), exp_table[growth][level].

The ROM's game code picks the game; its tables are read at the addresses of the
decomp's ELF (.cache/gba/<decomp>/<game>.elf unless --elf). See tools/gba/gen3.py.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen3  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["dump", "gamedata"])
    ap.add_argument("rom")
    ap.add_argument("sav", nargs="?")
    ap.add_argument("--elf", help="the decomp ELF matching the ROM")
    a = ap.parse_args()
    rom = gen3.Rom(a.rom, a.elf)
    if a.cmd == "dump":
        if not a.sav:
            ap.error("dump needs ROM SAV")
        with open(a.sav, "rb") as f:
            sav = gen3.Save(f.read(), rom.game)
        print(gen3.to_json(gen3.dump(rom, sav)))
    else:
        if a.sav:
            ap.error("gamedata takes only the ROM")
        print(gen3.to_json(gen3.gamedata(rom)))


if __name__ == "__main__":
    main()
