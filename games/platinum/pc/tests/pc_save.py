#!/usr/bin/env python3
"""Read a Pokemon Platinum save the way the game does, and answer questions.

This is the semantic oracle the corpus rests on. Every station in the plan can
pin a digest, and a digest that is stable and WRONG passes just as quietly as
one that is right, so a station also asserts what actually happened, and this
is what turns "did it really happen" into an assert.

    $ python3 pc/tests/pc_save.py build/pc/lab/station.sav
    $ python3 pc/tests/pc_save.py --json station.sav

What it restates and what it does not. Two facts about a save are unavailable
from outside the running game and both are fetched rather than assumed:

  * where each of the 38 save-table pages lands. That is not a constant in the
    tree; SavePageInfo_Init sums 38 size functions at boot. The port prints
    the answer under --save-layout and this reads it. Regenerated
    automatically when the binary is newer than the cached copy.
  * where each field lands inside its struct. That comes from the DWARF the
    port's own build emitted, the same source pc/abi_layout.py uses. No
    offset in this file is typed by hand.

What it does restate is two ALGORITHMS: CRC-16/CCITT and the Gen-4 Pokemon
block cipher. Both are small, both are checkable against a written standard
rather than against this tree, and both are covered by the round-trip property
test in the suite, mint a recipe, read it back, compare.
"""

import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PORT = os.path.join(ROOT, "build", "pc", "pokeplatinum")
LAYOUT_CACHE = os.path.join(ROOT, "build", "pc", "save-layout.json")

sys.path.insert(0, os.path.join(ROOT, "pc"))
import abi_layout  # noqa: E402  (path set above)

# The objects that between them declare every struct this reader reaches into.
# Any object that included the header would do; these are the ones that own it.
DWARF_OBJECTS = [
    "src/savedata.o", "src/save_player.o", "src/trainer_info.o",
    "src/party.o", "src/pokemon.o", "src/bag.o", "src/vars_flags.o",
    "src/field_overworld_state.o", "src/play_time.o", "src/pokedex.o",
    "src/system_data.o", "src/savedata_misc.o", "src/special_encounter.o",
    "src/daycare_save.o", "src/pc_boxes.o", "src/poffin.o",
    "src/underground.o", "src/poketch.o",
]

SECTOR_PRIMARY, SECTOR_BACKUP = 0, 1
SECTOR_INVALID, SECTOR_PARTIAL, SECTOR_VALID = 0, 1, 2
LOAD_OK, LOAD_CORRUPT, LOAD_ERROR, LOAD_EMPTY = "ok", "corrupt", "error", "empty"


# ---------------------------------------------------------------- primitives

def crc16_ccitt(data):
    """MATH_CalcCRC16CCITT: poly 0x1021, init 0xFFFF, MSB-first, no final xor."""
    table = crc16_ccitt.table
    r = 0xFFFF
    for b in data:
        r = ((r << 8) & 0xFFFF) ^ table[((r >> 8) ^ b) & 0xFF]
    return r


def _crc16_table():
    table = []
    for i in range(256):
        r = i << 8
        for _ in range(8):
            r = ((r << 1) ^ 0x1021) if (r & 0x8000) else (r << 1)
        table.append(r & 0xFFFF)
    return table


crc16_ccitt.table = _crc16_table()

LCRNG_MULT, LCRNG_INC = 1103515245, 24691


