#!/usr/bin/env python3
"""Ruby/Sapphire/Emerald save and ROM access for the story harness (library).

    import gen3
    rom = gen3.Rom("pokeemerald.gba")             # game from the header code at 0xAC
    sav = gen3.Save(open("x.sav", "rb").read(), rom.game)
    party = gen3.decode_party(raw, count, rom.game, rom)   # raw = gPlayerParty bytes (RAM or save)

Every layout comes from the decomps (pret/pokeemerald 731ad5b, pret/pokeruby
5784633 in .cache/gba); file:line references below are to those trees. The
ROM's tables (species, moves, type chart, learnsets, items, names) are read
from the ROM bytes at the addresses of the decomp's own ELF symbol table;
nothing of the ROM is copied anywhere. Constants (flags, vars, species ...)
come from the decomp's include/constants/*.h, parsed by decomp_constants().
"""
import json
import os
import re
import struct

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
GBA_CACHE = os.path.join(REPO, ".cache", "gba")

# ROM header game code at 0xAC (GBA cartridge header); the decomp dir and ELF
# that built it (tools/gbabuild.py GAMES). `macro` is the -D the pokeruby
# build defines per version (pokeruby Makefile: -DRUBY / -DSAPPHIRE).
GAMES = {
    "emerald": dict(code=b"BPEE", decomp="pokeemerald", elf="pokeemerald.elf", macro=None),
    "ruby": dict(code=b"AXVE", decomp="pokeruby", elf="pokeruby.elf", macro="RUBY"),
    "sapphire": dict(code=b"AXPE", decomp="pokeruby", elf="pokesapphire.elf", macro="SAPPHIRE"),
}
ROM_BASE = 0x08000000

# ---- save layout ---------------------------------------------------------
# pokeemerald include/save.h:8-30 and src/save.c:44-72; pokeruby src/save.c:74-131:
# 32 flash sectors of 4 KiB, two 14-sector slots (0-13, 14-27), Hall of Fame
# in 28-29. A sector is data[3968] + footer: u16 id @0xFF4, u16 checksum
# @0xFF6, u32 signature 0x08012025 @0xFF8, u32 save counter @0xFFC.
SECTOR = 0x1000
SECTOR_DATA = 3968
SIGNATURE = 0x08012025
SLOT_SECTORS = 14
HOF_SECTOR = 28          # pokeemerald include/save.h:26 SECTOR_ID_HOF_1; pokeruby src/save.c:93
HOF_SECTORS = 2
HOF_TEAMS = 50           # pokeemerald src/hall_of_fame.c:38; pokeruby src/hall_of_fame.c:53
HOF_MON = 20             # struct HallofFameMon: u32 tid, u32 personality, u16 species:9 lvl:7, u8 nick[10]
HOF_TEAM = 6 * HOF_MON   # (pokeemerald src/hall_of_fame.c:41-53; pokeruby src/hall_of_fame.c:39-51)

# chunk id -> (block, chunk index): 0 SaveBlock2, 1-4 SaveBlock1, 5-13 PokemonStorage
# (pokeemerald src/save.c:57-72 sSaveSlotLayout; pokeruby src/save.c:108-126 sSaveBlockChunks).
CHUNKS = [("sb2", 0)] + [("sb1", i) for i in range(4)] + [("storage", i) for i in range(9)]

# Per-game offsets. SaveBlock2: pokeemerald include/global.h:508-542, pokeruby
# include/global.h:841-863 (identical up to 0xA8; only Emerald has the
# encryptionKey at 0xAC). SaveBlock1: pokeemerald include/global.h:984-1079,
# pokeruby include/global.h:668-754. Sizes: Emerald SaveBlock2 0xF2C
# (global.h:542), SaveBlock1 0x3D88 (global.h:1078); R/S SaveBlock2 0x890
# (battleTower 0xA8 + BattleTowerData 0x7E8, global.h:814-839) and
# SaveBlock1 0x3AC0 (dexSeen3 0x3A8C + DEX_FLAGS_NO 52); PokemonStorage 0x83D0
# in both (tools/gba/gen3_save.py STORAGE_SIZE; the ELFs' gPokemonStorage).
LAYOUT = {
    "emerald": dict(
        sb2_size=0xF2C, sb1_size=0x3D88, storage_size=0x83D0, encrypted=True,
        pockets=[("items", 0x560, 30), ("key_items", 0x5D8, 30), ("balls", 0x650, 16),
                 ("tms_hms", 0x690, 64), ("berries", 0x790, 46)],
        seen_a=0x988, seen_b=0x3B24,          # seen1 / seen2 (global.h:1012, 1071)
        flags=0x1270, flags_bytes=0x139C - 0x1270,  # flags[NUM_FLAG_BYTES] (global.h:1020)
        vars=0x139C, vars_count=(0x159C - 0x139C) // 2,  # vars[VARS_COUNT] (global.h:1021)
        game_stats=0x159C, game_stats_count=64,     # gameStats[NUM_GAME_STATS] (global.h:1022, game_stat.h:58)
        berry_trees=0x169C,                         # berryTrees[BERRY_TREES_COUNT] (global.h:1023)
        # secretBases[SECRET_BASES_COUNT 20] (global.h:1024; struct SecretBase global.h:556-573: 0xA0 bytes,
        # secretBaseId +0, decorations[16] +0x12, decorationPositions[16] +0x22; [0] is the player's own base)
        secret_bases=0x1A9C, secret_base_size=0xA0, secret_base_count=20,
        # the decoration inventories decorationDesks..decorationCushions (global.h:1027-1034)
        decor=[("desk", 0x2734, 10), ("chair", 0x273E, 10), ("plant", 0x2748, 10), ("ornament", 0x2752, 30),
               ("mat", 0x2770, 30), ("poster", 0x278E, 10), ("doll", 0x2798, 40), ("cushion", 0x27C0, 10)],
    ),
    "rs": dict(
        sb2_size=0x890, sb1_size=0x3AC0, storage_size=0x83D0, encrypted=False,
        pockets=[("items", 0x560, 20), ("key_items", 0x5B0, 20), ("balls", 0x600, 16),
                 ("tms_hms", 0x640, 64), ("berries", 0x740, 46)],
        seen_a=0x938, seen_b=0x3A8C,          # dexSeen2 / dexSeen3 (global.h:694, 753)
        flags=0x1220, flags_bytes=0x1340 - 0x1220,  # flags[FLAGS_COUNT] (global.h:701)
        vars=0x1340, vars_count=(0x1540 - 0x1340) // 2,  # vars[VARS_COUNT] (global.h:702)
        game_stats=0x1540, game_stats_count=50,     # gameStats[NUM_GAME_STATS] (global.h:703, game_stat.h:54)
        berry_trees=0x1608,                         # berryTrees[BERRY_TREES_COUNT] (global.h:704)
        # secretBases[SECRET_BASES_COUNT 20] (global.h:705; struct SecretBaseRecord global.h:154-169: 0xA0 bytes,
        # secretBaseId +0, decorations[16] +0x12, decorationPos[16] +0x22; [0] is the player's own base)
        secret_bases=0x1A08, secret_base_size=0xA0, secret_base_count=20,
        # the decoration inventories decorDesk..decorCushion (global.h:708-715)
        decor=[("desk", 0x26A0, 10), ("chair", 0x26AA, 10), ("plant", 0x26B4, 10), ("ornament", 0x26BE, 30),
               ("mat", 0x26DC, 30), ("poster", 0x26FA, 10), ("doll", 0x2704, 40), ("cushion", 0x272C, 10)],
    ),
}
# Common SaveBlock1 / SaveBlock2 fields (same offsets in both decomps).
SB1_POS, SB1_LOCATION, SB1_CONTINUE_WARP = 0x00, 0x04, 0x0C  # Coords16 pos; WarpData location, continueGameWarp
SB1_PARTY_COUNT, SB1_PARTY = 0x234, 0x238
SB1_MONEY, SB1_COINS, SB1_REGISTERED = 0x490, 0x494, 0x496
SB2_NAME, SB2_GENDER, SB2_SPECIAL_WARP, SB2_TRAINER_ID = 0x00, 0x08, 0x09, 0x0A
SB2_PLAY_TIME = 0x0E                                   # u16 hours, u8 minutes, u8 seconds
SB2_DEX = 0x18                                         # struct Pokedex (global.h:206-217 E, 766-777 R/S)
DEX_ORDER, DEX_MODE, DEX_MAGIC, DEX_OWNED, DEX_SEEN = 0x18, 0x19, 0x1A, 0x28, 0x5C
DEX_BYTES = 52                                         # NUM_DEX_FLAG_BYTES / DEX_FLAGS_NO
SB2_ENCRYPTION_KEY = 0xAC                              # Emerald only (global.h:532)
# struct Time {s16 days; s8 hours, minutes, seconds} (pokeruby global.h:758-764, pokeemerald 198-204): the game's
# local time is the RTC minus localTimeOffset (pokeruby src/rtc.c:293-324 RtcCalcTimeDifference, RtcCalcLocalTime)
SB2_LOCAL_TIME_OFFSET, SB2_LAST_BERRY_UPDATE = 0x98, 0xA0  # (pokeruby global.h:860-861, pokeemerald 529-530)
TIME_FMT = "<hbbb"
# struct BerryTree (pokeruby include/global.berry.h:43-61, pokeemerald 63-76): u8 berry; u8 stage:7, sparkle:1;
# u16 minutesUntilNextStage; u8 berryYield; u8 regrowthCount:4, watered1..4:1; 8 bytes
BERRY_TREE_SIZE, BERRY_TREES_COUNT = 8, 128            # include/constants/global.h BERRY_TREES_COUNT
CONTINUE_GAME_WARP = 1   # specialSaveWarpFlags bit 0 (pokeruby src/load_save.c:40-50; tools/gba/gen3_warp.py)
NATIONAL_MAGIC = 0xDA    # EnableNationalPokedex (pokeemerald src/event_data.c:63-72, pokeruby 59-68)
NATIONAL_VAR_VALUE = 0x302
DEX_MODE_NATIONAL = 1

