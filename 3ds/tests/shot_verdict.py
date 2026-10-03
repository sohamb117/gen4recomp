#!/usr/bin/env python3
"""
3ds/tests/shot_verdict.py: read the self-test verdict out of a screenshot.

    3ds/tests/shot_verdict.py build/3ds/shots/run-emu.png

Exit 0 if the frame says the self-tests passed, 1 if it says they failed, 2 if
the frame does not say either.

Why this exists. `TEST_EMU=1 3ds/tests/run.sh` used to check only that the
emulator booted the binary. It reported 35 ok while the console was drawing
`DIV0 FAILED 1 OF 20`; a green run over a red console, which is the one kind
of failure worse than a red one. The verdict was on the screen the whole time
and nothing read it.

Why a coloured block and not the text. The `SELF n PASS` line beside it is
5x7 glyphs from `3ds_view.c`'s own font; reading it back means reimplementing
that font here and keeping the copy in step. The corner block is one colour at
one place: amber for a pass, red for a failure, chosen far enough apart in
every channel that scaling cannot turn one into the other.

Why it searches instead of sampling a fixed pixel. The PNG is the whole
emulator window, so where the console's screens land depends on the window
size and the menu bar's height. Searching for a run of the exact colour is
independent of all of that, and finding neither colour is its own answer;
that means the app did not draw, which a fixed sample would report as a
failure of the tests rather than of the boot.
"""

import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    print("shot_verdict: needs Pillow", file=sys.stderr)
    sys.exit(2)

GOOD = bytes((0xFF, 0xC0, 0x00))  # VIEW_COLOUR_MARK
BAD = bytes((0xFF, 0x00, 0x00))  # VIEW_COLOUR_MARK_BAD

# The block is 8x8 on each of the two screens, drawn at 1x. Requiring several
# pixels rejects a stray pixel of the emulator's own chrome; requiring fewer
# than 64 leaves room for a scaled window losing an edge row.
MIN_PIXELS = 16


def main():
    if len(sys.argv) != 2:
        print("usage: shot_verdict.py <frame.png>", file=sys.stderr)
        return 2

    raw = Image.open(sys.argv[1]).convert("RGB").tobytes()
    good = 0
    bad = 0
    for i in range(0, len(raw), 3):
        px = raw[i:i + 3]
        if px == GOOD:
            good += 1
        elif px == BAD:
            bad += 1

    if bad >= MIN_PIXELS:
        print("shot_verdict: FAIL, %d red marker pixel(s)" % bad)
        return 1
    if good >= MIN_PIXELS:
        print("shot_verdict: PASS (%d marker pixels)" % good)
        return 0
    print("shot_verdict: no marker in %s, %d amber, %d red; the app did not "
          "draw" % (sys.argv[1], good, bad), file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
