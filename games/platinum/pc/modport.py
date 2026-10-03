#!/usr/bin/env python3
"""Port content from another NDS ROM into a runtime package.

The source is a ROM you supply. A decomp tree tells this tool where a
file lives; it is not the bytes.

    python3 pc/modport.py --rom heartgold.us.nds --identify
    python3 pc/modport.py --rom heartgold.us.nds --list
    python3 pc/modport.py --rom heartgold.us.nds --out pkg --file a/0/0/4
    python3 pc/modport.py --rom heartgold.us.nds --out pkg \\
        --narc a/0/0/4 --member 915 --dest poketool/pokegra/pl_pokegra.narc
    python3 pc/modport.py --rom heartgold.us.nds --out pkg --pokemon chikorita
    python3 pc/modport.py --rom heartgold.us.nds --out pkg \\
        --pokemon chikorita --as piplup

Measured 2026-08-16 against the four NDS images this tree can see:

    Cpue  pokemon pl   poketool/pokegra/pl_pokegra.narc   2964 members
    Adae  pokemon d    poketool/pokegra/pokegra.narc      2964 members
    Ipke  pokemon hg   a/0/0/4                            2964 members
    Irbo  pokemon b    a/0/0/4                            14285 members
                       (Gen-5 layout, --pokemon refuses)

Same 6-member face order as make_pl_pokegra.py / modcook.py:
female_back, male_back, female_front, male_front, normal.pal, shiny.pal.
Chikorita male_front (idx 915) differs across all three Gen-4 images;
Piplup male_front (idx 2361) is byte-identical between Platinum and
HeartGold. HeartGold's 6448-byte NCGR draws through Platinum's
pokemon_sprite.c: PC_LAB_SPRITE=393 on `--pokemon chikorita --as
piplup` is HeartGold Chikorita (char_fnv=efc6c20d1c9fcf8b), not
Platinum Piplup (0ab387a09e642c50). Diamond's same-size NCGR does
not (PC_LAB_SPRITE=1 fills the canvas) so --pokemon from ADAE
still extracts and warns; drawing it is a conversion that has not
been measured. A catalog entry that was not measured is not listed,
SoulSilver, Pearl, White, and every regional twin refuse --pokemon
until a ROM of that code is in front of this script.

--text and --map refuse: Diamond msg.narc is 624 banks against
Platinum's 724, HeartGold has no land_data.narc, and no conversion
oracle has been measured. Raw --file / --narc still extract those
bytes for a later recipe.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# NDS cartridge header 0xC0..0x15B. Same 156 bytes on every ROM
# measured this run (Platinum, Diamond, HeartGold, Black). A GBA
# image puts its logo at 0x04; a loose NARC starts with "NARC"; a
# decomp files/ tree is a directory. All three fail this check.
NDS_LOGO = bytes.fromhex(
    "24ffae51699aa2213d84820a84e409ad"
    "11248b98c0817f21a352be199309ce20"
    "10464a4af82731ec58c7e83382e3cebf"
    "85f4df94ce4b09c194568ac01372a7fc"
    "9f844d73a3ca9a615897a327fc039876"
    "231dc7610304ae56bf38840040a70e"
    "fdff52fe036f9530f197fbc08560d680"
    "25a963be03014e38e2f9a234ffbb3e03"
    "44780090cb88113a9465c07c6387f03c"
    "afd625e48b380aac7221d4f807"
)

PL_POKEGRA = "poketool/pokegra/pl_pokegra.narc"
POKEGRA_MEMBERS = 2964  # 494 species * 6; SPECIES_NONE..ARCEUS
POKEGRA_VANILLA_SPECIES = 494

# Only game codes whose ROM was opened this run. Title is recorded
# so --identify can print what the header actually said.
# pokemon is the Nitro path of the 2964-member sprite NARC, or None
# when the layout is not the Gen-4 six-member one.
GAMES = {
    "CPUE": {
        "name": "platinum",
        "title": "POKEMON PL",
        "pokemon": PL_POKEGRA,
        "pokemon_count": POKEGRA_MEMBERS,
    },
    "ADAE": {
        "name": "diamond",
        "title": "POKEMON D",
        "pokemon": "poketool/pokegra/pokegra.narc",
        "pokemon_count": POKEGRA_MEMBERS,
        # Same 6448-byte RGCN as Platinum / HeartGold. PC_LAB_SPRITE=1
        # on a --pokemon 1 package fills the 160x80 dump (11872 of
        # 12800 pixels off the background). Extract still writes;
        # cmd_pokemon warns. Do not invent a DP scramble here.
        "pokemon_draw": False,
    },
    "IPKE": {
        "name": "heartgold",
        "title": "POKEMON HG",
        "pokemon": "a/0/0/4",
        "pokemon_count": POKEGRA_MEMBERS,
    },
    "IRBO": {
        "name": "black",
        "title": "POKEMON B",
        "pokemon": None,
        "pokemon_count": 14285,
        "pokemon_refuse": "Gen-5 pokegra (a/0/0/4 has 14285 members, "
                          "not the Gen-4 2964)",
    },
}


def die(msg: str) -> None:
    print("port: " + msg, file=sys.stderr)
    sys.exit(2)


def load_species_ids() -> dict[str, int]:
    path = ROOT / "generated" / "species.txt"
    if not path.is_file():
        die("%s not found" % path)
    ids: dict[str, int] = {}
    for i, raw in enumerate(path.read_text().splitlines()):
        name = raw.strip()
        if name.startswith("SPECIES_"):
            ids[name[len("SPECIES_"):].lower()] = i
    if not ids:
        die("generated/species.txt has no SPECIES_ entries")
    return ids


def resolve_species(token: str, ids: dict[str, int]) -> int:
    raw = token.strip().lower()
    if raw.startswith("species_"):
        raw = raw[len("species_"):]
    if raw.isdigit():
        n = int(raw)
    elif raw in ids:
        n = ids[raw]
    else:
        die("unknown species '%s'" % token)
    if n < 0 or n >= POKEGRA_VANILLA_SPECIES:
        die("species %d is outside the Gen-4 pokegra (0..493)" % n)
    return n


class NitroRom:
    """One NDS image: header, NitroFS, and NARC members on demand."""

    def __init__(self, path: Path):
        self.path = path
        if path.is_dir():
            die("%s is a directory; pass an NDS ROM (the same way "
                "the retail client does), not a decomp tree" % path)
        if not path.is_file():
            die("need --rom PATH (an NDS ROM; a decomp tree is not "
                "a source): %s" % path)
        data = path.read_bytes()
        if len(data) < 0x160:
            die("%s is too small to be an NDS ROM (%d bytes)"
                % (path, len(data)))
        if data[0xC0:0xC0 + 156] != NDS_LOGO:
            die("%s is not an NDS ROM (Nintendo logo missing)" % path)
        fnt_off, fnt_sz, fat_off, fat_sz = struct.unpack_from(
            "<IIII", data, 0x40)
        end = len(data)
        if (fnt_off + fnt_sz > end or fat_off + fat_sz > end
                or fnt_sz < 8 or fat_sz < 8):
            die("%s has a broken NitroFS table" % path)
        self.data = data
        self.title = data[0:12].split(b"\x00", 1)[0].decode(
            "ascii", "replace")
        self.code = data[0x0C:0x10].decode("ascii", "replace")
        self.maker = data[0x10:0x12].decode("ascii", "replace")
        self.fnt = data[fnt_off:fnt_off + fnt_sz]
        self.fat = data[fat_off:fat_off + fat_sz]
        self.files = self._walk()
        self.game = GAMES.get(self.code)

    def _walk(self) -> dict[str, tuple[int, int]]:
        fnt = self.fnt
        fat = self.fat

        def dir_off(did: int) -> tuple[int, int, int]:
            return struct.unpack_from("<IHH", fnt, (did & 0xFFF) * 8)

        found: dict[str, tuple[int, int]] = {}

        def walk(did: int, prefix: str) -> None:
            off, first, _parent = dir_off(did)
            p = off
            fid = first
            while p < len(fnt) and fnt[p] != 0:
                flag = fnt[p]
                p += 1
                namelen = flag & 0x7F
                name = fnt[p:p + namelen].decode("ascii", "replace")
                p += namelen
                if flag & 0x80:
                    sub = struct.unpack_from("<H", fnt, p)[0]
                    p += 2
                    walk(sub, prefix + name + "/")
                else:
                    start, end = struct.unpack_from("<II", fat, fid * 8)
                    found[prefix + name] = (start, end - start)
                    fid += 1

        walk(0xF000, "")
        return found

    def file_bytes(self, nitro_path: str) -> bytes:
        loc = self.files.get(nitro_path)
        if loc is None:
            die("%s has no '%s'" % (self.path, nitro_path))
        start, size = loc
        return self.data[start:start + size]

    def narc_members(self, nitro_path: str) -> list[bytes]:
        blob = self.file_bytes(nitro_path)
        if blob[:4] != b"NARC":
            die("%s '%s' is not a NARC (magic %r)"
                % (self.path, nitro_path, blob[:4]))
        off = 16
        fat = None
        gmif = None
        while off + 8 <= len(blob):
            magic = blob[off:off + 4]
            size = struct.unpack_from("<I", blob, off + 4)[0]
            if size < 8:
                break
            if magic == b"BTAF":
                n = struct.unpack_from("<I", blob, off + 8)[0]
                fat = [
                    struct.unpack_from("<II", blob, off + 12 + i * 8)
                    for i in range(n)
                ]
            elif magic == b"GMIF":
                gmif = off + 8
            off += size
        if fat is None or gmif is None:
            die("%s '%s' has no BTAF/GMIF" % (self.path, nitro_path))
        return [blob[gmif + a:gmif + b] for a, b in fat]


def identify_line(rom: NitroRom) -> str:
    game = rom.game
    if game is None:
        return ("port: %s %s (unlisted; --file/--narc only, no "
                "pokemon catalog)" % (rom.code, rom.title))
    poke = game.get("pokemon")
    if poke:
        extra = " pokemon=%s (%d)" % (poke, game["pokemon_count"])
    else:
        extra = " pokemon=refused (%s)" % game.get(
            "pokemon_refuse", "no catalog")
    return "port: %s %s \"%s\"%s" % (
        rom.code, game["name"], rom.title, extra)


def write_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def ensure_toml(pkg: Path) -> None:
    toml = pkg / "mod.toml"
    if toml.is_file():
        return
    ident = "".join(
        ch if ch.isalnum() or ch == "_" else "_"
        for ch in pkg.name.lower()
    ) or "ported"
    toml.write_text(
        'id = "%s"\n'
        'name = "%s"\n'
        'version = "1.0.0"\n'
        % (ident, pkg.name)
    )


def write_member(pkg: Path, dest_narc: str, idx: int, data: bytes) -> Path:
    # Package-root narc/<nitro-path>/<idx> is what pc_modfs walks.
    # replace/ is whole Nitro files only, a member planted there
    # would be claimed as a file named "narc/...".
    out = pkg / "narc" / dest_narc / str(idx)
    write_bytes(out, data)
    return out


def write_file(pkg: Path, dest_path: str, data: bytes) -> Path:
    out = pkg / "replace" / dest_path
    write_bytes(out, data)
    return out


def append_log(pkg: Path, line: str) -> None:
    log = pkg / "port.log"
    prev = log.read_text() if log.is_file() else ""
    log.write_text(prev + line + "\n")


def cmd_identify(rom: NitroRom) -> int:
    print(identify_line(rom))
    return 0


def cmd_list(rom: NitroRom) -> int:
    for name in sorted(rom.files):
        _start, size = rom.files[name]
        print("%8d  %s" % (size, name))
    return 0


def cmd_file(rom: NitroRom, pkg: Path, src: str, dest: str | None) -> int:
    dest_path = dest or src
    data = rom.file_bytes(src)
    out = write_file(pkg, dest_path, data)
    ensure_toml(pkg)
    append_log(pkg, "file %s:%s -> replace/%s (%d)"
               % (rom.code, src, dest_path, len(data)))
    print("port: wrote %s (%d bytes)" % (out, len(data)))
    return 0


def cmd_narc(rom: NitroRom, pkg: Path, src: str, member: int,
             dest: str | None) -> int:
    members = rom.narc_members(src)
    if member < 0 or member >= len(members):
        die("%s '%s' has %d members; %d is out of range"
            % (rom.path, src, len(members), member))
    dest_path = dest or src
    data = members[member]
    out = write_member(pkg, dest_path, member if dest is None else member,
                       data)
    # When remapping the archive, the dest index is still `member`
    # unless a pokemon recipe chose otherwise. Raw --narc keeps the
    # source index so a later cook/overlay can address the same slot.
    ensure_toml(pkg)
    append_log(pkg, "narc %s:%s/%d -> narc/%s/%d (%d)"
               % (rom.code, src, member, dest_path, member, len(data)))
    print("port: wrote %s (%d bytes)" % (out, len(data)))
    return 0


def cmd_pokemon(rom: NitroRom, pkg: Path, token: str,
                as_token: str | None) -> int:
    game = rom.game
    if game is None:
        die("%s (%s) has no pokemon catalog; measured codes are %s"
            % (rom.code, rom.title, ", ".join(sorted(GAMES))))
    if not game.get("pokemon"):
        die("%s (%s): %s"
            % (rom.code, game["name"],
               game.get("pokemon_refuse", "no pokemon catalog")))
    src_narc = game["pokemon"]
    members = rom.narc_members(src_narc)
    if len(members) != game["pokemon_count"]:
        die("%s '%s' has %d members, catalog expected %d"
            % (rom.path, src_narc, len(members), game["pokemon_count"]))
    ids = load_species_ids()
    src_id = resolve_species(token, ids)
    dest_id = resolve_species(as_token, ids) if as_token else src_id
    wrote = 0
    for face in range(6):
        src_idx = src_id * 6 + face
        dest_idx = dest_id * 6 + face
        data = members[src_idx]
        write_member(pkg, PL_POKEGRA, dest_idx, data)
        wrote += 1
    ensure_toml(pkg)
    append_log(pkg, "pokemon %s:%s %s (%d) -> %s (%d) %s/%d..%d"
               % (rom.code, game["name"], token, src_id,
                  as_token or token, dest_id, PL_POKEGRA,
                  dest_id * 6, dest_id * 6 + 5))
    print("port: %s %s (%d) -> %s members %d..%d (%d files)"
          % (game["name"], token, src_id, PL_POKEGRA,
             dest_id * 6, dest_id * 6 + 5, wrote))
    if game.get("pokemon_draw") is False:
        print("port: warning: %s 6448-byte NCGR does not decode "
              "through Platinum's sprite path (PC_LAB_SPRITE fills "
              "the canvas); --narc still copies the raw bytes"
              % game["name"], file=sys.stderr)
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Port content from an NDS ROM into a runtime package. "
                    "The ROM is required; a decomp tree is not a source.")
    ap.add_argument("--rom", metavar="NDS",
                    help="source NDS ROM (required, like the retail client)")
    ap.add_argument("--out", metavar="PKG",
                    help="destination package directory")
    ap.add_argument("--identify", action="store_true",
                    help="print the game code and catalog line")
    ap.add_argument("--list", action="store_true",
                    help="list NitroFS paths")
    ap.add_argument("--file", metavar="PATH",
                    help="extract one Nitro file into replace/")
    ap.add_argument("--narc", metavar="PATH",
                    help="Nitro path of a NARC to take a member from")
    ap.add_argument("--member", type=int, metavar="N",
                    help="NARC member index (with --narc)")
    ap.add_argument("--dest", metavar="PATH",
                    help="destination Nitro path (default: the source path)")
    ap.add_argument("--pokemon", metavar="NAME",
                    help="extract one species' six pl_pokegra members")
    ap.add_argument("--as", dest="as_species", metavar="NAME",
                    help="destination species for --pokemon "
                         "(default: same number)")
    ap.add_argument("--text", metavar="BANK",
                    help="not measured; refuses")
    ap.add_argument("--map", metavar="ID",
                    help="not measured; refuses")
    args = ap.parse_args(argv)

    if args.text is not None:
        die("--text is not in this pipeline: Diamond msg.narc is 624 "
            "banks and HeartGold has no pl_msg.narc; no conversion "
            "oracle has been measured. Use --narc to copy raw bytes")
    if args.map is not None:
        die("--map is not in this pipeline: HeartGold has no "
            "land_data.narc and Diamond's land_data_release.narc is "
            "578 members against Platinum's 666. Use --narc to copy "
            "raw bytes")

    if not args.rom:
        die("need --rom PATH (an NDS ROM; a decomp tree is not a source)")

    rom = NitroRom(Path(args.rom))

    if args.identify:
        return cmd_identify(rom)
    if args.list:
        return cmd_list(rom)

    selectors = sum(1 for v in (args.file, args.narc, args.pokemon) if v)
    if selectors == 0:
        die("nothing to extract (pass --identify, --list, --file, "
            "--narc --member, or --pokemon)")
    if selectors > 1:
        die("one of --file / --narc / --pokemon per invocation")
    if args.narc is not None and args.member is None:
        die("--narc needs --member N")
    if args.member is not None and args.narc is None:
        die("--member needs --narc PATH")
    if args.as_species and not args.pokemon:
        die("--as is for --pokemon")
    if not args.out:
        die("need --out PKG (the destination package directory)")

    pkg = Path(args.out)
    pkg.mkdir(parents=True, exist_ok=True)

    if args.file:
        return cmd_file(rom, pkg, args.file, args.dest)
    if args.narc is not None:
        return cmd_narc(rom, pkg, args.narc, args.member, args.dest)
    return cmd_pokemon(rom, pkg, args.pokemon, args.as_species)


if __name__ == "__main__":
    sys.exit(main())
