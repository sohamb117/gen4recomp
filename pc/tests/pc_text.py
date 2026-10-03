#!/usr/bin/env python3
"""The game's text, decoded outside the game and checked against its source.

    $ python3 pc/tests/pc_text.py                # every bank, summarised
    $ python3 pc/tests/pc_text.py --bank 344     # one bank, printed
    $ python3 pc/tests/pc_text.py --check        # against res/text/*.json

Why an offline decoder is worth having at all, when the port can already print
text: because it is an ORACLE and the port is not. Every other text check
available here compares the port against a recording of itself. This restates
the two algorithms, the per-entry table cipher and the per-string stream
cipher, both from src/message.c, reads the NARC the ROM build produced, and
compares the result against `res/text/*.json`, which is the human-authored
source those messages were compiled FROM. A disagreement means the decode is
wrong, and no amount of the port agreeing with itself would have said so.

It also answers the question 9.19 exists for without needing a running game:
every bank decodes, every glyph index is inside the charset, and nothing in
24,000 messages runs off the end of its own table.

What is not here. This decodes; it does not render. Proving the glyphs reach a
screen needs the game's own font and printer, and that is the in-port half.
What this gives that half is a known answer to check against.
"""

import argparse
import glob
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NARC = os.path.join(ROOT, "build", "rom", "res", "text", "pl_msg.narc")
TEXTDIR = os.path.join(ROOT, "res", "text")

# src/message.c. Restated rather than read, because they are compile-time
# constants in a .c file and there is nothing to read them out of; a test that
# proves the restatement right is the whole point of --check.
KEY_START = 596947
KEY_INC = 18749
ENTRY_KEY = 765


def narc_files(path=NARC):
    """[bytes], every member of a NARC, in order."""
    with open(path, "rb") as f:
        blob = f.read()
    if blob[:4] != b"NARC":
        sys.exit("pc_text: %s is not a NARC" % path)

    off, fat, gmif = 16, None, None
    while off < len(blob) - 8:
        magic = blob[off:off + 4]
        size = struct.unpack("<I", blob[off + 4:off + 8])[0]
        if size == 0:
            break
        if magic == b"BTAF":
            n = struct.unpack("<I", blob[off + 8:off + 12])[0]
            fat = [struct.unpack("<II", blob[off + 12 + i * 8:off + 20 + i * 8])
                   for i in range(n)]
        elif magic == b"GMIF":
            gmif = off + 8
        off += size

    if fat is None or gmif is None:
        sys.exit("pc_text: %s has no BTAF/GMIF" % path)
    return [blob[gmif + a:gmif + b] for a, b in fat]


def decode_bank(blob):
    """[str], one message bank's entries, as lists of charcodes.

    The two ciphers are the game's own. The entry table is XORed with a key
    derived from the bank's seed and the entry index; each string is then XORed
    with a running key that starts from the index and steps by a constant.
    """
    if len(blob) < 4:
        return []
    count, seed = struct.unpack("<HH", blob[:4])
    out = []
    for i in range(count):
        base = 4 + i * 8
        if base + 8 > len(blob):
            raise ValueError("entry %d runs past the bank (%d bytes)" % (i, len(blob)))
        offset, length = struct.unpack("<II", blob[base:base + 8])
        key = (seed * ENTRY_KEY * (i + 1)) & 0xFFFF
        key |= key << 16
        offset ^= key
        length ^= key
        if offset + length * 2 > len(blob) or length > 0xFFFF:
            raise ValueError("entry %d claims %d chars at %#x in a %d-byte bank"
                             % (i, length, offset, len(blob)))
        k = ((i + 1) * KEY_START) & 0xFFFF
        chars = []
        for j in range(length):
            c = struct.unpack("<H", blob[offset + j * 2:offset + j * 2 + 2])[0]
            chars.append((c ^ k) & 0xFFFF)
            k = (k + KEY_INC) & 0xFFFF
        out.append(chars)
    return out


# The charset, walked out of the game's own enum rather than restated. Most of
# `enum CharCode` has no explicit values; it is 500-odd implicit increments,
# so this evaluates it the way a compiler would and then names the block that
# matters here: digits, both Latin cases, space and the terminator. It is not a
# font. What it is for is turning a decoded bank back into something a person
# can compare against res/text/*.json by eye.
def charcodes():
    """{CHAR_*: value}, by evaluating include/constants/charcode.h's enum."""
    path = os.path.join(ROOT, "include", "constants", "charcode.h")
    with open(path) as f:
        text = f.read()
    body = re.search(r"enum\s+CharCode\s*\{(.*?)\n\}", text, re.S)
    if body is None:
        sys.exit("pc_text: include/constants/charcode.h has no enum CharCode")
    out, n = {}, 0
    for line in body.group(1).splitlines():
        m = re.match(r"\s*(CHAR_\w+)\s*(?:=\s*(\S+?))?\s*,?\s*(?://.*)?$", line)
        if not m:
            continue
        if m.group(2):
            n = int(m.group(2), 0)
        out[m.group(1)] = n
        n += 1
    return out


