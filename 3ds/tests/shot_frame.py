#!/usr/bin/env python3
"""
3ds/tests/shot_frame.py: is there a picture on the console, and what kind?

    3ds/tests/shot_frame.py <diag.png> <game.png>

Exit 0 if both screens hold a picture, 1 if either is black, 2 if the pair
cannot be read at all. On 0 it prints one line describing the frame, short
enough to live in a MANIFEST.txt row.

Why it takes two shots and not one. A screenshot of the emulator is a whole
window: menu bar, status bar, a wide black surround, and two small panels
somewhere in the middle whose size depends on how big the window came up.
Measuring "is the picture black" means knowing where the picture IS, and
every way of guessing that from the game shot alone is a guess, the frame's
own letterbox is black too, so the game's pixels and the surround are the
same colour at the edges.

The diagnostic screen answers it for free. It draws `view_test_pattern` on
both surfaces, and the two body colours (VIEW_COLOUR_TOP, VIEW_COLOUR_BOTTOM
in 3ds/src/3ds_view.h) appear nowhere else in the window. Their bounding
boxes ARE the two panels, in window coordinates, measured on this boot at
this window size. Take the diagnostic shot first, press A, take the game
shot from the same window, and the rectangles carry over exactly.

That also rules out the failure this check exists to catch. Azahar paints its
own "Now Loading / Launching..." splash before the app starts, and that splash
is not black, roughly six per cent of the window has ink in it. A plain
"the window is not all black" test passes on a console that never drew
anything. Finding the two body colours is proof the app came up, because
nothing but this port draws them.

Why black is the only failure. A white screen is the DS's forced blank and is
a real frame; so is a logo, and so is a fade that happens to be dark at the
moment of the grab. The check reports mean brightness, colour count and the
dominant colour so the caller can say WHICH of those it got, and only fails
when a panel has no ink at all.
"""

import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    print("shot_frame: needs Pillow", file=sys.stderr)
    sys.exit(2)

# 3ds/src/3ds_view.h: VIEW_COLOUR_TOP and VIEW_COLOUR_BOTTOM, the test
# pattern's per-surface body fill. Keep in step with that header.
SCREENS = (("top", (0x20, 0x40, 0x80)), ("bottom", (0x80, 0x20, 0x40)))

# A panel scaled down from 256x192 is still tens of thousands of pixels, so a
# body fill that survives as a few hundred is not a panel; it is a stray run
# of the same colour in the emulator's own chrome.
MIN_PANEL = 4000

# A panel is "black" below this. It is not near zero on purpose: a hair of ink
# is what a stray cursor or one antialiased edge leaves behind, and calling
# that a picture is the same mistake as calling the splash one.
MIN_INK = 0.02


def panels(im):
    """The two panel rectangles, from the diagnostic pattern's body colours."""
    w, h = im.size
    raw = im.tobytes()
    found = {}
    for name, want in SCREENS:
        target = bytes(want)
        x0, y0, x1, y1, n = w, h, -1, -1, 0
        for y in range(h):
            row = raw[y * w * 3:(y + 1) * w * 3]
            # A row without the colour anywhere is the common case by far, and
            # `in` on bytes is a C-speed scan; only the few rows that hit are
            # walked pixel by pixel.
            if target not in row:
                continue
            for x in range(w):
                if row[x * 3:x * 3 + 3] == target:
                    n += 1
                    if x < x0:
                        x0 = x
                    if x > x1:
                        x1 = x
                    if y < y0:
                        y0 = y
                    if y > y1:
                        y1 = y
        if n < MIN_PANEL:
            return None, "%s screen: %d body pixels, need %d" % (name, n, MIN_PANEL)
        found[name] = (x0, y0, x1 + 1, y1 + 1)
    return found, None


def describe(im, rect):
    """Ink, brightness, colour count and dominant colour inside one panel."""
    crop = im.crop(rect)
    raw = crop.tobytes()
    total = crop.size[0] * crop.size[1]
    counts = {}
    ink = 0
    lum = 0
    for i in range(0, len(raw), 3):
        p = raw[i:i + 3]
        counts[p] = counts.get(p, 0) + 1
        if p != b"\0\0\0":
            ink += 1
        lum += p[0] + p[1] + p[2]
    top = max(counts.items(), key=lambda kv: kv[1])
    return {
        "ink": ink / float(total),
        "lum": lum / float(total * 3),
        "colours": len(counts),
        "dominant": top[0],
        "dominant_share": top[1] / float(total),
    }


def main():
    if len(sys.argv) != 3:
        print("usage: shot_frame.py <diag.png> <game.png>", file=sys.stderr)
        return 2

    diag = Image.open(sys.argv[1]).convert("RGB")
    game = Image.open(sys.argv[2]).convert("RGB")
    if diag.size != game.size:
        print("shot_frame: the two shots are %s and %s, the window resized "
              "between them, so the panels do not carry over"
              % (diag.size, game.size), file=sys.stderr)
        return 2

    rects, why = panels(diag)
    if rects is None:
        print("shot_frame: no console picture in %s, %s. The app did not "
              "draw; this is the emulator's own window."
              % (sys.argv[1], why), file=sys.stderr)
        return 2

    parts = []
    black = []
    stuck = []
    for name, body in SCREENS:
        d = describe(game, rects[name])
        if d["ink"] < MIN_INK:
            black.append(name)
        # Still the diagnostic screen. The port leaves its own panel on the
        # display until the game presents its first frame, so a game that
        # never gets there photographs as a panel full of ink, and this
        # check passed over exactly that for a build that hung after the
        # hand-over. Nothing but the diagnostic screen draws these two
        # colours, so a panel still dominated by its own body colour is a
        # console that never handed the picture over.
        if bytes(body) == d["dominant"] and d["dominant_share"] > 0.5:
            stuck.append(name)
        # A panel of one colour is flat: a forced blank, or a fill with
        # nothing composited over it. Worth naming; it is a real frame and
        # it is not a picture.
        flat = " flat" if d["colours"] == 1 else ""
        parts.append("%s ink %.0f%% lum %.0f colours %d dom %02x%02x%02x%s"
                     % (name, d["ink"] * 100, d["lum"], d["colours"],
                        d["dominant"][0], d["dominant"][1], d["dominant"][2],
                        flat))

    line = "; ".join(parts)
    if stuck:
        print("shot_frame: NOT THE GAME, %s screen(s) are still the "
              "diagnostic panel, so the game never presented a frame. %s"
              % ("+".join(stuck), line), file=sys.stderr)
        return 1
    if black:
        print("shot_frame: BLACK, %s screen(s) have no ink. %s"
              % ("+".join(black), line), file=sys.stderr)
        return 1
    print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
