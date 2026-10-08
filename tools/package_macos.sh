#!/usr/bin/env bash
# Build the self-contained macOS app and zip it:
#
#   tools/package_macos.sh [--universal] [--test]
#     -> build/dist/nativeplat-macos-arm64.zip      (default, Apple Silicon)
#     -> build/dist/nativeplat-macos-universal.zip  (--universal: arm64 + x86_64)
#
# The zip holds nativeplat.app plus README.txt, LICENSE.txt, THIRD-PARTY.txt.
# SDL3 comes from SDL's official SDL3.xcframework (tools/fetch_toolchains.sh
# macos), not Homebrew: Homebrew's dylib targets the running macOS only and
# lives at an absolute /opt/homebrew path. SDL3.framework is copied into
# Contents/Frameworks, the executable finds it via
# @executable_path/../Frameworks, and the bundle is ad-hoc signed (not
# notarized). Minimum macOS 11.0, like SDL's framework.
#
# Every game whose wasm core exists is built in (NP_GUEST_WASM_<game>
# overrides the defaults games/platinum/build/pc-wasm/pokeplatinum.wasm and
# games/diamond/build/pc-wasm/poke{diamond,pearl}.wasm). NP_BUILD_DIR
# overrides build/mac-dist[-universal]. The build honours
# CMAKE_BUILD_PARALLEL_LEVEL (run it under tools/heavy.sh).
#
# --test unzips into a temporary directory and runs the copied app's
# autotest (SDL dummy video/audio) on each built-in game whose ROM is in the
# build tree (Platinum: NP_TEST_ROM, default
# games/platinum/build/rom/pokeplatinum.us.nds; Ruby/Sapphire/Emerald: the
# decomp ROMs in .cache/gba), after checking with otool that no load command
# or rpath points outside the bundle and the system. NP_MAC_SHOTS=<dir>
# keeps the screenshots (<game>-<arch>.png).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TC="${NP_TOOLCHAINS:-$ROOT/.cache/toolchains}"
DIST="$ROOT/build/dist"
ARCHS=arm64 FLAVOR=arm64 TEST=0
for arg in "$@"; do
    case "$arg" in
        --universal) ARCHS="arm64;x86_64" FLAVOR=universal ;;
        --test) TEST=1 ;;
        *) echo "usage: $0 [--universal] [--test]" >&2; exit 2 ;;
    esac
done
BUILD="${NP_BUILD_DIR:-$ROOT/build/mac-dist$([ "$FLAVOR" = arm64 ] || echo "-$FLAVOR")}"
NAME="nativeplat-macos-$FLAVOR"

[ -f "$TC/sdl3-apple/share/cmake/SDL3/SDL3Config.cmake" ] || "$ROOT/tools/fetch_toolchains.sh" macos
SDL_FW="$TC/sdl3-apple/SDL3.xcframework/macos-arm64_x86_64/SDL3.framework"

guest_args=() built=()
: "${NP_GUEST_WASM_platinum:=$ROOT/games/platinum/build/pc-wasm/pokeplatinum.wasm}"
: "${NP_GUEST_WASM_diamond:=$ROOT/games/diamond/build/pc-wasm/pokediamond.wasm}"
: "${NP_GUEST_WASM_pearl:=$ROOT/games/diamond/build/pc-wasm/pokepearl.wasm}"
: "${NP_GUEST_WASM_ruby:=$ROOT/games/ruby/build/pc-wasm/pokeruby.wasm}"
: "${NP_GUEST_WASM_sapphire:=$ROOT/games/ruby/build/pc-wasm/pokesapphire.wasm}"
: "${NP_GUEST_WASM_emerald:=$ROOT/games/emerald/build/pc-wasm/pokeemerald.wasm}"
for game in diamond pearl platinum ruby sapphire emerald; do
    var="NP_GUEST_WASM_$game"
    wasm="${!var:-}"
    if [ -n "$wasm" ] && [ -f "$wasm" ]; then
        guest_args+=("-D$var=$wasm" "-DNP_GUEST_POSTPROCESS_$game=$ROOT/tools/wasm2c_postprocess.py")
        built+=("$game")
        echo "package_macos: building in $game ($wasm)"
    fi
