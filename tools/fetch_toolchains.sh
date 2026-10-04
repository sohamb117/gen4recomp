#!/usr/bin/env bash
# Fetch the pinned host toolchains into .cache/toolchains.
#
#   tools/fetch_toolchains.sh            wasi-sdk + wabt (every build)
#   tools/fetch_toolchains.sh windows    also zig + SDL3 mingw (Windows cross build)
#   tools/fetch_toolchains.sh macos      also SDL3's official Apple release
#                                        (tools/package_macos.sh)
#
#   wasi-sdk  clang + wasm-ld + wasi-libc: compiles the game for wasm32, which is
#             what keeps guest pointers 32 bits wide on 64-bit hosts.
#   wabt      wasm2c: turns that module back into portable C for each host.
#   zig       `zig cc -target x86_64-windows-gnu` (clang + lld + mingw-w64
#             headers/libs, ar, ranlib, rc) for tools/cmake/windows-x64.cmake.
#   sdl3-mingw  SDL3's official mingw development package (headers, import
#             library, SDL3.dll, CMake config) for the Windows app.
#   sdl3-apple  SDL3's official SDL3.xcframework (universal macOS framework,
#             deployment target 11.0, plus iOS slices) and its CMake config,
#             extracted from the release .dmg without the dSYMs. Homebrew's
#             SDL3 is built for the running macOS only, so it is not
#             redistributable in an app bundle.
#
# Hashes are pinned; a mismatch aborts before anything is extracted.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${NP_TOOLCHAINS:-$ROOT/.cache/toolchains}"
mkdir -p "$DEST"

WANT_WINDOWS=0 WANT_MACOS=0
for arg in "$@"; do
    case "$arg" in
        windows) WANT_WINDOWS=1 ;;
        macos) WANT_MACOS=1 ;;
        *) echo "usage: $0 [windows] [macos]" >&2; exit 2 ;;
    esac
done

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) HOST_WASI=arm64-macos;  HOST_WABT=macos-arm64; HOST_ZIG=aarch64-macos ;;
    Darwin-x86_64) HOST_WASI=x86_64-macos; HOST_WABT=macos-x64; HOST_ZIG=x86_64-macos ;;
    Linux-x86_64) HOST_WASI=x86_64-linux; HOST_WABT=linux-x64; HOST_ZIG=x86_64-linux ;;
    Linux-aarch64) HOST_WASI=arm64-linux; HOST_WABT=linux-arm64; HOST_ZIG=aarch64-linux ;;
    *) echo "fetch_toolchains: unsupported host $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac

WASI_VER=34
WASI_NAME="wasi-sdk-${WASI_VER}.0-${HOST_WASI}"
WASI_URL="https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_VER}/${WASI_NAME}.tar.gz"

WABT_VER=1.0.42
WABT_NAME="wabt-${WABT_VER}"
WABT_URL="https://github.com/WebAssembly/wabt/releases/download/${WABT_VER}/${WABT_NAME}-${HOST_WABT}.tar.gz"

ZIG_VER=0.17.0
ZIG_NAME="zig-${HOST_ZIG}-${ZIG_VER}"
ZIG_URL="https://ziglang.org/download/${ZIG_VER}/${ZIG_NAME}.tar.xz"

SDL3_VER=3.4.18
SDL3_RELEASE="https://github.com/libsdl-org/SDL/releases/download/release-${SDL3_VER}"
SDL3_MINGW_NAME="SDL3-${SDL3_VER}"
SDL3_MINGW_ARCHIVE="SDL3-devel-${SDL3_VER}-mingw.tar.gz"
SDL3_MINGW_URL="${SDL3_RELEASE}/${SDL3_MINGW_ARCHIVE}"

SDL3_APPLE_NAME="SDL3-${SDL3_VER}-apple"
SDL3_APPLE_ARCHIVE="SDL3-${SDL3_VER}.dmg"
SDL3_APPLE_URL="${SDL3_RELEASE}/${SDL3_APPLE_ARCHIVE}"

