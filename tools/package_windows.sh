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
# overrides the defaults games/platinum/build/pc-wasm/pokeplatinum.wasm and
# games/diamond/build/pc-wasm/poke{diamond,pearl}.wasm). NP_BUILD_DIR
# overrides build/win-app. The build honours CMAKE_BUILD_PARALLEL_LEVEL (run
# it under tools/heavy.sh).
#
# --test runs the packaged exe from a temporary copy under wine (OrbStack
# amd64 container, tools/docker/wine.Dockerfile) with SDL's dummy video and
# audio drivers and NP_AUTOTEST booting each built-in game whose ROM is in
# the build tree (Platinum: NP_TEST_ROM, default
# games/platinum/build/rom/pokeplatinum.us.nds) to its title screen;
# NP_WIN_SHOTS=<dir> keeps the screenshots.
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

guest_args=() built=()
: "${NP_GUEST_WASM_platinum:=$ROOT/games/platinum/build/pc-wasm/pokeplatinum.wasm}"
: "${NP_GUEST_WASM_diamond:=$ROOT/games/diamond/build/pc-wasm/pokediamond.wasm}"
: "${NP_GUEST_WASM_pearl:=$ROOT/games/diamond/build/pc-wasm/pokepearl.wasm}"
for game in diamond pearl platinum; do
    var="NP_GUEST_WASM_$game"
    wasm="${!var:-}"
    if [ -n "$wasm" ] && [ -f "$wasm" ]; then
        guest_args+=("-D$var=$wasm" "-DNP_GUEST_POSTPROCESS_$game=$ROOT/tools/wasm2c_postprocess.py")
        built+=("$game")
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
    TMP="$(mktemp -d "${TMPDIR:-/tmp}/np-wintest.XXXXXX")"
    trap 'rm -rf "$TMP"' EXIT
    (cd "$TMP" && unzip -q "$DIST/$NAME.zip")
    for game in "${built[@]}"; do
        case "$game" in
            platinum) ROM="${NP_TEST_ROM:-$ROOT/games/platinum/build/rom/pokeplatinum.us.nds}" ;;
            diamond) ROM="$ROOT/games/diamond/build/diamond.us/pokediamond.us.nds" ;;
            pearl) ROM="$ROOT/games/diamond/build/pearl.us/pokepearl.us.nds" ;;
        esac
        [ -f "$ROM" ] || { echo "package_windows: no $game ROM at $ROM; not tested"; continue; }
        rm -f "$TMP/rom.nds" "$TMP/shot-$game.png"
        cp "$ROM" "$TMP/rom.nds"
        # Z: is the container's root in wine's default prefix. Wine under
        # Rosetta (OrbStack) sometimes dies at start with "rosetta error:
        # invalid gdt selector": such a run is retried, up to 3 attempts.
        for attempt in 1 2 3; do
            "${DOCKER[@]}" run --rm --platform linux/amd64 -v "$TMP:/t" -w /t/$NAME \
                -e XDG_RUNTIME_DIR=/tmp -e WINEDEBUG=-all \
                -e SDL_VIDEO_DRIVER=dummy -e SDL_AUDIO_DRIVER=dummy \
                -e "NP_AUTOTEST=frames=1500,png=Z:\\t\\shot-$game.png,game=$game,rom=Z:\\t\\rom.nds,press=1200:start:10" \
                -e WINEPATH= "$IMAGE" sh -c 'wine64 nativeplat.exe; s=$?; echo; echo "exit status $s"' 2>&1 | tee "$TMP/log-$game.txt"
            grep -q 'rosetta error' "$TMP/log-$game.txt" || break
            echo "package_windows: Rosetta crashed wine (attempt $attempt)"
        done
        grep -q '^exit status 0$' "$TMP/log-$game.txt" && [ -s "$TMP/shot-$game.png" ] ||
            { echo "package_windows: $game autotest FAILED" >&2; exit 1; }
        echo "package_windows: $game autotest passed under wine ($(wc -c <"$TMP/shot-$game.png") byte screenshot)"
        [ -z "${NP_WIN_SHOTS:-}" ] || cp "$TMP/shot-$game.png" "$NP_WIN_SHOTS/"
    done
fi
