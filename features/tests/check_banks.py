#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare ROM text decoded by ndsdata with pret/pokeplatinum's text sources.

usage: check_banks.py <dump_bank exe> <rom.nds> <games/platinum dir>

Every bank listed in generated/text_banks.txt (member index = line - 1) whose
JSON source can be found (res/**/<name>.json or the build-generated
build/rom/res/**/<name>.json) is decoded from the ROM and compared string by
string with the JSON, interpreted the way tools/msgenc/Json.cpp does
(en_US string or array of lines concatenated; `garbage: N` = N spaces).
Exit 77 (CTest skip) if the ROM is missing.
"""
import glob
import json
import os
import subprocess
import sys


def expected(entry):
    if isinstance(entry, str):
        text = entry
    elif 'en_US' in entry:
        v = entry['en_US']
        text = ''.join(v) if isinstance(v, list) else v
    elif 'garbage' in entry:
        return ' ' * int(entry['garbage'])
    else:
        raise ValueError(entry)
    # msgenc registers both the two-character spellings `\n`, `\r`, `\f` and
    # the control characters themselves for the same charcode
    # (MessagesConverter::ReadCharmap), so either form encodes identically.
    return text.replace('\\n', '\n').replace('\\r', '\r').replace('\\f', '\f')


def main() -> int:
    exe, rom, pt = sys.argv[1:4]
    if not os.path.exists(rom):
        print('SKIP: ROM not found at %s' % rom)
        return 77
    names = [l.strip() for l in open(os.path.join(pt, 'generated/text_banks.txt')) if l.strip()]
    index = {}
    for root in ('res', 'build/rom/res'):
        for path in glob.glob(os.path.join(pt, root, '**', '*.json'), recursive=True):
            index.setdefault(os.path.basename(path)[:-5], path)

    checked = mismatched_banks = 0
    for bank, name in enumerate(names):
        stem = name[len('TEXT_BANK_'):].lower()
        src = index.get(stem)
        if not src:
            continue
        with open(src, encoding='utf-8') as f:
            doc = json.load(f)
        if 'messages' not in doc:
            continue
        want = [expected(e) for e in doc['messages']]
        out = subprocess.run([exe, rom, str(bank)], check=True, capture_output=True)
        got = json.loads(out.stdout.decode('utf-8'))
        checked += 1
        if got != want:
            mismatched_banks += 1
            bad = [i for i in range(max(len(got), len(want)))
                   if i >= len(got) or i >= len(want) or got[i] != want[i]]
            i = bad[0]
            print('bank %d (%s): %d/%d strings differ; first #%d\n  rom:  %r\n  json: %r' % (
                bank, os.path.relpath(src, pt), len(bad), len(want), i,
                got[i] if i < len(got) else None, want[i] if i < len(want) else None))
    print('compared %d banks against decomp JSON, %d mismatched' % (checked, mismatched_banks))
    return 1 if mismatched_banks or checked < 600 else 0


if __name__ == '__main__':
    sys.exit(main())