# ---- Pokemon -------------------------------------------------------------
# struct BoxPokemon (80 bytes) / struct Pokemon (100): pokeemerald
# include/pokemon.h:99-232, pokeruby include/pokemon.h:54-168.
BOX_SIZE, PARTY_SIZE = 80, 100
PARTY_MAX = 6
# personality % 24 -> slot of substruct type 0..3 (growth, attacks, EVs, misc):
# GetSubstruct's SUBSTRUCT_CASE(n, v1, v2, v3, v4) puts type t at index v(t+1)
# (pokeemerald src/pokemon.c:3556-3633, pokeruby src/pokemon_2.c:218-270).
SUBSTRUCT_POS = [
    (0, 1, 2, 3), (0, 1, 3, 2), (0, 2, 1, 3), (0, 3, 1, 2), (0, 2, 3, 1), (0, 3, 2, 1),
    (1, 0, 2, 3), (1, 0, 3, 2), (2, 0, 1, 3), (3, 0, 1, 2), (2, 0, 3, 1), (3, 0, 2, 1),
    (1, 2, 0, 3), (1, 3, 0, 2), (2, 1, 0, 3), (3, 1, 0, 2), (2, 3, 0, 1), (3, 2, 0, 1),
    (1, 2, 3, 0), (1, 3, 2, 0), (2, 1, 3, 0), (3, 1, 2, 0), (2, 3, 1, 0), (3, 2, 1, 0),
]
NAME_LEN, OT_LEN = 10, 7          # POKEMON_NAME_LENGTH, PLAYER_NAME_LENGTH
EOS = 0xFF

# ---- ROM tables ----------------------------------------------------------
SPECIES_INFO_SIZE = 28   # struct SpeciesInfo / BaseStats (pokeemerald pokemon.h:297-325, pokeruby 277-308)
BATTLE_MOVE_SIZE = 12    # struct BattleMove (pokeemerald pokemon.h:327-338, pokeruby 310-321)
ITEM_SIZE = 44           # struct Item (pokeemerald include/item.h:10-26, pokeruby src/item.c:11-28)
ITEM_POCKET = 26         # u8 pocket after name[14], itemId, price, holdEffect(Param), description, importance, registrability
SPECIES_NAME_SIZE = 11   # gSpeciesNames[][POKEMON_NAME_LENGTH + 1]
MOVE_NAME_SIZE = 13      # gMoveNames[][MOVE_NAME_LENGTH + 1]
EXP_ROW = 101            # gExperienceTables[][MAX_LEVEL + 1]
LEVEL_UP_END = 0xFFFF    # u16 move:9 level:7 (pokeemerald pokemon.h:349-353)
MAP_HEADER_MAPSEC = 0x14  # struct MapHeader regionMapSectionId (pokeemerald global.fieldmap.h:171-179)
# POCKET_* (pokeemerald include/constants/item.h:5-10, pokeruby include/item.h:5-12) -> dump pocket name
POCKET_NAMES = {1: "items", 2: "balls", 3: "tms_hms", 4: "berries", 5: "key_items"}
TYPE_MYSTERY = 9         # IS_TYPE_PHYSICAL(t) = t < TYPE_MYSTERY (pokeemerald include/battle.h:466)
NUM_TYPES = 18           # NUMBER_OF_MON_TYPES (pokeemerald include/constants/pokemon.h:24)
TYPE_FORESIGHT, TYPE_ENDTABLE = 0xFE, 0xFF  # pokeemerald include/battle_main.h:37-38
MOVE_TARGET_BOTH, MOVE_TARGET_FOES_AND_ALLY = 0x08, 0x20  # pokeemerald include/battle.h:50-52
RANGE_ALL_ADJACENT = 0x08  # the harness's (tests/e2e/bots.py)
NATURES = ["Hardy", "Lonely", "Brave", "Adamant", "Naughty", "Bold", "Docile", "Relaxed", "Impish", "Lax",
           "Timid", "Hasty", "Serious", "Jolly", "Naive", "Modest", "Mild", "Quiet", "Bashful", "Rash",
           "Calm", "Gentle", "Sassy", "Careful", "Quirky"]  # NATURE_* order (pokeemerald constants/pokemon.h)