def decode_data(data, seed):
    """EncodeData/DecodeData in src/math_util.c, XOR with the LCRNG's top half."""
    out = bytearray(data)
    for i in range(0, (len(data) // 2) * 2, 2):
        seed = (seed * LCRNG_MULT + LCRNG_INC) & 0xFFFFFFFF
        k = (seed >> 16) & 0xFFFF
        out[i] ^= k & 0xFF
        out[i + 1] ^= (k >> 8) & 0xFF
    return bytes(out)


# The 24 permutations BoxPokemon_GetDataBlock selects between, indexed by
# ((personality & 0x3E000) >> 13) % 24. Written out because the C writes them
# out; the value is the position each of blocks a,b,c,d sits at.
BLOCK_ORDER = [
    (0, 1, 2, 3), (0, 1, 3, 2), (0, 2, 1, 3), (0, 3, 1, 2), (0, 2, 3, 1), (0, 3, 2, 1),
    (1, 0, 2, 3), (1, 0, 3, 2), (2, 0, 1, 3), (3, 0, 1, 2), (2, 0, 3, 1), (3, 0, 2, 1),
    (1, 2, 0, 3), (1, 3, 0, 2), (2, 1, 0, 3), (3, 1, 0, 2), (2, 3, 0, 1), (3, 2, 0, 1),
    (1, 2, 3, 0), (1, 3, 2, 0), (2, 1, 3, 0), (3, 1, 2, 0), (2, 3, 1, 0), (3, 2, 1, 0),
]


# ------------------------------------------------------------------- layouts

class Layout:
    """Struct member offsets, in bytes, out of the port's own DWARF."""

    def __init__(self, objects=None):
        objdir = os.path.join(ROOT, "build", "pc", "obj", "game")
        self.records = {}
        for rel in (objects or DWARF_OBJECTS):
            path = os.path.join(objdir, rel)
            if not os.path.exists(path):
                continue
            _, layouts = abi_layout.read_object(path)
            for name, layout in layouts.items():
                self.records.setdefault(name, layout)
        if not self.records:
            raise SystemExit(
                "pc_save: no DWARF found under build/pc/obj/game, build the port first")

    def size(self, struct):
        return self.records[struct][0]

    def off(self, struct, member):
        for name, bit, _width in self.records[struct][1]:
            if name == member:
                return bit // 8
        raise KeyError("%s has no member %s" % (struct, member))

    def bit(self, struct, member):
        """(byte offset, bit within it, width) for a bitfield member."""
        for name, bit, width in self.records[struct][1]:
            if name == member:
                return bit // 8, bit % 8, width
        raise KeyError("%s has no member %s" % (struct, member))


def save_layout(port=PORT, cache=LAYOUT_CACHE):
    """Where each save-table page lands, asked of the port that computes it."""
    fresh = (os.path.exists(cache) and os.path.exists(port)
             and os.path.getmtime(cache) >= os.path.getmtime(port))
    if not fresh:
        if not os.path.exists(port):
            raise SystemExit("pc_save: %s does not exist, build the port first" % port)
        env = dict(os.environ, PC_SAVE="none", PC_SAVE_LAYOUT=cache)
        subprocess.run([port], env=env, capture_output=True, check=True)
    with open(cache) as f:
        return json.load(f)


def save_table_ids():
    """SAVE_TABLE_ENTRY_* -> id, from the header that declares them."""
    path = os.path.join(ROOT, "include", "constants", "savedata", "save_table.h")
    ids, n = {}, 0
    with open(path) as f:
        text = f.read()
    body = re.search(r"enum\s+SaveTableEntryID\s*\{(.*?)\}", text, re.S)
    if body is None:
        raise SystemExit("pc_save: cannot find enum SaveTableEntryID")
    for line in body.group(1).splitlines():
        m = re.match(r"\s*(SAVE_TABLE_ENTRY_\w+)\s*(?:=\s*(\S+?))?\s*,?\s*(?://.*)?$", line)
        if not m:
            continue
        if m.group(2):
            n = int(m.group(2), 0)
        ids[m.group(1)] = n
        n += 1
    return ids


# ------------------------------------------------------------------ the save

class Footer:
    def __init__(self, raw, layout):
        o = layout.off
        self.save_counter = int.from_bytes(raw[o("SaveBlockFooter", "saveCounter"):][:4], "little")
        self.block_counter = int.from_bytes(raw[o("SaveBlockFooter", "blockCounter"):][:4], "little")
        self.size = int.from_bytes(raw[o("SaveBlockFooter", "size"):][:4], "little")
        self.signature = int.from_bytes(raw[o("SaveBlockFooter", "signature"):][:4], "little")
        self.block_id = raw[o("SaveBlockFooter", "saveBlockID")]
        self.checksum = int.from_bytes(raw[o("SaveBlockFooter", "checksum"):][:2], "little")


class Save:
    """One save file, arbitrated and readable by name."""

    def __init__(self, path, layout=None, table=None):
        with open(path, "rb") as f:
            self.raw = f.read()
        self.path = path
        self.t = layout or Layout()
        self.table = table or save_layout()
        self.ids = save_table_ids()
        self.pages = {p["id"]: p for p in self.table["pages"]}
        self.blocks = {b["id"]: b for b in self.table["blocks"]}
        self.errors = []
        self._arbitrate()

    # integrity -------------------------------------------------------

    def _sector_base(self, sector):
        start = (self.table["primary_sector_start"] if sector == SECTOR_PRIMARY
                 else self.table["backup_sector_start"])
        return start * self.table["sector_size"]

    def _block_bytes(self, sector, block_id):
        b = self.blocks[block_id]
        start = self._sector_base(sector) + b["offset"]
        return self.raw[start:start + b["size"]]

    def _check(self, sector, block_id):
        """SaveBlockFooter_Validate, plus what it found, for the report."""
        b = self.blocks[block_id]
        body = self._block_bytes(sector, block_id)
        fsize = self.table["footer_size"]
        if len(body) < b["size"]:
            return None, False, "truncated"
        footer = Footer(body[-fsize:], self.t)
        # An all-0xFF footer is an ERASED copy, not a damaged one, and a valid
        # save routinely has one: SaveBlockFooter_Erase clears all four at the
        # first save of a new game, and after that only the NORMAL block gets
        # written on every save (fullSaveRequired goes false), so the boxes
        # block's second copy stays erased until something writes the boxes.
        # Reporting that as corruption would make every healthy save look bad.
        if body[-fsize:] == b"\xff" * fsize:
            return footer, False, "erased"
        want = crc16_ccitt(body[:b["size"] - fsize])
        if footer.size != b["size"]:
            return footer, False, "size %d != %d" % (footer.size, b["size"])
        if footer.signature != self.table["signature"]:
            return footer, False, "signature %#x" % footer.signature
        if footer.block_id != block_id:
            return footer, False, "block id %d != %d" % (footer.block_id, block_id)
        if footer.checksum != want:
            return footer, False, "checksum %#06x != %#06x" % (footer.checksum, want)
        return footer, True, None

    @staticmethod
    def _compare_counters(a, b):
        """SaveCheckInfo_CompareCounters, 0xffffffff wraps to 0."""
        if a == 0xFFFFFFFF and b == 0:
            return -1
        if a == 0 and b == 0xFFFFFFFF:
            return 1
        return (a > b) - (a < b)

    def _compare_sectors(self, info):
        """SaveCheckInfo_CompareSectors: (result, current, stale)."""
        p, bk = info[SECTOR_PRIMARY], info[SECTOR_BACKUP]
        if p[1] and bk[1]:
            g = self._compare_counters(p[0].save_counter, bk[0].save_counter)
            c = self._compare_counters(p[0].block_counter, bk[0].block_counter)
            if g > 0 or (g == 0 and c >= 0):
                return SECTOR_VALID, SECTOR_PRIMARY, SECTOR_BACKUP
            return SECTOR_VALID, SECTOR_BACKUP, SECTOR_PRIMARY
        if p[1]:
            return SECTOR_PARTIAL, SECTOR_PRIMARY, None
        if bk[1]:
            return SECTOR_PARTIAL, SECTOR_BACKUP, None
        return SECTOR_INVALID, SECTOR_PRIMARY, None

    def _arbitrate(self):
        """SaveData_LoadCheck, block by block, and remember what it chose."""
        self.check = {}
        self.erased = []
        for block_id in self.blocks:
            info = {}
            for sector in (SECTOR_PRIMARY, SECTOR_BACKUP):
                footer, ok, why = self._check(sector, block_id)
                info[sector] = (footer, ok, why)
                if not ok and why != "erased":
                    self.errors.append("block %d sector %d: %s" % (block_id, sector, why))
                elif why == "erased":
                    self.erased.append((block_id, sector))
            self.check[block_id] = info

        normal = self._compare_sectors(self.check[0])
        boxes = self._compare_sectors(self.check[1])
        self.current = {0: normal[1], 1: boxes[1]}

        if normal[0] == SECTOR_INVALID and boxes[0] == SECTOR_INVALID:
            self.result = LOAD_EMPTY
        elif SECTOR_INVALID in (normal[0], boxes[0]):
            self.result = LOAD_ERROR
        else:
            gn = self.check[0][normal[1]][0].save_counter
            gb = self.check[1][boxes[1]][0].save_counter
            self.result = LOAD_OK if gn == gb else LOAD_CORRUPT

    @property
    def ok(self):
        return self.result == LOAD_OK and not self.errors

    @property
    def save_counter(self):
        return self.check[0][self.current[0]][0].save_counter

    # pages -----------------------------------------------------------

    def page(self, entry):
        """One save-table page's bytes, from whichever copy arbitration chose."""
        pid = self.ids[entry] if isinstance(entry, str) else entry
        p = self.pages[pid]
        block = self.blocks[p["block"]]
        base = self._sector_base(self.current[p["block"]])
        # location is an offset into the whole body, and the block's own offset
        # is already inside that, exactly as SaveData_SaveTable indexes it.
        start = base + p["location"]
        return self.raw[start:start + p["size"]], block

    # named answers ---------------------------------------------------

    def _u(self, buf, off, width):
        return int.from_bytes(buf[off:off + width], "little")

    @property
    def _player(self):
        return self.page("SAVE_TABLE_ENTRY_PLAYER")[0]

    @property
    def _trainer(self):
        return self._player[self.t.off("PlayerSave", "info"):]

    @property
    def options(self):
        """What the options screen left behind, by the game's own member names.

        The whole struct is one u16 of bitfields, so every field comes out of
        the DWARF's bit position rather than a shift written down here, the
        same rule the egg flag and the form follow.
        """
        raw = self._u(self._player, self.t.off("PlayerSave", "options"), 2)
        out = {}
        for member in ("textSpeed", "soundMode", "battleStyle", "battleScene",
                       "buttonMode", "frame"):
            byte, shift, width = self.t.bit("Options", member)
            out[member] = (raw >> (8 * byte + shift)) & ((1 << width) - 1)
        return out

    @property
    def name(self):
        raw = self._trainer[self.t.off("TrainerInfo", "name"):]
        out = []
        for i in range(0, 16, 2):
            c = int.from_bytes(raw[i:i + 2], "little")
            if c == CHAR_EOS:
                break
            out.append(charcode_to_ascii(c))
        return "".join(out)

    @property
    def trainer_id(self):
        return self._u(self._trainer, self.t.off("TrainerInfo", "id"), 4)

    @property
    def money(self):
        return self._u(self._trainer, self.t.off("TrainerInfo", "money"), 4)

    @property
    def gender(self):
        return self._trainer[self.t.off("TrainerInfo", "gender")]

    @property
    def badge_mask(self):
        return self._trainer[self.t.off("TrainerInfo", "badgeMask")]

    @property
    def badges(self):
        return bin(self.badge_mask).count("1")

    @property
    def play_time(self):
        pt = self._player[self.t.off("PlayerSave", "playTime"):]
        return (self._u(pt, self.t.off("PlayTime", "hours"), 2),
                pt[self.t.off("PlayTime", "minutes")],
                pt[self.t.off("PlayTime", "seconds")])

    @property
    def game_day(self):
        """GameTime.day, the absolute day the game last rolled over on.

        The single number every daily system hangs off: sub_02055A14 compares
        it against today and calls FieldSystem_HandleDailyEvents with the
        difference. Booting one save on two dates and reading this is how a
        rollover is observed without watching anything on screen.
        """
        page = self.page("SAVE_TABLE_ENTRY_SYSTEM")[0]
        gt = self.t.off("SystemData", "gameTime")
        return self._u(page, gt + self.t.off("GameTime", "day"), 4)

    @property
    def game_time(self):
        """GameTime's own calendar, the wall clock the save was written on.

        Every per-minute system is driven from the difference between this and
        the clock the next boot reports, so reading it before and after is how
        an elapsed-minutes count becomes arithmetic a test can check rather
        than a number only the port knows.
        """
        page = self.page("SAVE_TABLE_ENTRY_SYSTEM")[0]
        gt = page[self.t.off("SystemData", "gameTime"):]
        date = gt[self.t.off("GameTime", "date"):]
        time = gt[self.t.off("GameTime", "time"):]
        u = lambda buf, member, struct: self._u(buf, self.t.off(struct, member), 4)
        return {
            "year": u(date, "year", "RTCDate"),
            "month": u(date, "month", "RTCDate"),
            "day_of_month": u(date, "day", "RTCDate"),
            "week": u(date, "week", "RTCDate"),
            "hour": u(time, "hour", "RTCTime"),
            "minute": u(time, "minute", "RTCTime"),
            "second": u(time, "second", "RTCTime"),
            "day": self._u(gt, self.t.off("GameTime", "day"), 4),
        }

    @property
    def minute_of_epoch(self):
        """game_time as whole minutes, for differencing two saves.

        Only ever used as a difference, so the epoch is arbitrary as long as
        the calendar arithmetic is right; the game's own RTC_ConvertDateTime-
        ToSecond is what the port compares, and this agrees with it inside a
        month, which is all any station spans.
        """
        t = self.game_time
        days = t["day"]
        return ((days * 24 + t["hour"]) * 60) + t["minute"]

    @property
    def berry_patches(self):
        """Every berry patch, as the misc save block holds them.

        The count is the array's own, how much room the block gives berries
        before the next member starts, so it follows the header rather than
        being restated.
        """
        page = self.page("SAVE_TABLE_ENTRY_MISC")[0]
        base = self.t.off("MiscSaveBlock", "berryPatches")
        stride = self.t.size("BerryPatch")
        count = (self.t.off("MiscSaveBlock", "persistedMapFeatures") - base) // stride
        out = []
        for i in range(count):
            p = page[base + i * stride:base + (i + 1) * stride]
            o = lambda m: self.t.off("BerryPatch", m)
            out.append({
                "berryID": p[o("berryID")],
                "growthStage": p[o("growthStage")],
                "stageMinutesRemaining": self._u(p, o("stageMinutesRemaining"), 2),
                "moistureMinutesRemaining": self._u(p, o("moistureMinutesRemaining"), 2),
                "replantCount": p[o("replantCount")],
                "yield": self._u(p, o("yield"), 2),
                "moistureRating": p[o("moistureRating")],
                "yieldRating": p[o("yieldRating")],
                "mulchType": p[o("mulchType")],
                "isGrowing": p[o("isGrowing")],
            })
        return out

    @property
    def honey_trees(self):
        """The 21 honey trees' countdowns, in minutes, and what is waiting."""
        page = self.page("SAVE_TABLE_ENTRY_ENCOUNTERS")[0]
        trees = page[self.t.off("SpecialEncounter", "treeStates"):]
        base = self.t.off("PlayerHoneyTreeStates", "honeyTrees")
        stride = self.t.size("HoneyTree")
        count = (self.t.size("PlayerHoneyTreeStates") - base) // stride
        out = []
        for i in range(count):
            h = trees[base + i * stride:base + (i + 1) * stride]
            out.append({
                "minutesRemaining": int.from_bytes(
                    h[self.t.off("HoneyTree", "minutesRemaining"):][:4],
                    "little", signed=True),
                "encounterSlot": h[self.t.off("HoneyTree", "encounterSlot")],
                "encounterGroup": h[self.t.off("HoneyTree", "encounterGroup")],
                "numShakes": h[self.t.off("HoneyTree", "numShakes")],
            })
        return out

    @property
    def position(self):
        """(map header id, x, z, facing), where a continue would spawn."""
        page = self.page("SAVE_TABLE_ENTRY_FIELD_PLAYER_STATE")[0]
        loc = page[self.t.off("FieldOverworldState", "player"):]
        s32 = lambda o: int.from_bytes(loc[o:o + 4], "little", signed=True)
        return (s32(self.t.off("Location", "mapHeaderID")),
                s32(self.t.off("Location", "x")),
                s32(self.t.off("Location", "z")),
                s32(self.t.off("Location", "faceDirection")))

    @property
    def party(self):
        """Every party slot, decrypted, as dicts."""
        page = self.page("SAVE_TABLE_ENTRY_PARTY")[0]
        count = self._u(page, self.t.off("Party", "currentCount"), 4)
        base = self.t.off("Party", "pokemon")
        stride = self.t.size("Pokemon")
        return [self._mon(page[base + i * stride:base + (i + 1) * stride])
                for i in range(min(count, 6))]

    def _boxmon(self, raw):
        """The 136 boxed bytes, decrypted, what a stored Pokemon is.

        Split out of _mon because the day care holds BoxPokemon and nothing
        else: a deposited parent has no party block at all, so a reader that
        only knew how to decode a full Pokemon could not say what is in there.
        """
        t = self.t
        personality = self._u(raw, t.off("BoxPokemon", "personality"), 4)
        checksum = self._u(raw, t.off("BoxPokemon", "checksum"), 2)
        blocks_at = t.off("BoxPokemon", "dataBlocks")
        block_size = t.size("PokemonDataBlockA")
        blob = decode_data(raw[blocks_at:blocks_at + block_size * 4], checksum)
        order = BLOCK_ORDER[((personality & 0x3E000) >> 13) % 24]
        blocks = [blob[order[i] * block_size:(order[i] + 1) * block_size] for i in range(4)]
        a, b = blocks[0], blocks[1]

        moves_at = t.off("PokemonDataBlockB", "moves")
        # form shares a byte with fatefulEncounter and gender, and isEgg shares
        # one with the IVs, so both come out of the DWARF's bit positions
        # rather than shifts written down here.
        form_byte, form_shift, form_width = t.bit("PokemonDataBlockB", "form")
        egg_byte, egg_shift, _ = t.bit("PokemonDataBlockB", "isEgg")
        c = blocks[2]
        return {
            "personality": personality,
            "checksum": checksum,
            "checksum_ok": checksum == sum(
                int.from_bytes(blob[i:i + 2], "little")
                for i in range(0, len(blob), 2)) & 0xFFFF,
            "species": self._u(a, t.off("PokemonDataBlockA", "species"), 2),
            "held_item": self._u(a, t.off("PokemonDataBlockA", "heldItem"), 2),
            "ot_id": self._u(a, t.off("PokemonDataBlockA", "otID"), 4),
            "exp": self._u(a, t.off("PokemonDataBlockA", "exp"), 4),
            # An egg's remaining cycles live in this byte, which is why it
            # matters to a growth station and not only to a friendship one.
            "friendship": a[t.off("PokemonDataBlockA", "friendship")],
            "form": (b[form_byte] >> form_shift) & ((1 << form_width) - 1),
            "is_egg": bool(b[egg_byte] >> egg_shift & 1),
            "moves": [self._u(b, moves_at + 2 * i, 2) for i in range(4)],
            # What a poffin raises and what a contest is judged on. The five
            # conditions and the sheen are one byte each in block A, in the
            # MON_DATA_COOL..MON_DATA_SHEEN order the feeding code walks.
            "cool": a[t.off("PokemonDataBlockA", "cool")],
            "beauty": a[t.off("PokemonDataBlockA", "beauty")],
            "cute": a[t.off("PokemonDataBlockA", "cute")],
            "smart": a[t.off("PokemonDataBlockA", "smart")],
            "tough": a[t.off("PokemonDataBlockA", "tough")],
            "sheen": a[t.off("PokemonDataBlockA", "sheen")],
            # The Super Contest ribbons, one bit each, indexed from
            # MON_DATA_SUPER_COOL_RIBBON; which is how the game reads them
            # too (GetRibbon subtracts that same base).
            "ribbons_super": self._u(c, t.off("PokemonDataBlockC", "ribbonsDS2"), 8),
        }

    @property
    def boxes(self):
        """The PC's 18 boxes: which one is open, and what is in each slot.

        `stored` is the whole reason this exists. A station that deposits two
        Pokemon and asserts only that the party shrank passes just as happily
        if the game released them, and releasing is one menu entry away from
        storing on the same screen.
        """
        page = self.page("SAVE_TABLE_ENTRY_PC_BOXES")[0]
        base = self.t.off("PCBoxes", "boxMons")
        stride = self.t.size("BoxPokemon")
        boxes = []
        for b in range(18):
            slots = []
            for i in range(30):
                at = base + (b * 30 + i) * stride
                mon = self._boxmon(page[at:at + stride])
                slots.append(mon if mon["species"] else None)
            boxes.append(slots)
        return {
            "current": self._u(page, self.t.off("PCBoxes", "currentBoxID"), 4),
            "boxes": boxes,
            "stored": sum(1 for b in boxes for m in b if m),
        }

    @property
    def poffins(self):
        """The poffin case: 100 slots, each a flavour profile or empty.

        An empty slot is not zeroed, Poffin_Clear writes TYPE_NONE, one past
        the last real PoffinType, and Poffin_HasValidFlavor is that comparison
        and nothing else. So `filled` counts slots whose type is a real one,
        which is what PoffinCase_CountFilledSlots counts and what the cooking
        script's CheckCanCookPoffin branches on.
        """
        page = self.page("SAVE_TABLE_ENTRY_POFFINS")[0]
        base = self.t.off("PoffinCase", "slot")
        stride = self.t.size("Poffin")
        fields = ("type", "spiciness", "dryness", "sweetness", "bitterness",
                  "sourness", "smoothness")
        slots = []
        for i in range(100):
            at = base + i * stride
            slot = {f: page[at + self.t.off("Poffin", f)] for f in fields}
            slot["level"] = _poffin_level(slot)
            slots.append(slot)
        return {
            "slots": slots,
            "filled": sum(1 for s in slots if s["type"] != POFFIN_TYPE_NONE),
        }

    @property
    def underground(self):
        """The Underground page: sphere/trap/goods bags and the secret base.

        Empty slots are the NONE enumerator, which is 0 for every one of
        these bags (SPHERE_NONE, TRAP_NONE, UG_GOOD_NONE). `filled` counts
        the rest, which is what Underground_GetSphereCount and friends
        walk. The base is active once SecretBase_SetEntrance has run.
        """
        page = self.page("SAVE_TABLE_ENTRY_UNDERGROUND")[0]
        t = self.t
        types_at = t.off("Underground", "sphereTypes")
        sizes_at = t.off("Underground", "sphereSizes")
        spheres = [{"type": page[types_at + i], "size": page[sizes_at + i]}
                   for i in range(40)]
        traps_at = t.off("Underground", "traps")
        traps = [page[traps_at + i] for i in range(40)]
        goods_at = t.off("Underground", "goodsBag")
        goods = [page[goods_at + i] for i in range(40)]
        base_at = t.off("Underground", "secretBase")
        base = {
            "x": self._u(page, base_at + t.off("SecretBase", "entranceX"), 2),
            "z": self._u(page, base_at + t.off("SecretBase", "entranceZ"), 2),
            "dir": page[base_at + t.off("SecretBase", "entranceDir")],
            "active": page[base_at + t.off("SecretBase", "active")],
        }
        return {
            "spheres": spheres,
            "sphere_filled": sum(1 for s in spheres if s["type"]),
            "traps": traps,
            "trap_filled": sum(1 for v in traps if v),
            "goods": goods,
            "good_filled": sum(1 for v in goods if v),
            "base": base,
        }

    @property
    def daycare(self):
        """The two parents, their step counters and the waiting egg.

        `offspring` is the whole of "an egg is waiting": Daycare_HasEgg is that
        field being non-zero, and the day-care man's script reads nothing else
        to decide whether he has one to hand over. `state` restates
        Daycare_GetState, which is what a script actually branches on.
        """
        page = self.page("SAVE_TABLE_ENTRY_DAYCARE")[0]
        base = self.t.off("Daycare", "mons")
        stride = self.t.size("DaycareMon")
        mons = []
        for i in range(2):
            slot = page[base + i * stride:base + (i + 1) * stride]
            mon = self._boxmon(slot[self.t.off("DaycareMon", "boxMon"):])
            mon["steps"] = self._u(slot, self.t.off("DaycareMon", "steps"), 4)
            mons.append(mon)
        offspring = self._u(page, self.t.off("Daycare", "offspringPersonality"), 4)
        occupied = sum(1 for m in mons if m["species"])
        return {
            "mons": mons,
            "offspring": offspring,
            "counter": page[self.t.off("Daycare", "stepCounter")],
            "count": occupied,
            "state": _daycare("DAYCARE_EGG_WAITING") if offspring
                     else (occupied + 1 if occupied
                           else _daycare("DAYCARE_NO_MONS")),
        }

    def dex(self, species):
        """"caught", "seen" or "no" for one species, the way the dex reads it.

        Pokedex_HasCaughtSpecies wants BOTH bits, and the bit index is the
        species number minus one, ReadBit_2Forms decrements before it shifts.
        """
        page = self.page("SAVE_TABLE_ENTRY_POKEDEX")[0]
        def bit(member):
            base = self.t.off("Pokedex", member) + (species - 1) // 8
            return bool(page[base] & (1 << ((species - 1) % 8)))
        if bit("caughtPokemon") and bit("seenPokemon"):
            return "caught"
        return "seen" if bit("seenPokemon") else "no"

    def _mon(self, raw):
        t = self.t
        personality = self._u(raw, t.off("BoxPokemon", "personality"), 4)
        party_at = t.off("Pokemon", "party")
        party_size = t.size("PartyPokemon")
        pd = decode_data(raw[party_at:party_at + party_size], personality)

        mon = self._boxmon(raw)
        mon.update({
            "level": pd[t.off("PartyPokemon", "level")],
            "hp": self._u(pd, t.off("PartyPokemon", "hp"), 2),
            "max_hp": self._u(pd, t.off("PartyPokemon", "maxHP"), 2),
            "attack": self._u(pd, t.off("PartyPokemon", "attack"), 2),
            "defense": self._u(pd, t.off("PartyPokemon", "defense"), 2),
            "speed": self._u(pd, t.off("PartyPokemon", "speed"), 2),
            "sp_atk": self._u(pd, t.off("PartyPokemon", "spAtk"), 2),
            "sp_def": self._u(pd, t.off("PartyPokemon", "spDef"), 2),
        })
        return mon

    @property
    def bag(self):
        """{item id: quantity} across every pocket."""
        page = self.page("SAVE_TABLE_ENTRY_BAG")[0]
        out = {}
        entry = self.t.size("BagItem")
        for pocket, _bit, _w in self.t.records["Bag"][1]:
            if pocket == "registeredItem":
                continue
            off = self.t.off("Bag", pocket)
            for i in range(off, len(page), entry):
                item = self._u(page, i, 2)
                qty = self._u(page, i + 2, 2)
                if item == 0:
                    break
                out[item] = out.get(item, 0) + qty
        return out

    def flag(self, flag_id):
        page = self.page("SAVE_TABLE_ENTRY_VARS_FLAGS")[0]
        base = self.t.off("VarsFlags", "flags")
        return bool(page[base + flag_id // 8] & (1 << (flag_id % 8)))

    def var(self, var_id):
        """By the VAR_* id the scripts use; VARS_START is subtracted here the
        way VarsFlags_GetVarAddress subtracts it."""
        page = self.page("SAVE_TABLE_ENTRY_VARS_FLAGS")[0]
        idx = var_id - VARS_START if var_id >= VARS_START else var_id
        return self._u(page, self.t.off("VarsFlags", "vars") + 2 * idx, 2)

    @property
    def poketch(self):
        """The Poketch page: which app is showing, and the handful of apps
        that write back into the save (steps, alarm, colour, calendar)."""
        page = self.page("SAVE_TABLE_ENTRY_POKETCH")[0]
        t = self.t

        def bit(member):
            byte, shift, width = t.bit("Poketch", member)
            return (page[byte] >> shift) & ((1 << width) - 1)

        app = page[t.off("Poketch", "appIndex")]
        if app >= 128:
            app -= 256
        return {
            "enabled": bit("poketchEnabled"),
            "pedometer": bit("pedometerEnabled"),
            "dotart": bit("dotArtModifiedByPlayer"),
            "color": bit("screenColor"),
            "app": app,
            "count": page[t.off("Poketch", "appCount")],
            "steps": self._u(page, t.off("Poketch", "stepCount"), 4),
            "alarm": bit("alarmSet"),
            "alarm-hour": bit("alarmHour"),
            "alarm-minute": bit("alarmMinute"),
            "calendar-month": page[t.off("Poketch", "calendarMonth")],
            "calendar-marks": self._u(page, t.off("Poketch", "calendarMarkBitmap"), 4),
        }

    # report ----------------------------------------------------------

    def summary(self):
        h, m, s = self.play_time
        map_id, x, z, facing = self.position
        return {
            "path": self.path,
            "result": self.result,
            "errors": self.errors,
            "save_counter": self.save_counter,
            "current_sector": {str(k): v for k, v in self.current.items()},
            "name": self.name,
            "trainer_id": self.trainer_id,
            "gender": self.gender,
            "money": self.money,
            "badges": self.badges,
            "badge_mask": self.badge_mask,
            "play_time": "%d:%02d:%02d" % (h, m, s),
            "position": {"map": map_id, "x": x, "z": z, "facing": facing},
            "party": self.party,
            "bag": self.bag,
        }


# Poffin_CalcLevel, restated: the type picks which flavour is the level, in
# blocks of five, and the sixth block (the four one-off types, rich,
# overripe, foul, mild) takes the largest of them. Capped at 99 there and here.
# An empty slot is TYPE_NONE, which poffin.c writes as one past the last real
# type rather than as a zero, so the count is read off the same comparison.
POFFIN_TYPE_NONE = 30
_POFFIN_FLAVOURS = ("spiciness", "dryness", "sweetness", "bitterness", "sourness")


def _poffin_level(slot):
    block = slot["type"] // 5
    if block < len(_POFFIN_FLAVOURS):
        level = slot[_POFFIN_FLAVOURS[block]]
    else:
        level = max(slot[f] for f in _POFFIN_FLAVOURS)
    return min(level, 99)


# The Latin block of the game's charcode enum, read from the enum rather than
# guessed, so a charset change upstream moves this with it.
def _charcode_bases():
    path = os.path.join(ROOT, "include", "constants", "charcode.h")
    with open(path) as f:
        text = f.read()
    body = re.search(r"enum\s+CharCode\s*\{(.*?)\n\};", text, re.S)
    if body is None:
        body = re.search(r"\{(.*)\}", text, re.S)
    names, n = {}, 0
    for line in body.group(1).splitlines():
        m = re.match(r"\s*(CHAR_\w+)\s*(?:=\s*(\S+?))?\s*,?\s*(?://.*)?$", line)
        if not m:
            continue
        if m.group(2):
            n = int(m.group(2), 0)
        names[m.group(1)] = n
        n += 1
    return names


def _daycare(name):
    """A DAYCARE_* state, from the header that defines it."""
    path = os.path.join(ROOT, "include", "constants", "daycare.h")
    with open(path) as f:
        m = re.search(r"^#define\s+%s\s+(\d+)" % name, f.read(), re.M)
    if m is None:
        raise SystemExit("pc_save: %s is not in constants/daycare.h" % name)
    return int(m.group(1))


def _vars_start():
    """VARS_START, from the generated header the build just wrote."""
    path = os.path.join(ROOT, "build", "pc", "geninclude", "generated", "vars_flags.h")
    with open(path) as f:
        m = re.search(r"^\s*VARS_START\s*=\s*(\d+)", f.read(), re.M)
    return int(m.group(1)) if m else 0


VARS_START = _vars_start()
CHARCODES = _charcode_bases()
CHAR_EOS = CHARCODES["CHAR_EOS"]


def ascii_to_charcode(c):
    if c.isdigit():
        return CHARCODES["CHAR_0"] + int(c)
    if "A" <= c <= "Z":
        return CHARCODES["CHAR_A"] + ord(c) - ord("A")
    if "a" <= c <= "z":
        return CHARCODES["CHAR_a"] + ord(c) - ord("a")
    return CHARCODES["CHAR_SPACE"]


def charcode_to_ascii(code):
    for base, first, count in (("CHAR_0", "0", 10), ("CHAR_A", "A", 26),
                               ("CHAR_a", "a", 26)):
        b = CHARCODES[base]
        if b <= code < b + count:
            return chr(ord(first) + code - b)
    return " " if code == CHARCODES["CHAR_SPACE"] else "?"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("save")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    save = Save(args.save)
    if args.json:
        json.dump(save.summary(), sys.stdout, indent=2)
        print()
    else:
        s = save.summary()
        print("%s: %s (counter %d)" % (s["path"], s["result"], s["save_counter"]))
        for e in s["errors"]:
            print("  ! %s" % e)
        print("  %s (id %d, %s), $%d, %d badge(s), %s" % (
            s["name"], s["trainer_id"], "girl" if s["gender"] else "boy",
            s["money"], s["badges"], s["play_time"]))
        p = s["position"]
        print("  map %d at (%d, %d) facing %d" % (p["map"], p["x"], p["z"], p["facing"]))
        for i, mon in enumerate(s["party"]):
            print("  party %d: species %d level %d, %d/%d HP, moves %s%s" % (
                i, mon["species"], mon["level"], mon["hp"], mon["max_hp"],
                mon["moves"], "" if mon["checksum_ok"] else "  (CHECKSUM BAD)"))
        if s["bag"]:
            print("  bag: %s" % ", ".join("%d x%d" % (k, v) for k, v in sorted(s["bag"].items())))
    return 0 if save.ok else 1


if __name__ == "__main__":
    sys.exit(main())