done
[ ${#guest_args[@]} -gt 0 ] || { echo "package_macos: no wasm core found; build one first (docs/BUILDING.md)" >&2; exit 1; }

cmake -S "$ROOT/shell" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DNP_CORE=real -DBUILD_TESTING=OFF \
    -DCMAKE_OSX_ARCHITECTURES="$ARCHS" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
    -DSDL3_DIR="$TC/sdl3-apple/share/cmake/SDL3" \
    "${guest_args[@]}"
cmake --build "$BUILD" --target nativeplat

STAGE="$DIST/$NAME"
APP="$STAGE/nativeplat.app"
EXE="$APP/Contents/MacOS/nativeplat"
rm -rf "$STAGE" "$DIST/$NAME.zip"
mkdir -p "$STAGE"
ditto "$BUILD/nativeplat.app" "$APP"

# The framework without headers and SDL's own signature (re-signed below),
# thinned to the app's architectures.
FW="$APP/Contents/Frameworks/SDL3.framework"
mkdir -p "$APP/Contents/Frameworks"
ditto "$SDL_FW" "$FW"
rm -rf "$FW/Headers" "$FW/Versions/A/Headers" "$FW/Versions/A/_CodeSignature"
if [ "$FLAVOR" = arm64 ]; then
    lipo "$FW/Versions/A/SDL3" -thin arm64 -output "$FW/Versions/A/SDL3.thin"
    mv "$FW/Versions/A/SDL3.thin" "$FW/Versions/A/SDL3"
fi

# Replace whatever rpaths the build recorded (the xcframework directory) with
# the bundle's Frameworks directory.
otool -l "$EXE" | awk '/cmd LC_RPATH/{r=1} r&&/ path /{print $2; r=0}' | sort -u | while read -r rp; do
    install_name_tool -delete_rpath "$rp" "$EXE"
done
install_name_tool -add_rpath "@executable_path/../Frameworks" "$EXE"
strip -S "$EXE"

codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP"

# Nothing outside the bundle and the OS may be referenced.
bad="$( { otool -L "$EXE" "$APP/Contents/Frameworks/SDL3.framework/SDL3" | grep -v ':$' | awk '{print $1}';
          otool -l "$EXE" | awk '/cmd LC_RPATH/{r=1} r&&/ path /{print $2; r=0}'; } |
        grep -v -E '^(/System/Library/|/usr/lib/|@rpath/SDL3\.framework/|@executable_path/\.\./Frameworks$)' || true)"
if [ -n "$bad" ]; then
    echo "package_macos: non-system references left in the bundle:" >&2
    echo "$bad" >&2
    exit 1
fi
echo "package_macos: $(lipo -archs "$EXE") executable; links:"
otool -L "$EXE" | sed 1d

cp "$ROOT/tools/dist/README.txt" "$STAGE/README.txt"
cp "$ROOT/games/platinum/LICENSE" "$STAGE/LICENSE.txt"
{
    cat "$ROOT/tools/dist/THIRD-PARTY.txt" | grep -v '^- Windows only' | grep -v '^  Zope Public License'
    printf '\n--- SDL3 (zlib license) ---\n\n'
    cat "$TC/sdl3-apple/LICENSE.txt"
} >"$STAGE/THIRD-PARTY.txt"
(cd "$DIST" && ditto -c -k --keepParent "$NAME" "$NAME.zip")
rm -rf "$STAGE"
ls -l "$DIST/$NAME.zip"
shasum -a 256 "$DIST/$NAME.zip"

if [ "$TEST" = 1 ]; then
    TMP="$(mktemp -d "${TMPDIR:-/tmp}/np-mactest.XXXXXX")"
    trap 'rm -rf "$TMP"' EXIT
    ditto -x -k "$DIST/$NAME.zip" "$TMP"
    codesign --verify --deep --strict "$TMP/$NAME/nativeplat.app"
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
        esac
        [ -f "$ROM" ] || { echo "package_macos: no $game ROM at $ROM; not tested"; continue; }
        for arch in $(lipo -archs "$TMP/$NAME/nativeplat.app/Contents/MacOS/nativeplat"); do
            rm -f "$TMP/shot.png"
            echo "package_macos: autotest $game ($arch) from $TMP"
            SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy \
                NP_AUTOTEST="$run,png=$TMP/shot.png,game=$game,rom=$ROM" \
                arch "-$arch" "$TMP/$NAME/nativeplat.app/Contents/MacOS/nativeplat"
            [ -s "$TMP/shot.png" ] || { echo "package_macos: autotest wrote no screenshot" >&2; exit 1; }
            echo "package_macos: autotest $game passed ($arch, $(wc -c <"$TMP/shot.png") byte screenshot)"
            [ -z "${NP_MAC_SHOTS:-}" ] || cp "$TMP/shot.png" "$NP_MAC_SHOTS/$game-$arch.png"
        done
    done
fi
