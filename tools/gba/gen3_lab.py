#!/usr/bin/env python3
"""Apply a save-lab recipe to a Ruby/Sapphire/Emerald save, host-side.

    tools/gba/gen3_lab.py ROM IN.sav OUT.sav RECIPE [--elf ELF]

RECIPE is a recipe file (compiled by tests/gameplay/labc.py with the ROM's
game: names from the decomp's headers) or `inline:VERB ARGS;...` (numbers, as
labc compiles them; names are resolved the same way). Python:
gen3_lab.apply(rom, in_sav, out_sav, recipe), rom a path or gen3.Rom.

The verbs are Diamond/Pearl's save lab's (games/diamond/pc/game/pc_dp_lab.c),
same names and argument order, applied to the newest save slot the way the
game's own code would leave it:

  name TEXT            the player's name (7 characters)
  gender G             0 male, 1 female
  trainer-id N         the 32-bit OT id (TID | SID << 16)
  money N              0..999999 (Emerald: XORed with SaveBlock2's encryptionKey)
  badge N              1..8: FLAG_BADGE0N_GET
  var V N / flag F / clear-flag F
  party SPECIES LEVEL ITEM
                       appended, owned by the save's trainer: a fixed non-shiny
                       personality, IVs 20, the species' level-up moves at that
                       level (GiveBoxMonInitialMoveset), stats from the gen 3
                       formulas, HP and PP full, marked seen and caught in the
                       Pokedex (ScriptGiveMon)
  party-move SLOT MOVESLOT MOVE   (PP full, that slot's PP Ups cleared)
  party-level SLOT LEVEL          (exp = the growth table's base for LEVEL, stats
                                   recomputed, HP full; no evolution, no moves)
  party-item SLOT ITEM
  party-iv SLOT STAT 0..31, party-ev SLOT STAT 0..255
                       STAT 0..5 = HP ATK DEF SPEED SPATK SPDEF; stats recomputed, HP full
  item ITEM QTY        into the pocket gItems names, stacked as AddBagItem stacks
                       (Emerald: quantities XORed with the encryption key)
  register-item ITEM   the SELECT item
  pokedex 0|1          FLAG_SYS_POKEDEX_GET
  national-dex 0|1     EnableNationalPokedex / DisableNationalPokedex (each decomp's
                       src/event_data.c: SaveBlock2 nationalMagic 0xDA, VAR_NATIONAL_DEX
                       0x302, FLAG_SYS_NATIONAL_DEX, dex mode national)
  dex-seen SPECIES / dex-caught SPECIES
  map MAP X Z          the CONTINUE location: SaveBlock1's continue-game warp
                       (map, warp -1, X, Z) and its flag in SaveBlock2, so CONTINUE
                       warps there as after a link room (tools/gba/gen3_warp.py)
  clock ...            ignored here (labc turns it into the run's PC_RTC)

Anything else, or a bad argument, stops with an error and writes nothing.
Every changed SaveBlock sector of the newest slot gets its checksum
recomputed; the save counter and the sector rotation stay, so the game loads
the same slot.
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tests", "gameplay"))
import gen3  # noqa: E402

# verb -> argument count (None: the rest of the line is text)
VERBS = {
    "name": None, "gender": 1, "trainer-id": 1, "money": 1, "badge": 1, "var": 2, "flag": 1,
    "clear-flag": 1, "party": 3, "party-move": 3, "party-level": 2, "party-item": 2, "party-iv": 3,
    "party-ev": 3, "item": 2, "register-item": 1, "pokedex": 1, "national-dex": 1, "dex-seen": 1,
    "dex-caught": 1, "map": 3,
}
MAIL_NONE = 0xFF        # pokeemerald include/constants/items.h:448; pokeruby src/pokemon_1.c:1346 (CreateMon)
MAX_MONEY = 999999      # pokeemerald src/money.c:13 (R/S: the same cap in src/money.c)
PARTY_IVS = 20
NAME_BYTES = 8          # playerName[PLAYER_NAME_LENGTH + 1]


class Lab:
    def __init__(self, rom, sav):
        self.rom, self.sav, self.game = rom, sav, rom.game
        self.where = ""

    def fail(self, msg):
        raise SystemExit("gen3_lab: %s: %s" % (self.where, msg))

    def need(self, ok, msg):
        if not ok:
            self.fail(msg)

    def const(self, name):
        return gen3.const(self.game, name)

    def species(self, sp):
        self.need(0 < sp < self.const("NUM_SPECIES") and sp < len(self.rom.species_info()),
                  "species %d is not a species" % sp)
        return sp

    def move(self, mv):
        self.need(0 < mv < len(self.rom.moves()), "move %d is not a move" % mv)
        return mv

    def item(self, it, allow_none=False):
        self.need((allow_none and it == 0) or 0 < it < len(self.rom.items()), "item %d is not an item" % it)
        return it

    def slot(self, i):
        self.need(0 <= i < self.sav.party_count(), "party slot %d is empty (party of %d)" % (i, self.sav.party_count()))
        return self.sav.party_mon(i)

    def nat(self, sp):
        n = self.rom.national_dex()[self.species(sp)]
        self.need(n, "species %d has no national dex number" % sp)
        return n

    # -- verbs
    def v_name(self, text):
        self.need(text, "name needs TEXT")
        self.sav.sb2[gen3.SB2_NAME:gen3.SB2_NAME + NAME_BYTES] = gen3.encode_text(text, NAME_BYTES - 1, self.game) + b"\xff"

    def v_gender(self, g):
        self.need(g in (0, 1), "gender is 0 (male) or 1 (female)")
        self.sav.sb2[gen3.SB2_GENDER] = g

    def v_trainer_id(self, n):
        self.need(0 <= n <= 0xFFFFFFFF, "trainer id is 32 bits")
        struct.pack_into("<I", self.sav.sb2, gen3.SB2_TRAINER_ID, n)

    def v_money(self, n):
        self.need(0 <= n <= MAX_MONEY, "money is 0..%d" % MAX_MONEY)
        self.sav.u32_enc(gen3.SB1_MONEY, n)

    def v_badge(self, n):
        self.need(1 <= n <= 8, "badge is 1..8")
        self.sav.flag(self.const("FLAG_BADGE0%d_GET" % n), True)

    def v_var(self, v, n):
        self.need(0 <= n <= 0xFFFF, "var value is 16 bits")
        self.sav.var(v, n)

    def v_flag(self, f):
        self.sav.flag(f, True)

    def v_clear_flag(self, f):
        self.sav.flag(f, False)

    def v_party(self, sp, level, it):
        sav, rom, g = self.sav, self.rom, self.game
        n = sav.party_count()
        self.need(n < gen3.PARTY_MAX, "the party is full")
        info = rom.species_info()[self.species(sp)]
        self.need(1 <= level <= self.const("MAX_LEVEL"), "level is 1..%d" % self.const("MAX_LEVEL"))
        self.item(it, allow_none=True)
        # CreateBoxMon (pokeemerald src/pokemon.c:2206-2303, pokeruby src/pokemon_1.c:1376):
        # the player as OT, the species name, the growth table's exp, base friendship,
        # met here at this level in a Poke Ball, the second ability when personality is odd.
        otid = sav.trainer_id
        pid = 0x4E500000 | sp << 8 | level
        while ((otid >> 16) ^ (otid & 0xFFFF) ^ (pid >> 16) ^ (pid & 0xFFFF)) < 8:  # never shiny
            pid += 1
        mon = gen3.Mon.blank(g)
        mon.personality, mon.ot_id = pid, otid
        name_addr = rom.sym("gSpeciesNames")[0] + gen3.SPECIES_NAME_SIZE * sp
        name = bytes(rom.at(name_addr, gen3.NAME_LEN))
        cut = name.find(b"\xff")
        mon.head[8:18] = (name if cut < 0 else name[:cut] + b"\xff" * (gen3.NAME_LEN - cut))
        mon.head[18] = self.const("GAME_LANGUAGE")
        mon.head[20:27] = sav.sb2[gen3.SB2_NAME:gen3.SB2_NAME + gen3.OT_LEN]
        mon.species, mon.held_item = sp, it
        mon.exp = rom.exp_table()[info["growth"]][level]
        mon.subs[0][9] = info["friendship"]
        lg, ln = struct.unpack_from("<BB", sav.sb1, gen3.SB1_LOCATION)
        mon.subs[3][1] = rom.map_section(lg << 8 | ln)
        origin = level | self.const("GAME_VERSION") << 7 | self.const("ITEM_POKE_BALL") << 11 | (sav.sb2[gen3.SB2_GENDER] & 1) << 15
        struct.pack_into("<H", mon.subs[3], 2, origin)
        for s in range(6):
            mon.set_iv(s, PARTY_IVS)
        if info["abilities"][1]:
            mon._set_iv_word(mon._get_iv_word() | (pid & 1) << 31)
        # GiveBoxMonInitialMoveset (pokeemerald src/pokemon.c:2991-3012): every move up to the
        # level, a known one skipped, the first dropped when four are known.
        moves = []
        for lv, mv in rom.learnset(sp):
            if lv > level:
                break
            if mv in moves:
                continue
            if len(moves) == 4:
                moves.pop(0)
            moves.append(mv)
        for i in range(4):
            mv = moves[i] if i < len(moves) else 0
            mon.set_move(i, mv, rom.moves()[mv]["pp"] if mv else 0)
        mon.party = bytearray(20)
        mon.party[5] = MAIL_NONE
        mon.recalc_stats(rom, level)
        sav.set_party_mon(n, mon)
        sav.sb1[gen3.SB1_PARTY_COUNT] = n + 1
        # ScriptGiveMon (pokeemerald src/script_pokemon_util.c:72-81): seen and caught
        sav.dex_set(self.nat(sp), caught=True)

    def v_party_move(self, i, ms, mv):
        mon = self.slot(i)
        self.need(0 <= ms < 4, "move slot is 0..3")
        mon.set_move(ms, self.move(mv), self.rom.moves()[mv]["pp"])  # SetMonMoveSlot (pokemon.c:2974-2978)
        self.sav.set_party_mon(i, mon)

    def v_party_level(self, i, level):
        mon = self.slot(i)
        self.need(1 <= level <= self.const("MAX_LEVEL"), "level is 1..%d" % self.const("MAX_LEVEL"))
        mon.exp = self.rom.exp_table()[self.rom.species_info()[mon.species]["growth"]][level]
        mon.recalc_stats(self.rom, level)
        self.sav.set_party_mon(i, mon)

    def v_party_item(self, i, it):
        mon = self.slot(i)
        mon.held_item = self.item(it, allow_none=True)
        self.sav.set_party_mon(i, mon)

    def v_party_iv(self, i, stat, v):
        mon = self.slot(i)
        self.need(0 <= stat <= 5 and 0 <= v <= 31, "party-iv wants STAT 0..5 and a value 0..31")
        mon.set_iv(stat, v)
        mon.recalc_stats(self.rom)
        self.sav.set_party_mon(i, mon)

    def v_party_ev(self, i, stat, v):
        mon = self.slot(i)
        self.need(0 <= stat <= 5 and 0 <= v <= 255, "party-ev wants STAT 0..5 and a value 0..255")
        mon.set_ev(stat, v)
        mon.recalc_stats(self.rom)
        self.sav.set_party_mon(i, mon)

    def v_item(self, it, qty):
        """AddBagItem (pokeemerald src/item.c, pokeruby src/item.c): fill the item's
        stacks to the pocket's capacity (MAX_BERRY_CAPACITY for berries, else
        MAX_BAG_ITEM_CAPACITY), then empty slots."""
        self.item(it)
        self.need(qty > 0, "quantity must be at least 1")
        pocket = gen3.POCKET_NAMES.get(self.rom.items()[it]["pocket"])
        self.need(pocket, "item %d belongs to no bag pocket" % it)
        cap = self.const("MAX_BERRY_CAPACITY" if pocket == "berries" else "MAX_BAG_ITEM_CAPACITY")
        off, n = self.sav.pocket(pocket)
        left = qty
        for want_empty in (False, True):
            for s in range(n):
                o = off + 4 * s
                cur = struct.unpack_from("<H", self.sav.sb1, o)[0]
                if left == 0 or cur != (0 if want_empty else it):
                    continue
                have = self.sav.u16_enc(o + 2) if cur else 0
                add = min(cap - have, left)
                if add <= 0:
                    continue
                struct.pack_into("<H", self.sav.sb1, o, it)
                self.sav.u16_enc(o + 2, have + add)
                left -= add
        self.need(left == 0, "the %s pocket has no room for %d more" % (pocket, left))

    def v_register_item(self, it):
        struct.pack_into("<H", self.sav.sb1, gen3.SB1_REGISTERED, self.item(it, allow_none=True))

    def v_pokedex(self, on):
        self.need(on in (0, 1), "pokedex is 0 or 1")
        self.sav.flag(self.const("FLAG_SYS_POKEDEX_GET"), bool(on))

    def v_national_dex(self, on):
        self.need(on in (0, 1), "national-dex is 0 or 1")
        sb2 = self.sav.sb2
        sb2[gen3.DEX_MAGIC] = gen3.NATIONAL_MAGIC if on else 0
        self.sav.var(self.const("VAR_NATIONAL_DEX"), gen3.NATIONAL_VAR_VALUE if on else 0)
        self.sav.flag(self.const("FLAG_SYS_NATIONAL_DEX"), bool(on))
        if on:
            sb2[gen3.DEX_MODE], sb2[gen3.DEX_ORDER] = gen3.DEX_MODE_NATIONAL, 0

    def v_dex_seen(self, sp):
        self.sav.dex_set(self.nat(sp), caught=False)

    def v_dex_caught(self, sp):
        self.sav.dex_set(self.nat(sp), caught=True)

    def v_map(self, map_id, x, z):
        maps = {v for k, v in gen3.decomp_constants(self.game).items() if k.startswith("MAP_")}
        self.need(map_id in maps and map_id >> 8 < self.const("MAP_GROUPS_COUNT"),
                  "map %d is not one of %s's MAP_* maps" % (map_id, gen3.GAMES[self.game]["decomp"]))
        self.need(-0x8000 <= x < 0x8000 and -0x8000 <= z < 0x8000, "coordinates are s16")
        # WarpData continueGameWarp: s8 mapGroup, s8 mapNum, s8 warpId, pad, s16 x, s16 y
        struct.pack_into("<BBbxhh", self.sav.sb1, gen3.SB1_CONTINUE_WARP, map_id >> 8, map_id & 0xFF, -1, x, z)
        self.sav.sb2[gen3.SB2_SPECIAL_WARP] |= gen3.CONTINUE_GAME_WARP


def parse(recipe, game):
    """[(where, verb, args)]: a file through labc, or inline:VERB ARGS;..."""
    import labc  # tests/gameplay/labc.py
    if recipe.startswith("inline:"):
        lines = [(("inline line %d" % i), ln) for i, ln in enumerate(recipe[len("inline:"):].split(";"), 1)]
    else:
        if not os.path.exists(recipe):
            raise SystemExit("gen3_lab: no recipe file %s" % recipe)
        inline, _env = labc.compile_recipe(recipe, game)
        lines = [(("%s line %d" % (recipe, i)), ln) for i, ln in enumerate(inline[len("inline:"):].split(";"), 1)]
    resolve = labc.make_resolver(game)
    ops = []
    for where, line in lines:
        line = line.strip()
        if not line:
            continue
        verb, _, rest = line.partition(" ")
        if verb == "clock":
            continue
        if verb not in VERBS:
            raise SystemExit("gen3_lab: %s: unknown verb %r (verbs: %s)" % (where, verb, " ".join(sorted(VERBS))))
        if VERBS[verb] is None:
            ops.append((where, verb, [rest.strip()]))
            continue
        args = rest.split()
        if len(args) != VERBS[verb]:
            raise SystemExit("gen3_lab: %s: %s takes %d argument(s), got %d" % (where, verb, VERBS[verb], len(args)))
        ops.append((where, verb, [resolve(a, where) for a in args]))
    return ops


def apply(rom, in_sav, out_sav, recipe):
    if not isinstance(rom, gen3.Rom):
        rom = gen3.Rom(rom)
    with open(in_sav, "rb") as f:
        sav = gen3.Save(f.read(), rom.game)
    lab = Lab(rom, sav)
    ops = parse(recipe, rom.game)
    for where, verb, args in ops:
        lab.where = where
        try:
            getattr(lab, "v_" + verb.replace("-", "_"))(*args)
        except SystemExit as e:
            msg = str(e)
            raise SystemExit(msg if msg.startswith("gen3_lab:") else "gen3_lab: %s: %s" % (where, msg))
    with open(out_sav, "wb") as f:
        f.write(sav.to_bytes())
    return ops


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("recipe")
    ap.add_argument("--elf", help="the decomp ELF matching the ROM")
    a = ap.parse_args()
    rom = gen3.Rom(a.rom, a.elf)
    ops = apply(rom, a.src, a.out, a.recipe)
    print("%s: %s, %d line(s): %s" % (a.out, rom.game, len(ops), "; ".join(
        " ".join([v] + [str(x) for x in args]) for _w, v, args in ops)))


if __name__ == "__main__":
    main()