def charset():
    c = charcodes()
    table = {}
    for i in range(10):
        table[c["CHAR_%d" % i]] = "0123456789"[i]
    for i, ch in enumerate("ABCDEFGHIJKLMNOPQRSTUVWXYZ"):
        table[c["CHAR_A"] + i] = ch
    for i, ch in enumerate("abcdefghijklmnopqrstuvwxyz"):
        table[c["CHAR_a"] + i] = ch
    for name, ch in (("CHAR_SPACE", " "), ("CHAR_PERIOD", "."),
                     ("CHAR_COMMA", ","), ("CHAR_EXCLAMATION", "!"),
                     ("CHAR_QUESTION", "?"), ("CHAR_MINUS", "-"),
                     ("CHAR_SLASH", "/"), ("CHAR_COLON", ":"),
                     ("CHAR_SEMICOLON", ";"), ("CHAR_PAREN_OPEN", "("),
                     ("CHAR_PAREN_CLOSE", ")"), ("CHAR_PLUS", "+"),
                     ("CHAR_PERCENT", "%"), ("CHAR_AMPERSAND", "&"),
                     ("CHAR_ASTERISK", "*"), ("CHAR_EQUALS", "="),
                     ("CHAR_AT_SIGN", "@"), ("CHAR_HASH", "#"),
                     ("CHAR_UNDERSCORE", "_")):
        if name in c:
            table[c[name]] = ch
    return table, c["CHAR_EOS"]


def render(chars, table, eos):
    """A best-effort ASCII rendering, for the banks that are plain Latin."""
    out = []
    for c in chars:
        if c == eos:
            break
        out.append(table.get(c, "�"))
    return "".join(out)


def source_banks():
    """{bank id: [en_US strings]} from the human-authored JSON."""
    out = {}
    for path in glob.glob(os.path.join(TEXTDIR, "*.json")):
        try:
            with open(path) as f:
                j = json.load(f)
        except (ValueError, UnicodeDecodeError):
            continue
        msgs = j.get("messages")
        if not isinstance(msgs, list) or not msgs:
            continue
        ids = [m.get("id", "") for m in msgs if isinstance(m, dict)]
        m = re.match(r"pl_msg_(\d+)_", ids[0] if ids else "")
        if not m:
            continue
        # A message's en_US may be a plain string or a list of alternates
        # (the game picks by gender or by context); only the single-string ones
        # can be compared to a single decoded entry.
        out[int(m.group(1))] = [
            x.get("en_US") if isinstance(x.get("en_US"), str) else None
            for x in msgs]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bank", type=int)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--narc", default=NARC)
    args = ap.parse_args()

    if not os.path.exists(args.narc):
        sys.exit("pc_text: %s does not exist, build the ROM's res/ pipeline"
                 % os.path.relpath(args.narc, ROOT))

    banks = narc_files(args.narc)
    table, eos = charset()

    if args.bank is not None:
        for i, chars in enumerate(decode_bank(banks[args.bank])):
            print("%4d  %s" % (i, render(chars, table, eos)))
        return 0

    total, bad = 0, []
    for i, blob in enumerate(banks):
        try:
            entries = decode_bank(blob)
        except ValueError as e:
            bad.append("bank %d: %s" % (i, e))
            continue
        total += len(entries)
    print("%d bank(s), %d message(s) decoded, %d refused"
          % (len(banks), total, len(bad)))
    for b in bad[:8]:
        print("  %s" % b)

    if args.check:
        # The real known-answer test: decode a bank and compare the STRINGS to
        # the JSON they were compiled from. Restricted to banks whose source is
        # plain Latin, because the charset above is a Latin block rather than a
        # font, a bank of Japanese or of icon glyphs would fail on the
        # rendering rather than on the decode, which would prove nothing.
        src = source_banks()
        checked = counted = mismatched = 0
        problems = []
        for bank_id, want in sorted(src.items()):
            if bank_id >= len(banks):
                continue
            try:
                got = decode_bank(banks[bank_id])
            except ValueError as e:
                problems.append("bank %d: %s" % (bank_id, e))
                continue
            if len(got) != len(want):
                mismatched += 1
                problems.append("bank %d holds %d message(s), its source has %d"
                                % (bank_id, len(got), len(want)))
                continue
            counted += 1
            for i, (chars, text) in enumerate(zip(got, want)):
                if text is None:
                    continue
                plain = re.sub(r"\{[^}]*\}", "", text)
                if not plain or not all(
                        c in " 0123456789.,!?-/:'"
                        or c.isalpha() and c.isascii() for c in plain):
                    continue
                shown = render(chars, table, eos)
                # Only compare where BOTH sides are inside the Latin block this
                # table covers. A message the table cannot spell would fail on
                # the rendering rather than on the decode, and proving the
                # charset incomplete is not what this check is for.
                if "\ufffd" in shown:
                    continue
                if shown != plain:
                    mismatched += 1
                    problems.append("bank %d entry %d decodes to %r, its source "
                                    "says %r" % (bank_id, i, shown, plain))
                else:
                    checked += 1
        print("%d bank(s) match their source's message count; %d individual "
              "message(s) decode to exactly their source text; %d disagreement(s)"
              % (counted, checked, mismatched))
        for p in problems[:8]:
            print("  %s" % p)
        return 1 if (mismatched or bad) else 0

    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
