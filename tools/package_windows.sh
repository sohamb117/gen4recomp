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
# overrides the defaults: games/{platinum,diamond}/build/pc-wasm/poke*.wasm,
# games/ndsrec/build/pc-wasm/ndsrec-{black,white,poketransfer}.wasm (Poke Transfer's
# child station),
# games/heartgold/build/pc-wasm/poke{heartgold,soulsilver}.wasm,
# games/{ruby,emerald}/build/pc-wasm/poke*.wasm). NP_BUILD_DIR
# overrides build/win-app. The build honours CMAKE_BUILD_PARALLEL_LEVEL (run
# it under tools/heavy.sh).
#
# --test runs the packaged exe from a temporary copy under wine (OrbStack
# amd64 container, tools/docker/wine.Dockerfile) with SDL's dummy video and
# audio drivers and NP_AUTOTEST booting each built-in game whose ROM is in
# the build tree (Platinum: NP_TEST_ROM, default
# games/platinum/build/rom/pokeplatinum.us.nds; HeartGold/SoulSilver: NP_HG_ROM /
# NP_SS_ROM, default their decomp ROMs in games/heartgold/build; Black/White:
# NP_BLACK_ROM / NP_WHITE_ROM, default the dumps in roms/; Ruby/Sapphire/Emerald:
# the decomp ROMs in .cache/gba) to its title screen;
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
: "${NP_GUEST_WASM_ruby:=$ROOT/games/ruby/build/pc-wasm/pokeruby.wasm}"
: "${NP_GUEST_WASM_sapphire:=$ROOT/games/ruby/build/pc-wasm/pokesapphire.wasm}"
: "${NP_GUEST_WASM_emerald:=$ROOT/games/emerald/build/pc-wasm/pokeemerald.wasm}"
: "${NP_GUEST_WASM_black:=$ROOT/games/ndsrec/build/pc-wasm/ndsrec-black.wasm}"
: "${NP_GUEST_WASM_white:=$ROOT/games/ndsrec/build/pc-wasm/ndsrec-white.wasm}"
: "${NP_GUEST_WASM_heartgold:=$ROOT/games/heartgold/build/pc-wasm/pokeheartgold.wasm}"
: "${NP_GUEST_WASM_soulsilver:=$ROOT/games/heartgold/build/pc-wasm/pokesoulsilver.wasm}"
: "${NP_GUEST_WASM_poketransfer:=$ROOT/games/ndsrec/build/pc-wasm/ndsrec-poketransfer.wasm}"
for game in diamond pearl platinum black white heartgold soulsilver ruby sapphire emerald poketransfer; do
    var="NP_GUEST_WASM_$game"
    wasm="${!var:-}"
    if [ -n "$wasm" ] && [ -f "$wasm" ]; then
        guest_args+=("-D$var=$wasm" "-DNP_GUEST_POSTPROCESS_$game=$ROOT/tools/wasm2c_postprocess.py")
        built+=("$game")
        echo "package_windows: building in $game ($wasm)"
    fi
done
[ ${#guest_args[@]} -gt 0 ] || { echo "package_windows: no wasm core found; build one first (docs/BUILDING.md)" >&2; exit 1; }

# -g0: zig cc emits debug info by default, which the package strips anyway
# (llvm-strip below); with it the six cores' objects took ~3.5 GB.
cmake -S "$ROOT/shell" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/cmake/windows-x64.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-g0 -DNP_CORE=real -DBUILD_TESTING=OFF \
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
        # DS: title screen at 1500 (START at 1200 skips the intro); GBA: START
        # at 400 skips the intro, the title screen ("<GAME> VERSION") at 900.
        run="frames=1500,press=1200:start:10"
        case "$game" in
            platinum) ROM="${NP_TEST_ROM:-$ROOT/games/platinum/build/rom/pokeplatinum.us.nds}" ;;
            diamond) ROM="$ROOT/games/diamond/build/diamond.us/pokediamond.us.nds" ;;
            pearl) ROM="$ROOT/games/diamond/build/pearl.us/pokepearl.us.nds" ;;
            ruby | sapphire) ROM="$ROOT/.cache/gba/pokeruby/poke$game.gba" run="frames=900,press=400:start:10" ;;
            emerald) ROM="$ROOT/.cache/gba/pokeemerald/pokeemerald.gba" run="frames=900,press=400:start:10" ;;
            # B/W: the title (Reshiram / Zekrom) from ~4800, no input (START
            # at 5000 would leave it); HG/SS: the intro runs to the title (Ho-Oh
            # / Lugia below the logo) by ~4400 with no input.
            black) ROM="${NP_BLACK_ROM:-$ROOT/roms/Pokemon - Black Version (USA, Europe) (NDSi Enhanced).nds}" run="frames=4950" ;;
            white) ROM="${NP_WHITE_ROM:-$ROOT/roms/Pokemon - White Version (USA, Europe) (NDSi Enhanced).nds}" run="frames=4950" ;;
            heartgold) ROM="${NP_HG_ROM:-$ROOT/games/heartgold/build/heartgold.us/pokeheartgold.us.nds}" run="frames=4800" ;;
            soulsilver) ROM="${NP_SS_ROM:-$ROOT/games/heartgold/build/soulsilver.us/pokesoulsilver.us.nds}" run="frames=4800" ;;
            # Poke Transfer's child station shows nothing until Black/White's
            # lab sends it the child (tests/poketransfer/run_tests.py).
            poketransfer) continue ;;
        esac
        [ -f "$ROM" ] || { echo "package_windows: no $game ROM at $ROM; not tested"; continue; }
        ext="${ROM##*.}"
        rm -f "$TMP"/rom.* "$TMP/shot-$game.png"
        cp "$ROM" "$TMP/rom.$ext"
        # Z: is the container's root in wine's default prefix. Wine under
        # Rosetta (OrbStack) sometimes dies at start with "rosetta error:
        # invalid gdt selector": such a run is retried, up to 3 attempts.
        for attempt in 1 2 3; do
            "${DOCKER[@]}" run --rm --platform linux/amd64 -v "$TMP:/t" -w /t/$NAME \
                -e XDG_RUNTIME_DIR=/tmp -e WINEDEBUG=-all \
                -e SDL_VIDEO_DRIVER=dummy -e SDL_AUDIO_DRIVER=dummy \
                -e "NP_AUTOTEST=$run,png=Z:\\t\\shot-$game.png,game=$game,rom=Z:\\t\\rom.$ext" \
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