def layout(game):
    return LAYOUT["emerald" if game == "emerald" else "rs"]


def decomp_dir(game):
    return os.path.join(GBA_CACHE, GAMES[game]["decomp"])


def detect_game(rom_bytes):
    code = bytes(rom_bytes[0xAC:0xB0])
    for game, g in GAMES.items():
        if g["code"] == code:
            return game
    raise SystemExit("gen3: ROM game code %r is not Ruby (AXVE), Sapphire (AXPE) or Emerald (BPEE)" % code)


def checksum(data, size):
    """CalculateChecksum (pokeemerald src/save.c, pokeruby src/save.c): u32 sum, folded to 16 bits."""
    total = sum(struct.unpack_from("<%dI" % (size // 4), data)) & 0xFFFFFFFF
    return ((total >> 16) + total) & 0xFFFF


# ---- decomp constants ----------------------------------------------------
_TOKEN = re.compile(r"\s*(0[xX][0-9a-fA-F]+|\d+|[A-Za-z_]\w*|<<|>>|&&|\|\||[=!<>]=|[-+*/%&|^~!()<>?:])")
_CONST_CACHE = {}


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


class _Expr:
    """C integer constant expressions over names defined anywhere in the headers."""

    def __init__(self):
        self.raw = {}     # name -> expression text (first definition wins)
        self.done = {}
        self.busy = set()

    def value(self, name):
        if name in self.done:
            return self.done[name]
        if name not in self.raw or name in self.busy:
            raise KeyError(name)
        self.busy.add(name)
        try:
            v = self.eval(self.raw[name])
        finally:
            self.busy.discard(name)
        self.done[name] = v
        return v

    def eval(self, text, unknown_zero=False):
        out, pos = [], 0
        text = text.strip()
        while pos < len(text):
            m = _TOKEN.match(text, pos)
            if not m:
                raise KeyError(text)
            tok, pos = m.group(1), m.end()
            if tok[0].isdigit():
                out.append(str(int(tok, 0)))
                pos += len(re.match(r"[uUlL]*", text[pos:]).group(0))
            elif tok[0].isalpha() or tok[0] == "_":
                try:
                    out.append(str(self.value(tok)))
                except KeyError:
                    if not unknown_zero:
                        raise
                    out.append("0")
            else:
                out.append({"&&": " and ", "||": " or ", "/": "//", "!": " not "}.get(tok, tok))
        if not out:
            raise KeyError(text)
        try:
            v = eval("".join(out), {"__builtins__": {}}, {})  # tokens are numbers and operators only
        except Exception:
            raise KeyError(text)
        return int(v)


def _preprocess(text, defined, expr):
    """Keep the lines of the taken #if/#ifdef/#elif/#else branches."""
    keep, stack = [], []   # stack of (taking, any branch taken, parent taking)
    taking = True
    for line in text.split("\n"):
        m = re.match(r"\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)", line)
        if not m:
            if taking:
                keep.append(line)
            continue
        kw, rest = m.group(1), m.group(2).strip()

        def cond():
            if kw == "ifdef":
                return rest.split()[0] in defined
            if kw == "ifndef":
                return rest.split()[0] not in defined
            e = re.sub(r"defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)",
                       lambda d: "1" if (d.group(1) or d.group(2)) in defined else "0", rest)
            try:
                return bool(expr.eval(e, unknown_zero=True))
            except KeyError:
                return False
        if kw in ("if", "ifdef", "ifndef"):
            c = taking and cond()
            stack.append((c, c, taking))
            taking = c
        elif kw == "elif" and stack:
            _, any_taken, parent = stack[-1]
            c = parent and not any_taken and cond()
            stack[-1] = (c, any_taken or c, parent)
            taking = c
        elif kw == "else" and stack:
            _, any_taken, parent = stack[-1]
            c = parent and not any_taken
            stack[-1] = (c, True, parent)
            taking = c
        elif kw == "endif" and stack:
            taking = stack.pop()[2]
    return "\n".join(keep)


def decomp_constants(game):
    """Every numeric #define and enumerator in the decomp's include/constants/*.h
    (pokeemerald for Emerald, pokeruby built as RUBY or SAPPHIRE), expression
    defines evaluated. map_groups.h is among them: Emerald's enum
    `MAP_X = (num | (group << 8))` and pokeruby's `#define MAP_X (num | (group << 8))`
    (both generated from data/maps/map_groups.json); LOCALID_* are in
    event_objects.h and map_event_ids.h."""
    if game in _CONST_CACHE:
        return _CONST_CACHE[game]
    if game not in GAMES:
        raise SystemExit("gen3: unknown game %s" % game)
    cdir = os.path.join(decomp_dir(game), "include", "constants")
    if not os.path.isdir(cdir):
        raise SystemExit("gen3: %s is missing (the %s decomp, see games/gba-common/README.md)" % (cdir, game))
    defined = {"ENGLISH"} | ({GAMES[game]["macro"]} if GAMES[game]["macro"] else set())
    expr = _Expr()
    names = []
    for fn in sorted(os.listdir(cdir)):
        if not fn.endswith(".h"):
            continue
        with open(os.path.join(cdir, fn), errors="replace") as f:
            text = _strip_comments(f.read().replace("\\\n", " "))
        # #defines first, so conditionals see the names defined above them
        lines = []
        for line in _preprocess(text, defined, expr).split("\n"):
            m = re.match(r"\s*#\s*define\s+([A-Za-z_]\w*)(\(?)(.*)$", line)
            if m:
                name, fnlike, body = m.group(1), m.group(2), m.group(3).strip()
                defined.add(name)
                if not fnlike and body and name not in expr.raw:
                    expr.raw[name] = body
                    names.append(name)
                continue
            lines.append(line)
        body = "\n".join(lines)
        for em in re.finditer(r"\benum\b[^{;]*\{(.*?)\}", body, flags=re.S):
            prev = None
            for item in em.group(1).split(","):
                item = item.strip()
                if not item:
                    continue
                m = re.match(r"([A-Za-z_]\w*)\s*(?:=\s*(.+))?$", item, flags=re.S)
                if not m:
                    prev = None
                    continue
                name = m.group(1)
                if m.group(2) is not None:
                    e = m.group(2)
                elif prev is None:
                    e = "0"
                else:
                    e = "(%s) + 1" % prev
                if name not in expr.raw:
                    expr.raw[name] = e
                    names.append(name)
                prev = name
    out = {}
    for name in names:
        try:
            out[name] = expr.value(name)
        except KeyError:
            pass  # not an integer constant (strings, casts, function-like uses)
    _CONST_CACHE[game] = out
    return out


def const(game, name):
    c = decomp_constants(game)
    if name not in c:
        raise SystemExit("gen3: %s has no constant %s" % (GAMES[game]["decomp"], name))
    return c[name]


# ---- text ----------------------------------------------------------------
_CHARMAPS = {}


def charmap(game):
    """(decode, encode) from the decomp's charmap.txt: the first single-character
    entry of each byte wins (the Latin table precedes the Japanese one)."""
    key = GAMES[game]["decomp"]
    if key not in _CHARMAPS:
        dec, enc = {}, {}
        with open(os.path.join(decomp_dir(game), "charmap.txt"), encoding="utf-8") as f:
            for line in f:
                m = re.match(r"\s*'(\\.|[^'\\])'\s*=\s*([0-9A-Fa-f]{2})\s*$", line)
                if not m:
                    continue
                ch = m.group(1)
                ch = {"\\'": "'", "\\\\": "\\", "\\n": "\n"}.get(ch, ch)
                b = int(m.group(2), 16)
                dec.setdefault(b, ch)
                enc.setdefault(ch, b)
        enc.setdefault("'", enc.get("’"))
        _CHARMAPS[key] = (dec, enc)
    return _CHARMAPS[key]


def decode_text(data, game):
    dec = charmap(game)[0]
    s = []
    for b in data:
        if b == EOS:
            break
        s.append(dec.get(b, "?"))
    return "".join(s)


def encode_text(text, length, game):
    enc = charmap(game)[1]
    out = bytearray()
    for ch in text:
        if enc.get(ch) is None:
            raise SystemExit("gen3: character %r has no %s encoding" % (ch, game))
        out.append(enc[ch])
    if len(out) > length:
        raise SystemExit("gen3: %r is longer than %d characters" % (text, length))
    return bytes(out) + bytes([EOS]) * (length - len(out))


# ---- ELF symbols and ROM tables -----------------------------------------
_SYMS = {}


def elf_symbols(path):
    """{name: (address, size)} from an ELF32 .symtab (globals win over locals)."""
    if path in _SYMS:
        return _SYMS[path]
    with open(path, "rb") as f:
        hdr = f.read(52)
        if hdr[:4] != b"\x7fELF" or hdr[4] != 1 or hdr[5] != 1:
            raise SystemExit("gen3: %s is not a little-endian ELF32 file" % path)
        shoff, = struct.unpack_from("<I", hdr, 0x20)
        shentsize, shnum = struct.unpack_from("<HH", hdr, 0x2E)
        f.seek(shoff)
        tab = f.read(shentsize * shnum)
        secs = [struct.unpack_from("<10I", tab, i * shentsize) for i in range(shnum)]
        symtab = next(s for s in secs if s[1] == 2)  # SHT_SYMTAB
        strtab = secs[symtab[6]]
        f.seek(symtab[4])
        syms = f.read(symtab[5])
        f.seek(strtab[4])
        strs = f.read(strtab[5])
    out = {}
    for off in range(0, len(syms), 16):
        name_off, value, size, info, _other, _shndx = struct.unpack_from("<IIIBBH", syms, off)
        if not name_off or (info & 15) not in (0, 1, 2):  # NOTYPE, OBJECT, FUNC
            continue
        name = strs[name_off:strs.index(b"\0", name_off)].decode("latin-1")
        if name not in out or info >> 4:  # a global binding overrides a local
            out[name] = (value, size)
    _SYMS[path] = out
    return out


class Rom:
    """A Ruby/Sapphire/Emerald ROM and the ELF of the decomp that built it."""

    def __init__(self, path, elf=None):
        with open(path, "rb") as f:
            self.data = f.read()
        self.path = path
        self.game = detect_game(self.data)
        self.elf = elf or os.path.join(decomp_dir(self.game), GAMES[self.game]["elf"])
        if not os.path.exists(self.elf):
            raise SystemExit("gen3: no ELF for %s (%s); build the decomp ROM (tools/rom_build.sh)" % (self.game, self.elf))
        self.syms = elf_symbols(self.elf)
        self._cache = {}

    def sym(self, *names):
        for n in names:
            if n in self.syms:
                return self.syms[n]
        raise SystemExit("gen3: %s has none of %s" % (self.elf, ", ".join(names)))

    def at(self, addr, size):
        off = addr - ROM_BASE
        if off < 0 or off + size > len(self.data):
            raise SystemExit("gen3: address 0x%08X is outside the ROM" % addr)
        return self.data[off:off + size]

    def table(self, name, entry, *alts):
        """(bytes, count) of a fixed-entry table by symbol."""
        addr, size = self.sym(name, *alts)
        return self.at(addr, size), size // entry

    def _memo(self, key, fn):
        if key not in self._cache:
            self._cache[key] = fn()
        return self._cache[key]

    def species_info(self):
        """[{base: [hp, atk, def, spe, spa, spd], types, abilities, growth, friendship, gender_ratio}]."""
        def load():
            raw, n = self.table("gSpeciesInfo", SPECIES_INFO_SIZE, "gBaseStats")
            out = []
            for i in range(n):
                e = raw[i * SPECIES_INFO_SIZE:(i + 1) * SPECIES_INFO_SIZE]
                out.append(dict(base=list(e[0:6]), types=[e[6], e[7]], gender_ratio=e[0x10],
                                friendship=e[0x12], growth=e[0x13], abilities=[e[0x16], e[0x17]]))
            return out
        return self._memo("species", load)

    def species_names(self):
        def load():
            raw, n = self.table("gSpeciesNames", SPECIES_NAME_SIZE)
            return [decode_text(raw[i * SPECIES_NAME_SIZE:(i + 1) * SPECIES_NAME_SIZE], self.game) for i in range(n)]
        return self._memo("species_names", load)

    def move_names(self):
        def load():
            raw, n = self.table("gMoveNames", MOVE_NAME_SIZE)
            return [decode_text(raw[i * MOVE_NAME_SIZE:(i + 1) * MOVE_NAME_SIZE], self.game) for i in range(n)]
        return self._memo("move_names", load)

    def moves(self):
        """[{effect, power, type, accuracy, pp, chance, target, priority, flags}] by move id."""
        def load():
            raw, n = self.table("gBattleMoves", BATTLE_MOVE_SIZE)
            out = []
            for i in range(n):
                e = struct.unpack_from("<BBBBBBBbB", raw, i * BATTLE_MOVE_SIZE)
                out.append(dict(effect=e[0], power=e[1], type=e[2], accuracy=e[3], pp=e[4], chance=e[5],
                                target=e[6], priority=e[7], flags=e[8]))
            return out
        return self._memo("moves", load)

    def type_chart(self):
        """[atk][def] multiplier x10 from gTypeEffectiveness (atk, def, mul) triples up to
        TYPE_ENDTABLE. The TYPE_FORESIGHT separator is skipped; the rows after it
        (Normal and Fighting on Ghost, which Foresight lifts) apply as usual
        (pokeemerald src/battle_main.c:335; pokeruby src/battle_script_commands.c:1504-1520)."""
        def load():
            addr, _ = self.sym("gTypeEffectiveness")
            chart = [[10] * NUM_TYPES for _ in range(NUM_TYPES)]
            off = addr - ROM_BASE
            while True:
                a, d, mul = self.data[off:off + 3]
                off += 3
                if a == TYPE_ENDTABLE:
                    break
                if a == TYPE_FORESIGHT:
                    continue
                chart[a][d] = mul
            return chart
        return self._memo("type_chart", load)

    def exp_table(self):
        """gExperienceTables[growth rate][level 0..100]."""
        def load():
            raw, rows = self.table("gExperienceTables", EXP_ROW * 4)
            return [list(struct.unpack_from("<%dI" % EXP_ROW, raw, r * EXP_ROW * 4)) for r in range(rows)]
        return self._memo("exp", load)

    def learnset(self, species):
        """[(level, move)] from gLevelUpLearnsets[species] up to LEVEL_UP_END."""
        addr, size = self.sym("gLevelUpLearnsets")
        if species >= size // 4:
            raise SystemExit("gen3: species %d has no learnset" % species)
        ptr, = struct.unpack_from("<I", self.at(addr + 4 * species, 4))
        out, off = [], ptr - ROM_BASE
        while True:
            v, = struct.unpack_from("<H", self.data, off)
            if v == LEVEL_UP_END:
                return out
            out.append((v >> 9, v & 0x1FF))
            off += 2

    def items(self):
        """[{name, pocket}] by item id (gItems)."""
        def load():
            raw, n = self.table("gItems", ITEM_SIZE)
            return [dict(name=decode_text(raw[i * ITEM_SIZE:i * ITEM_SIZE + 14], self.game),
                         pocket=raw[i * ITEM_SIZE + ITEM_POCKET]) for i in range(n)]
        return self._memo("items", load)

    def national_dex(self):
        """species id -> national dex number (SpeciesToNationalPokedexNum's table, indexed species - 1:
        pokeemerald sSpeciesToNationalPokedexNum, pokeruby gSpeciesToNationalPokedexNum)."""
        def load():
            raw, n = self.table("sSpeciesToNationalPokedexNum", 2, "gSpeciesToNationalPokedexNum")
            return [0] + list(struct.unpack_from("<%dH" % n, raw))
        return self._memo("natdex", load)

    def national_to_species(self):
        def load():
            out = {}
            for sp, nat in enumerate(self.national_dex()):
                if nat:
                    out.setdefault(nat, sp)
            return out
        return self._memo("natdex_inv", load)

    def map_section(self, map_id):
        """gMapGroups[group][num]->regionMapSectionId, for a mon's met location."""
        addr, _ = self.sym("gMapGroups")
        grp, num = map_id >> 8, map_id & 0xFF
        gptr, = struct.unpack_from("<I", self.at(addr + 4 * grp, 4))
        hptr, = struct.unpack_from("<I", self.at(gptr + 4 * num, 4))
        return self.at(hptr + MAP_HEADER_MAPSEC, 1)[0]

    def species_name(self, sp):
        names = self.species_names()
        return names[sp] if sp < len(names) else None

    def item_name(self, item):
        items = self.items()
        return items[item]["name"] if item < len(items) else None

    def move_name(self, move):
        names = self.move_names()
        return names[move] if move < len(names) else None


# ---- Pokemon -------------------------------------------------------------
class Mon:
    """A decrypted BoxPokemon (+ the party fields). subs[t] is substruct type t
    (0 growth, 1 attacks, 2 EVs/contest, 3 misc), 12 plaintext bytes each."""

    def __init__(self, raw, game):
        raw = bytes(raw)
        self.game = game
        self.head = bytearray(raw[:32])
        self.party = bytearray(raw[80:100]) if len(raw) >= PARTY_SIZE else None
        key = self.personality ^ self.ot_id
        plain = struct.pack("<12I", *[w ^ key for w in struct.unpack_from("<12I", raw, 32)])
        pos = SUBSTRUCT_POS[self.personality % 24]
        self.subs = [bytearray(plain[pos[t] * 12:pos[t] * 12 + 12]) for t in range(4)]
        self.checksum_ok = struct.unpack_from("<H", raw, 28)[0] == self.calc_checksum()

    @classmethod
    def blank(cls, game):
        return cls(bytes(PARTY_SIZE), game)

    # header: u32 personality, u32 otId, nickname[10], language, flags, otName[7], markings, u16 checksum, u16 unknown
    personality = property(lambda s: struct.unpack_from("<I", s.head, 0)[0],
                           lambda s, v: struct.pack_into("<I", s.head, 0, v))
    ot_id = property(lambda s: struct.unpack_from("<I", s.head, 4)[0],
                     lambda s, v: struct.pack_into("<I", s.head, 4, v))
    species = property(lambda s: struct.unpack_from("<H", s.subs[0], 0)[0],
                       lambda s, v: struct.pack_into("<H", s.subs[0], 0, v))
    held_item = property(lambda s: struct.unpack_from("<H", s.subs[0], 2)[0],
                         lambda s, v: struct.pack_into("<H", s.subs[0], 2, v))
    exp = property(lambda s: struct.unpack_from("<I", s.subs[0], 4)[0],
                   lambda s, v: struct.pack_into("<I", s.subs[0], 4, v))

    def _get_iv_word(self):
        return struct.unpack_from("<I", self.subs[3], 4)[0]

    def _set_iv_word(self, v):
        struct.pack_into("<I", self.subs[3], 4, v)

    def calc_checksum(self):
        return sum(struct.unpack("<24H", b"".join(bytes(s) for s in self.subs))) & 0xFFFF

    @property
    def is_empty(self):
        return self.personality == 0 and self.ot_id == 0 and not (self.head[19] & 2)

    @property
    def is_egg(self):
        return bool(self._get_iv_word() >> 30 & 1)

    @property
    def ivs(self):
        w = self._get_iv_word()
        return [(w >> (5 * i)) & 31 for i in range(6)]

    def set_iv(self, stat, value):
        w = self._get_iv_word() & ~(31 << (5 * stat))
        self._set_iv_word(w | (value & 31) << (5 * stat))

    @property
    def evs(self):
        return list(self.subs[2][0:6])

    def set_ev(self, stat, value):
        self.subs[2][stat] = value

    @property
    def moves(self):
        ids = struct.unpack_from("<4H", self.subs[1], 0)
        pps = list(self.subs[1][8:12])
        bonuses = self.subs[0][8]
        return [(ids[i], pps[i], bonuses >> (2 * i) & 3) for i in range(4)]

    def set_move(self, slot, move, pp):
        struct.pack_into("<H", self.subs[1], 2 * slot, move)
        self.subs[1][8 + slot] = pp
        self.subs[0][8] &= ~(3 << (2 * slot)) & 0xFF  # a new move has no PP Ups (gPPUpClearMask)

    @property
    def level(self):
        return self.party[4] if self.party else None

    @property
    def stats(self):
        return list(struct.unpack_from("<6H", self.party, 8)) if self.party else None

    @property
    def hp(self):
        return struct.unpack_from("<H", self.party, 6)[0] if self.party else None

    def pack(self, party=True):
        struct.pack_into("<H", self.head, 28, self.calc_checksum())
        # hasSpecies follows the species (SetBoxMonData MON_DATA_SPECIES, pokeemerald pokemon.c)
        self.head[19] = (self.head[19] & ~2) | (2 if self.species else 0)
        key = self.personality ^ self.ot_id
        pos = SUBSTRUCT_POS[self.personality % 24]
        plain = bytearray(48)
        for t in range(4):
            plain[pos[t] * 12:pos[t] * 12 + 12] = self.subs[t]
        secure = struct.pack("<12I", *[w ^ key for w in struct.unpack("<12I", plain)])
        out = bytes(self.head) + secure
        if party:
            out += bytes(self.party if self.party is not None else bytes(20))
        return out

    def recalc_stats(self, rom, level=None, full_hp=True):
        """CalculateMonStats (pokeemerald src/pokemon.c:2814-2896; pokeruby src/pokemon_1.c:1702)
        with the level from the exp (GetLevelFromMonExp, pokemon.c:2910-2920)."""
        info = rom.species_info()[self.species]
        if level is None:
            table = rom.exp_table()[info["growth"]]
            level = 1
            while level <= 100 and table[level] <= self.exp:
                level += 1
            level -= 1
        ivs, evs = self.ivs, self.evs
        if self.species == const(self.game, "SPECIES_SHEDINJA"):
            maxhp = 1
        else:
            maxhp = (2 * info["base"][0] + ivs[0] + evs[0] // 4) * level // 100 + level + 10
        nature = self.personality % 25
        up, down = nature // 5 + 1, nature % 5 + 1   # ModifyStatByNature / gNatureStatTable
        stats = [maxhp]
        for i in range(1, 6):
            n = (2 * info["base"][i] + ivs[i] + evs[i] // 4) * level // 100 + 5
            if up != down and i == up:
                n = n * 110 // 100
            elif up != down and i == down:
                n = n * 90 // 100
            stats.append(n)
        if self.party is None:
            self.party = bytearray(20)
        self.party[4] = level
        struct.pack_into("<6H", self.party, 8, *stats)
        if full_hp:
            struct.pack_into("<H", self.party, 6, maxhp)

    def to_dict(self, rom=None, slot=None):
        g = self.game
        tid, sid = self.ot_id & 0xFFFF, self.ot_id >> 16
        p = self.personality
        misc = self.subs[3]
        origin, = struct.unpack_from("<H", misc, 2)
        iv_word = self._get_iv_word()
        d = {}
        if slot is not None:
            d["slot"] = slot
        d.update(species=self.species,
                 species_name=rom.species_name(self.species) if rom else None,
                 nickname=decode_text(self.head[8:18], g),
                 is_egg=self.is_egg or bool(self.head[19] & 4),
                 checksum_ok=self.checksum_ok)
        if self.party is not None:
            d["level"] = self.level
        d.update(exp=self.exp, pid="0x%08X" % p,
                 shiny=(tid ^ sid ^ (p >> 16) ^ (p & 0xFFFF)) < 8,
                 nature=NATURES[p % 25])
        ability_num = iv_word >> 31
        ability = None
        if rom and self.species < len(rom.species_info()):
            ability = rom.species_info()[self.species]["abilities"][ability_num]
        d["ability"] = {"id": ability, "num": ability_num}
        d["held_item"] = {"id": self.held_item, "name": rom.item_name(self.held_item) if rom else None}
        d["moves"] = [{"id": m, "name": rom.move_name(m) if rom else None, "pp": pp, "pp_ups": ups}
                      for m, pp, ups in self.moves if m]
        d.update(ivs=self.ivs, evs=self.evs, friendship=self.subs[0][9],
                 ot_name=decode_text(self.head[20:27], g), tid=tid, sid=sid,
                 met_location={"id": misc[1]}, met_level=origin & 0x7F,
                 met_game=origin >> 7 & 15, ball={"id": origin >> 11 & 15},
                 ot_gender="female" if origin >> 15 else "male",
                 language=self.head[18], pokerus=misc[0])
        if self.party is not None:
            d.update(hp=self.hp, stats=self.stats, status=struct.unpack_from("<I", self.party, 0)[0])
        return d


def decode_party(raw, count, game, rom=None):
    """The party as dump dicts from raw struct Pokemon[count] bytes: a save's
    SaveBlock1 playerParty or the guest's gPlayerParty read from RAM."""
    count = max(0, min(count, PARTY_MAX))
    return [Mon(raw[i * PARTY_SIZE:(i + 1) * PARTY_SIZE], game).to_dict(rom, i + 1) for i in range(count)]


# ---- the save ------------------------------------------------------------
class Save:
    """The 128 KiB flash image; the newest valid slot assembled into sb2/sb1/storage."""

    def __init__(self, image, game):
        if len(image) < 2 * SLOT_SECTORS * SECTOR:
            raise SystemExit("gen3: not a 128 KiB flash save (%d bytes)" % len(image))
        self.img = bytearray(image)
        self.game = game
        self.lay = layout(game)
        self.sizes = {"sb2": self.lay["sb2_size"], "sb1": self.lay["sb1_size"], "storage": self.lay["storage_size"]}
        self.slot, self.counter, self.load_result = self._pick_slot()
        self.sector_of = {}
        for i in range(SLOT_SECTORS):
            sec = self.slot * SLOT_SECTORS + i
            self.sector_of[self.footer(sec)[0]] = sec
        self.blocks = {k: bytearray(v) for k, v in self.sizes.items()}
        for cid, (block, n) in enumerate(CHUNKS):
            size = self.chunk_size(cid)
            base = self.sector_of[cid] * SECTOR
            self.blocks[block][n * SECTOR_DATA:n * SECTOR_DATA + size] = self.img[base:base + size]
        self.sb2, self.sb1, self.storage = self.blocks["sb2"], self.blocks["sb1"], self.blocks["storage"]

    def footer(self, sec):
        return struct.unpack_from("<HHII", self.img, sec * SECTOR + 0xFF4)  # id, checksum, signature, counter

    def chunk_size(self, cid):
        block, n = CHUNKS[cid]
        return min(self.sizes[block] - n * SECTOR_DATA, SECTOR_DATA)

    def _slot_state(self, slot):
        valid, counter, signed = 0, None, False
        for i in range(SLOT_SECTORS):
            sec = slot * SLOT_SECTORS + i
            sid, cs, sig, cnt = self.footer(sec)
            if sig != SIGNATURE or sid >= SLOT_SECTORS:
                continue
            signed = True
            if checksum(self.img[sec * SECTOR:], self.chunk_size(sid)) == cs:
                valid |= 1 << sid
                counter = cnt
        if not signed:
            return "empty", None
        return ("ok" if valid == (1 << SLOT_SECTORS) - 1 else "error"), counter

    def _pick_slot(self):
        """GetSaveValidStatus (pokeemerald src/save.c:515-638; pokeruby src/save.c:503-600)."""
        (s1, c1), (s2, c2) = self._slot_state(0), self._slot_state(1)
        if s1 == "ok" and s2 == "ok":
            if (c1, c2) in ((0xFFFFFFFF, 0), (0, 0xFFFFFFFF)):
                newer = 1 if (c1 + 1) & 0xFFFFFFFF < (c2 + 1) & 0xFFFFFFFF else 0
            else:
                newer = 1 if c1 < c2 else 0
            return newer, (c1, c2)[newer], "ok"
        if s1 == "ok":
            return 0, c1, "ok" if s2 == "empty" else "recovered"
        if s2 == "ok":
            return 1, c2, "ok" if s1 == "empty" else "recovered"
        raise SystemExit("gen3: no valid save slot (slot 1 %s, slot 2 %s)" % (s1, s2))

    def to_bytes(self):
        """The image with the newest slot rewritten from sb2/sb1/storage; each
        sector's checksum recomputed over its chunk size; ids, signature and
        counter kept (the slot stays the one the game loads)."""
        img = bytearray(self.img)
        for cid, (block, n) in enumerate(CHUNKS):
            size = self.chunk_size(cid)
            base = self.sector_of[cid] * SECTOR
            img[base:base + size] = self.blocks[block][n * SECTOR_DATA:n * SECTOR_DATA + size]
            struct.pack_into("<H", img, base + 0xFF6, checksum(img[base:], size))
        return bytes(img)

    # -- Emerald's encrypted values (load_save.c:286-292; money.c:72-80; item.c:26-34; overworld.c:447-458)
    @property
    def key(self):
        if not self.lay["encrypted"]:
            return 0
        return struct.unpack_from("<I", self.sb2, SB2_ENCRYPTION_KEY)[0]

    def u32_enc(self, off, value=None):
        if value is None:
            return struct.unpack_from("<I", self.sb1, off)[0] ^ self.key
        struct.pack_into("<I", self.sb1, off, (value ^ self.key) & 0xFFFFFFFF)

    def u16_enc(self, off, value=None):
        if value is None:
            return (struct.unpack_from("<H", self.sb1, off)[0] ^ self.key) & 0xFFFF
        struct.pack_into("<H", self.sb1, off, (value ^ self.key) & 0xFFFF)

    # -- trainer
    @property
    def trainer_id(self):
        return struct.unpack_from("<I", self.sb2, SB2_TRAINER_ID)[0]

    def flag(self, fid, value=None):
        n = self.lay["flags_bytes"] * 8
        if not 0 <= fid < n:
            raise SystemExit("gen3: flag %d out of range (0..%d)" % (fid, n - 1))
        off = self.lay["flags"] + fid // 8
        if value is None:
            return bool(self.sb1[off] >> (fid % 8) & 1)
        if value:
            self.sb1[off] |= 1 << (fid % 8)
        else:
            self.sb1[off] &= ~(1 << (fid % 8)) & 0xFF

    def var(self, vid, value=None):
        start = const(self.game, "VARS_START")
        idx = vid - start
        if not 0 <= idx < self.lay["vars_count"]:
            raise SystemExit("gen3: var 0x%X is not a saved var (0x%X..0x%X)" % (
                vid, start, start + self.lay["vars_count"] - 1))
        off = self.lay["vars"] + 2 * idx
        if value is None:
            return struct.unpack_from("<H", self.sb1, off)[0]
        struct.pack_into("<H", self.sb1, off, value)

    def party_count(self):
        return min(self.sb1[SB1_PARTY_COUNT], PARTY_MAX)

    def party_mon(self, i):
        off = SB1_PARTY + PARTY_SIZE * i
        return Mon(self.sb1[off:off + PARTY_SIZE], self.game)

    def set_party_mon(self, i, mon):
        off = SB1_PARTY + PARTY_SIZE * i
        self.sb1[off:off + PARTY_SIZE] = mon.pack()

    def pocket(self, name):
        for pname, off, cap in self.lay["pockets"]:
            if pname == name:
                return off, cap
        raise SystemExit("gen3: no pocket %s" % name)

    def bag(self):
        out = {}
        for pname, off, cap in self.lay["pockets"]:
            slots = []
            for i in range(cap):
                item = struct.unpack_from("<H", self.sb1, off + 4 * i)[0]
                if item:
                    slots.append((item, self.u16_enc(off + 4 * i + 2)))
            out[pname] = slots
        return out

    def dex_get(self, nat):
        """(seen, caught) as GetSetPokedexFlag FLAG_GET_SEEN / FLAG_GET_CAUGHT read
        them: set in SaveBlock2's pokedex and both SaveBlock1 copies
        (pokeemerald src/pokedex.c:4263-4320, pokeruby src/pokedex.c:3986-4040)."""
        i, mask = (nat - 1) // 8, 1 << ((nat - 1) % 8)
        seen = [self.sb2[DEX_SEEN + i] & mask, self.sb1[self.lay["seen_a"] + i] & mask,
                self.sb1[self.lay["seen_b"] + i] & mask]
        owned = self.sb2[DEX_OWNED + i] & mask
        s = bool(seen[0]) and seen[0] == seen[1] == seen[2]
        return s, bool(owned) and s

    def dex_set(self, nat, caught):
        """FLAG_SET_SEEN (all three seen copies), FLAG_SET_CAUGHT (owned)."""
        i, mask = (nat - 1) // 8, 1 << ((nat - 1) % 8)
        self.sb2[DEX_SEEN + i] |= mask
        self.sb1[self.lay["seen_a"] + i] |= mask
        self.sb1[self.lay["seen_b"] + i] |= mask
        if caught:
            self.sb2[DEX_OWNED + i] |= mask

    def national_dex(self):
        """IsNationalPokedexEnabled (pokeemerald src/event_data.c:74-80, pokeruby 70-76)."""
        return (self.sb2[DEX_MAGIC] == NATIONAL_MAGIC
                and self.var(const(self.game, "VAR_NATIONAL_DEX")) == NATIONAL_VAR_VALUE
                and self.flag(const(self.game, "FLAG_SYS_NATIONAL_DEX")))

    def hall_of_fame(self):
        """The Hall of Fame sectors: valid when signed and the checksum (kept in the
        footer's id field) matches (pokeemerald src/save.c:640-666 TryLoadSaveSector)."""
        data = bytearray()
        for i in range(HOF_SECTORS):
            sec = HOF_SECTOR + i
            if len(self.img) < (sec + 1) * SECTOR:
                return None
            sid, _cs, sig, _cnt = self.footer(sec)
            if sig != SIGNATURE or sid != checksum(self.img[sec * SECTOR:], SECTOR_DATA):
                return None
            data += self.img[sec * SECTOR:sec * SECTOR + SECTOR_DATA]
        teams = []
        for t in range(HOF_TEAMS):
            team = data[t * HOF_TEAM:(t + 1) * HOF_TEAM]
            if not struct.unpack_from("<H", team, 8)[0] & 0x1FF:
                break  # Task_Hof_InitTeamSaveData: the first team without a lead species ends the list
            mons = []
            for m in range(6):
                e = team[m * HOF_MON:(m + 1) * HOF_MON]
                v, = struct.unpack_from("<H", e, 8)
                if v & 0x1FF:
                    mons.append(dict(species=v & 0x1FF, level=v >> 9, nickname=decode_text(e[10:20], self.game)))
            teams.append(mons)
        return teams


def load(rom_path, sav_path, elf=None):
    rom = Rom(rom_path, elf)
    with open(sav_path, "rb") as f:
        return rom, Save(f.read(), rom.game)


def dump(rom, sav):
    """np_save4 dump's keys (features/tools/np_save4.c cmd_dump) for a gen 3 save."""
    g = rom.game
    sb2, sb1, lay = sav.sb2, sav.sb1, sav.lay
    tid = sav.trainer_id
    badge_flags = [const(g, "FLAG_BADGE0%d_GET" % i) for i in range(1, 9)]
    badge_mask = sum(1 << i for i, f in enumerate(badge_flags) if sav.flag(f))
    hours, minutes, seconds = struct.unpack_from("<HBB", sb2, SB2_PLAY_TIME)
    national = sav.national_dex()
    out = {"game": g, "load_result": sav.load_result,
           "rom": {"title": rom.data[0xA0:0xAC].decode("latin-1").rstrip("\0"),
                   "gamecode": rom.data[0xAC:0xB0].decode("latin-1"), "version": rom.data[0xBC]},
           "slot": sav.slot + 1, "save_counter": sav.counter}
    out["trainer"] = {
        "name": decode_text(sb2[SB2_NAME:SB2_NAME + 8], g), "tid": tid & 0xFFFF, "sid": tid >> 16,
        "gender": "female" if sb2[SB2_GENDER] else "male",
        "money": sav.u32_enc(SB1_MONEY), "coins": sav.u16_enc(SB1_COINS),
        "badges": bin(badge_mask).count("1"), "badge_mask": badge_mask,
        "play_time": "%u:%02u:%02u" % (hours, minutes, seconds),
        "national_dex": national}
    # The CONTINUE location: the continue-game warp when its flag is set (the
    # game warps there, pokeemerald src/overworld.c:1739-1745), else the saved
    # map and position. `saved` is always SaveBlock1's location/pos.
    lg, ln = struct.unpack_from("<bb", sb1, SB1_LOCATION)
    px, py = struct.unpack_from("<hh", sb1, SB1_POS)
    saved = {"map": (lg & 0xFF) << 8 | (ln & 0xFF), "x": px, "y": py}
    cont = bool(sb2[SB2_SPECIAL_WARP] & CONTINUE_GAME_WARP)
    if cont:
        cg, cn, _w, cx, cy = struct.unpack_from("<bbbxhh", sb1, SB1_CONTINUE_WARP)
        loc = {"map": (cg & 0xFF) << 8 | (cn & 0xFF), "x": cx, "y": cy}
    else:
        loc = dict(saved)
    loc["z"] = loc["y"]  # np_save4's name for the second map coordinate
    loc["continue_warp"] = cont
    loc["saved"] = saved
    out["location"] = loc
    out["flags"] = [f for f in range(lay["flags_bytes"] * 8) if sav.flag(f)]
    start = const(g, "VARS_START")
    out["vars"] = {str(start + i): v for i in range(lay["vars_count"])
                   for v in [struct.unpack_from("<H", sb1, lay["vars"] + 2 * i)[0]] if v}
    out["party"] = decode_party(sb1[SB1_PARTY:SB1_PARTY + PARTY_MAX * PARTY_SIZE], sav.party_count(), g, rom)
    bag = {}
    for pname, slots in sav.bag().items():
        bag[pname] = [{"item": it, "name": rom.item_name(it), "qty": q} for it, q in slots]
    bag["registered"] = struct.unpack_from("<H", sb1, SB1_REGISTERED)[0]
    out["bag"] = bag
    nat_to_sp = rom.national_to_species()
    seen_l, caught_l = [], []
    for nat in range(1, max(rom.national_dex()) + 1):  # NATIONAL_DEX_COUNT (pokeruby has it outside constants/)
        s, c = sav.dex_get(nat)
        if s:
            seen_l.append(nat_to_sp.get(nat, 0))
        if c:
            caught_l.append(nat_to_sp.get(nat, 0))
    out["pokedex"] = {"seen": len(seen_l), "caught": len(caught_l),
                      "obtained": sav.flag(const(g, "FLAG_SYS_POKEDEX_GET")), "national": national,
                      "seen_list": sorted(seen_l), "caught_list": sorted(caught_l)}
    teams = sav.hall_of_fame()
    if not teams:
        out["hall_of_fame"] = {"total": 0, "latest": None}
    else:
        out["hall_of_fame"] = {"total": len(teams), "latest": {"party": [
            dict(species=m["species"], species_name=rom.species_name(m["species"]), level=m["level"],
                 nickname=m["nickname"]) for m in teams[-1]]}}
    out["game_stats"] = [sav.u32_enc(lay["game_stats"] + 4 * i) for i in range(lay["game_stats_count"])]
    out["clock"] = {name: dict(zip(("days", "hours", "minutes", "seconds"), struct.unpack_from(TIME_FMT, sb2, off)))
                    for name, off in (("local_time_offset", SB2_LOCAL_TIME_OFFSET),
                                      ("last_berry_tree_update", SB2_LAST_BERRY_UPDATE))}
    trees = []
    for i in range(BERRY_TREES_COUNT):
        berry, st, mins, yld, bits = struct.unpack_from("<BBHBB", sb1, lay["berry_trees"] + i * BERRY_TREE_SIZE)
        if berry or st:
            trees.append({"id": i, "berry": berry, "stage": st & 0x7F, "sparkle": st >> 7,
                          "minutes_until_next_stage": mins, "yield": yld, "regrowth_count": bits & 0xF,
                          "watered": [bits >> (4 + k) & 1 for k in range(4)]})
    out["berry_trees"] = trees
    if "secret_bases" in lay:
        out["secret_bases"] = []
        for i in range(lay["secret_base_count"]):
            off = lay["secret_bases"] + i * lay["secret_base_size"]
            if sb1[off]:
                out["secret_bases"].append({"index": i, "id": sb1[off],
                                            "decorations": list(sb1[off + 0x12:off + 0x22]),
                                            "positions": list(sb1[off + 0x22:off + 0x32])})
        out["decorations"] = {name: list(sb1[off:off + n]) for name, off, n in lay["decor"]}
    return out


def move_class(power, mtype):
    """2 status (no power), else 0 physical / 1 special by type (IS_TYPE_PHYSICAL)."""
    if power == 0:
        return 2
    return 0 if mtype < TYPE_MYSTERY else 1


def move_range(target):
    """The harness's range bits: MOVE_TARGET_FOES_AND_ALLY (Earthquake) -> RANGE_ALL_ADJACENT;
    other targets keep their gen 3 bits without that one (MOVE_TARGET_BOTH, which
    is 0x08 too, hits only the foes)."""
    if target & MOVE_TARGET_FOES_AND_ALLY:
        return RANGE_ALL_ADJACENT | (target & ~(MOVE_TARGET_FOES_AND_ALLY | RANGE_ALL_ADJACENT))
    return target & ~RANGE_ALL_ADJACENT


def gamedata(rom):
    """np_save4 gamedata's shape (tests/e2e/bots.py gamedata/mon_types/move_value):
    species[i] = [type1, type2, ability1, ability2, hp, atk, def, spe, spa, spd, growth];
    moves[i] = [effect, class, power, type, accuracy, pp, priority, range];
    type_chart[atk][def] x10; exp_table[growth][level]."""
    species = [i["types"] + i["abilities"] + i["base"] + [i["growth"]] for i in rom.species_info()]
    moves = [[m["effect"], move_class(m["power"], m["type"]), m["power"], m["type"], m["accuracy"], m["pp"],
              m["priority"], move_range(m["target"])] for m in rom.moves()]
    return {"game": rom.game, "species": species, "moves": moves, "type_chart": rom.type_chart(),
            "exp_table": rom.exp_table()}


def to_json(obj):
    return json.dumps(obj, indent=1, ensure_ascii=False)