# sha256 per host archive. Unknown hosts fail closed.
sha_for() {
    case "$1" in
        "${WASI_NAME}.tar.gz")
            case "$HOST_WASI" in
                arm64-macos) echo "${NP_SHA_WASI_ARM64_MACOS:-}" ;;
                *) echo "" ;;
            esac ;;
        "${WABT_NAME}-${HOST_WABT}.tar.gz")
            case "$HOST_WABT" in
                macos-arm64) echo "${NP_SHA_WABT_MACOS_ARM64:-}" ;;
                *) echo "" ;;
            esac ;;
        "${ZIG_NAME}.tar.xz")
            case "$HOST_ZIG" in
                aarch64-macos) echo "${NP_SHA_ZIG_AARCH64_MACOS:-}" ;;
                *) echo "" ;;
            esac ;;
        "${SDL3_MINGW_ARCHIVE}") echo "${NP_SHA_SDL3_MINGW:-}" ;;
        "${SDL3_APPLE_ARCHIVE}") echo "${NP_SHA_SDL3_APPLE:-}" ;;
    esac
}

fetch() { # url archive-name extract-marker
    local url="$1" name="$2" marker="$3"
    if [ -e "$DEST/$marker" ]; then
        echo "have $marker"
        return
    fi
    local tmp="$DEST/.$name.part"
    [ -s "$tmp" ] || curl -fL --retry 3 -o "$tmp" "$url"
    local want got
    want="$(sha_for "$name")"
    got="$(shasum -a 256 "$tmp" | cut -d' ' -f1)"
    if [ -z "$want" ]; then
        echo "fetch_toolchains: no pinned sha256 for $name (got $got); pin it in tools/toolchains.lock" >&2
        rm -f "$tmp"
        exit 1
    fi
    if [ "$want" != "$got" ]; then
        echo "fetch_toolchains: sha256 mismatch for $name: want $want got $got" >&2
        rm -f "$tmp"
        exit 1
    fi
    case "$name" in
        *.dmg)
            local mnt
            mnt="$(mktemp -d)"
            hdiutil attach -nobrowse -readonly -mountpoint "$mnt" "$tmp" >/dev/null
            mkdir -p "$DEST/$marker"
            rsync -a --exclude dSYMs --exclude .DS_Store --exclude .logo "$mnt/" "$DEST/$marker/" ||
                { hdiutil detach "$mnt" >/dev/null; exit 1; }
            hdiutil detach "$mnt" >/dev/null
            rmdir "$mnt" ;;
        *) tar -xf "$tmp" -C "$DEST" ;;
    esac
    rm -f "$tmp"
    echo "installed $marker"
}

# shellcheck source=/dev/null
. "$ROOT/tools/toolchains.lock"

fetch "$WASI_URL" "${WASI_NAME}.tar.gz" "$WASI_NAME"
fetch "$WABT_URL" "${WABT_NAME}-${HOST_WABT}.tar.gz" "$WABT_NAME"

ln -sfn "$WASI_NAME" "$DEST/wasi-sdk"
ln -sfn "$WABT_NAME" "$DEST/wabt"

if [ "$WANT_WINDOWS" = 1 ]; then
    fetch "$ZIG_URL" "${ZIG_NAME}.tar.xz" "$ZIG_NAME"
    fetch "$SDL3_MINGW_URL" "$SDL3_MINGW_ARCHIVE" "$SDL3_MINGW_NAME"
    ln -sfn "$ZIG_NAME" "$DEST/zig"
    ln -sfn "$SDL3_MINGW_NAME" "$DEST/sdl3-mingw"
fi
if [ "$WANT_MACOS" = 1 ]; then
    fetch "$SDL3_APPLE_URL" "$SDL3_APPLE_ARCHIVE" "$SDL3_APPLE_NAME"
    ln -sfn "$SDL3_APPLE_NAME" "$DEST/sdl3-apple"
fi
echo "toolchains ready in $DEST"
