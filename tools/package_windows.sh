#!/usr/bin/env bash
# Cross-build the Windows x64 app (zig + SDL3 mingw, tools/cmake/windows-x64.cmake)
# and package it as build/dist/nativeplat-windows-x64.zip:
#
#   nativeplat-windows-x64/
#     nativeplat.exe  SDL3.dll  README.txt  LICENSE.txt  THIRD-PARTY.txt
#
#   tools/package_windows.sh [--test]
#
# Every game whose wasm core exists is built in (NP_GUEST_WASM_<game>
# overrides the default path; Platinum: games/platinum/build/pc-wasm/
# pokeplatinum.wasm). NP_BUILD_DIR overrides build/win-app.
#
# --test runs the packaged exe from a temporary copy under wine (OrbStack
# amd64 container, tools/docker/wine.Dockerfile) with SDL's dummy video and
# audio drivers and NP_AUTOTEST booting the Platinum ROM
# (NP_TEST_ROM, default games/platinum/build/rom/pokeplatinum.us.nds).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${NP_BUILD_DIR:-$ROOT/build/win-app}"
DIST="$ROOT/build/dist"
NAME=nativeplat-windows-x64
TC="${NP_TOOLCHAINS:-$ROOT/.cache/toolchains}"
TEST=0
for arg in "$@"; do
    case "$arg" in
        --test) TEST=1 ;;
        *) echo "usage: $0 [--test]" >&2; exit 2 ;;
    esac
done

[ -x "$TC/zig/zig" ] && [ -d "$TC/sdl3-mingw" ] || "$ROOT/tools/fetch_toolchains.sh" windows

guest_args=()
: "${NP_GUEST_WASM_platinum:=$ROOT/games/platinum/build/pc-wasm/pokeplatinum.wasm}"
for game in diamond pearl platinum; do
    var="NP_GUEST_WASM_$game"
    wasm="${!var:-}"
    if [ -n "$wasm" ] && [ -f "$wasm" ]; then
        guest_args+=("-D$var=$wasm" "-DNP_GUEST_POSTPROCESS_$game=$ROOT/tools/wasm2c_postprocess.py")
        echo "package_windows: building in $game ($wasm)"
    fi
done
[ ${#guest_args[@]} -gt 0 ] || { echo "package_windows: no wasm core found; build one first (docs/BUILDING.md)" >&2; exit 1; }

cmake -S "$ROOT/shell" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/cmake/windows-x64.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DNP_CORE=real -DBUILD_TESTING=OFF \
    "${guest_args[@]}"
cmake --build "$BUILD" --target nativeplat

STAGE="$DIST/$NAME"
rm -rf "$STAGE" "$DIST/$NAME.zip"
mkdir -p "$STAGE"
cp "$BUILD/nativeplat.exe" "$STAGE/"
# zig objcopy is ELF-only; wasi-sdk ships a host llvm-strip that reads PE.
"$TC/wasi-sdk/bin/llvm-strip" --strip-debug "$STAGE/nativeplat.exe"
cp "$TC/sdl3-mingw/x86_64-w64-mingw32/bin/SDL3.dll" "$STAGE/"
cp "$ROOT/tools/dist/README.txt" "$STAGE/README.txt"
cp "$ROOT/games/platinum/LICENSE" "$STAGE/LICENSE.txt"
{
    cat "$ROOT/tools/dist/THIRD-PARTY.txt"
    printf '\n--- SDL3 (zlib license) ---\n\n'
    cat "$TC/sdl3-mingw/LICENSE.txt"
    printf '\n--- mingw-w64 runtime (ZPL 2.1) ---\n\n'
    cat "$TC/zig/lib/libc/mingw/COPYING"
} >"$STAGE/THIRD-PARTY.txt"
# Windows readers expect CRLF in the text files.
for f in README.txt LICENSE.txt THIRD-PARTY.txt; do
    sed -i '' -e 's/\r$//' -e 's/$/\r/' "$STAGE/$f" 2>/dev/null || sed -i -e 's/\r$//' -e 's/$/\r/' "$STAGE/$f"
done
(cd "$DIST" && zip -qr -X "$NAME.zip" "$NAME")
rm -rf "$STAGE"
ls -l "$DIST/$NAME.zip"
shasum -a 256 "$DIST/$NAME.zip"

if [ "$TEST" = 1 ]; then
    CONTEXT="${NP_DOCKER_CONTEXT:-orbstack}"
    DOCKER=(docker --context "$CONTEXT")
    IMAGE=nativeplat-wine
    "${DOCKER[@]}" image inspect "$IMAGE" >/dev/null 2>&1 ||
        "${DOCKER[@]}" build --platform linux/amd64 -t "$IMAGE" -f "$ROOT/tools/docker/wine.Dockerfile" "$ROOT/tools/docker"
    ROM="${NP_TEST_ROM:-$ROOT/games/platinum/build/rom/pokeplatinum.us.nds}"
    TMP="$(mktemp -d "${TMPDIR:-/tmp}/np-wintest.XXXXXX")"
    trap 'rm -rf "$TMP"' EXIT
    (cd "$TMP" && unzip -q "$DIST/$NAME.zip")
    cp "$ROM" "$TMP/rom.nds"
    # Z: is the container's root in wine's default prefix.
    "${DOCKER[@]}" run --rm --platform linux/amd64 -v "$TMP:/t" -w /t/$NAME \
        -e XDG_RUNTIME_DIR=/tmp -e WINEDEBUG=-all \
        -e SDL_VIDEO_DRIVER=dummy -e SDL_AUDIO_DRIVER=dummy \
        -e NP_AUTOTEST='frames=1500,png=Z:\t\shot.png,rom=Z:\t\rom.nds,press=1200:start:10' \
        -e WINEPATH= "$IMAGE" sh -c 'wine64 nativeplat.exe; echo "exit status $?"' 2>&1 | tee "$TMP/log.txt"
    grep -q '^exit status 0$' "$TMP/log.txt" && [ -s "$TMP/shot.png" ] || { echo "package_windows: autotest FAILED" >&2; exit 1; }
    echo "package_windows: autotest passed under wine ($(wc -c <"$TMP/shot.png") byte screenshot)"
fi
