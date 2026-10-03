#!/bin/sh
# Stand-in for NitroSDK ntrcomp.exe as pokediamond calls it:
#   ntrcomp -l2 -s [-A4] -o OUT IN
# LZ77 (type 0x10) with minimum match distance 2 -- nitrogfx's default LZ mode.
# -A4 pads the compressed stream to a multiple of 4 bytes; without it, no pad.
set -eu
NITROGFX="$(cd "$(dirname "$0")/../../games/diamond/tools/nitrogfx" && pwd)/nitrogfx"
pad=-nopad out= in=
while [ $# -gt 0 ]; do
    case "$1" in
        -l2|-s) ;;
        -A4) pad= ;;
        -o) out="$2"; shift ;;
        -*) echo "ntrcomp: unsupported option $1" >&2; exit 2 ;;
        *) in="$1" ;;
    esac
    shift
done
[ -n "$in" ] && [ -n "$out" ] || { echo "usage: ntrcomp -l2 -s [-A4] -o OUT IN" >&2; exit 2; }
# nitrogfx picks its mode from file extensions; give it neutral names.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cp "$in" "$tmp/in.bin"
"$NITROGFX" "$tmp/in.bin" "$tmp/out.lz" $pad
mv "$tmp/out.lz" "$out"
