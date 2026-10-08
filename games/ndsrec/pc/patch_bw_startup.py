#!/usr/bin/env python3
"""Apply the experimentally tested BW startup proposal to known assembly only.

This substitutes detector outcomes; it does not implement overlay 230. Keep the
full-file hashes in sync with the reviewed proposal, not arbitrary ROM revisions.
"""

import argparse
import hashlib
from pathlib import Path


VERSIONS = {
    "black": (
        "c737d8a6eb70ac2959ff036583e306600a9ee331a084a3af65b5250c0b36cab4",
        "6ac07a4b418fd9e22ca11402bc1f9341fb01718c62f73c4aea8ed9e833982b3e",
        ("sub_02034AC4", "armrec_dispatch_021882A0",
         "armrec_dispatch_02188354", "armrec_dispatch_02188390", "sub_02034A5C"),
    ),
    "white": (
        "3486e24783a99445e052072ebd6620bfe18cc134a8285898ea901234b2999592",
        "95777aa0b21db0f416c3fd6d30220a697476bd9a1e66fee82b170fe40c533597",
        ("sub_02034ADC", "armrec_dispatch_021882C0",
         "armrec_dispatch_02188374", "armrec_dispatch_021883B0", "sub_02034A74"),
    ),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", choices=VERSIONS)
    parser.add_argument("assembly", type=Path)
    args = parser.parse_args()
    before, after, targets = VERSIONS[args.version]
    source = args.assembly.read_bytes()
    digest = hashlib.sha256(source).hexdigest()
    if digest == after:
        print(f"BW startup proposal: {args.version} already applied ({after})")
        return
    if digest != before:
        parser.exit(1, f"BW startup proposal: refusing unexpected {args.version} assembly\n"
                    f"  {args.assembly}\n  got {digest}\n"
                    f"  expected original {before} or proposed {after}\n")

    # Each Thumb BL occupied four bytes. Two two-byte instructions retain
    # all subsequent guest addresses, including the caller's literal pool.
    lines = source.splitlines(keepends=True)
    replacements = (b"\tnop\n\tnop\n", b"\tmov r0, #0x0\n\tnop\n",
                    b"\tmvn r0, r1\n\tnop\n", b"\tnop\n\tnop\n",
                    b"\tnop\n\tnop\n")
    for line, target, replacement in zip((1785, 1791, 1810, 1831, 1838),
                                         targets, replacements):
        if lines[line - 1] != f"\tbl {target}\n".encode():
            parser.exit(1, f"BW startup proposal: unexpected call at line {line}\n")
        lines[line - 1] = replacement
    proposed = b"".join(lines)
    if hashlib.sha256(proposed).hexdigest() != after:
        parser.exit(1, "BW startup proposal: proposed output hash mismatch; no write\n")
    args.assembly.write_bytes(proposed)
    print(f"BW startup proposal: applied {args.version} ({after})")


if __name__ == "__main__":
    main()
