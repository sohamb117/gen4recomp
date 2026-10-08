#!/usr/bin/env python3
"""Apply the experimentally tested BW proposal to known assembly only.

Every caller of overlay 230 gets the outcome of a genuine cartridge, read
from its own comparisons; the overlay's load, unload and calls are removed.
This substitutes detector outcomes; it does not implement overlay 230. Keep
the full-file hashes in sync with the reviewed proposal, not arbitrary ROM
revisions. The argument is the emitted static's ndsrec_arm9_004.s; the
overlay files are found from the same assembly root.
"""

import argparse
import hashlib
from pathlib import Path

# Each Thumb BL occupied four bytes. Two two-byte instructions retain all
# subsequent guest addresses, including the caller's literal pool.
NOPS = b"\tnop\n\tnop\n"
ZERO = b"\tmov r0, #0x0\n\tnop\n"
NOT_R1 = b"\tmvn r0, r1\n\tnop\n"


def edits(load, unload, startup, ov10, ov20):
    return (
        # sub_02011D9C (startup): skip the load/unload and third call; the
        # first check returns zero, the second the complement of r1.
        ("arm9/asm/ndsrec_arm9_004.s",
         ((1785, load, NOPS), (1791, startup[0], ZERO), (1810, startup[1], NOT_R1),
          (1831, startup[2], NOPS), (1838, unload, NOPS))),
        # ov10_0216EB98 (after a battle): r7 starts at a multiple of 0x1933
        # and each check adds 0 on the genuine path; any other sum queues
        # the 0x02011D35 task. The first two add 0 when the result is ~r1;
        # the third adds 0x9D when it is ~r1, so it returns zero instead.
        ("arm9/overlays/10/asm/ndsrec_ov010_005.s",
         ((3379, load, NOPS), (3393, ov10[0], NOT_R1), (3426, ov10[1], NOT_R1),
          (3445, ov10[2], ZERO), (3476, unload, NOPS))),
        # ov20_021841C0 (field): the three calls' results are not read.
        ("arm9/overlays/20/asm/ndsrec_ov020_000.s",
         ((840, load, NOPS), (870, ov20[0], NOPS), (909, ov20[1], NOPS),
          (929, ov20[2], NOPS), (956, unload, NOPS))),
    )


VERSIONS = {
    "black": (
        edits("sub_02034AC4", "sub_02034A5C",
              ("armrec_dispatch_021882A0", "armrec_dispatch_02188354", "armrec_dispatch_02188390"),
              ("armrec_dispatch_021882DC", "armrec_dispatch_02188354", "armrec_dispatch_02188390"),
              ("armrec_dispatch_021882A0", "armrec_dispatch_02188318", "armrec_dispatch_021883CC")),
        {
            "arm9/asm/ndsrec_arm9_004.s": (
                "c737d8a6eb70ac2959ff036583e306600a9ee331a084a3af65b5250c0b36cab4",
                "6ac07a4b418fd9e22ca11402bc1f9341fb01718c62f73c4aea8ed9e833982b3e"),
            "arm9/overlays/10/asm/ndsrec_ov010_005.s": (
                "a496da5b5eaf9dfae44bde53dc4f17ea538c598707da166dfa9a09da83d27d96",
                "9dc89ec2538a2231c2efece7a8f5d53916daff3eccc0d41db6ef90c15a43c22c"),
            "arm9/overlays/20/asm/ndsrec_ov020_000.s": (
                "8e3275ae4a45444f884614559c7205c2d3ce9b39be4ee68f9827c310f0749e22",
                "dcfb5aeb046ecad8c9b9fd51b6f645328efd20b8dd9fa0bbbab4ba080b834fd6"),
        },
    ),
    "white": (
        edits("sub_02034ADC", "sub_02034A74",
              ("armrec_dispatch_021882C0", "armrec_dispatch_02188374", "armrec_dispatch_021883B0"),
              ("armrec_dispatch_021882FC", "armrec_dispatch_02188374", "armrec_dispatch_021883B0"),
              ("armrec_dispatch_021882C0", "armrec_dispatch_02188338", "armrec_dispatch_021883EC")),
        {
            "arm9/asm/ndsrec_arm9_004.s": (
                "3486e24783a99445e052072ebd6620bfe18cc134a8285898ea901234b2999592",
                "95777aa0b21db0f416c3fd6d30220a697476bd9a1e66fee82b170fe40c533597"),
            "arm9/overlays/10/asm/ndsrec_ov010_005.s": (
                "4428954964afeb8a00446b3fe57280929f0466694ef8a2a5c217e7998a69520d",
                "0580d0227b95e4d8ceb4538d1a4f8fc4c149a95f7e8df70701818f16b635a37e"),
            "arm9/overlays/20/asm/ndsrec_ov020_000.s": (
                "9895abdccc850bb58d88194a7d4fec42ecb7745447a78710a787b85faddd4774",
                "5429efa591df18a2e527b9457f6a6f3417123413850504a283f0a738bf670817"),
        },
    ),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", choices=VERSIONS)
    parser.add_argument("assembly", type=Path, help="the emitted arm9/asm/ndsrec_arm9_004.s")
    args = parser.parse_args()
    files, hashes = VERSIONS[args.version]
    root = args.assembly.resolve().parents[2]
    for rel, subs in files:
        path = root / rel
        before, after = hashes[rel]
        source = path.read_bytes()
        digest = hashlib.sha256(source).hexdigest()
        if digest == after:
            print(f"BW proposal: {args.version} {rel} already applied ({after})")
            continue
        if digest != before:
            parser.exit(1, f"BW proposal: refusing unexpected {args.version} assembly\n"
                        f"  {path}\n  got {digest}\n"
                        f"  expected original {before} or proposed {after}\n")
        lines = source.splitlines(keepends=True)
        for line, target, replacement in subs:
            if lines[line - 1] != f"\tbl {target}\n".encode():
                parser.exit(1, f"BW proposal: unexpected call at {rel}:{line}\n")
            lines[line - 1] = replacement
        proposed = b"".join(lines)
        if hashlib.sha256(proposed).hexdigest() != after:
            parser.exit(1, f"BW proposal: proposed output hash mismatch for {rel}; no write\n")
        path.write_bytes(proposed)
        print(f"BW proposal: applied {args.version} {rel} ({after})")


if __name__ == "__main__":
    main()
