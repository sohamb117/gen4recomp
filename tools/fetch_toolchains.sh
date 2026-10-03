#!/usr/bin/env bash
# Fetch the pinned host toolchains the guest core needs into .cache/toolchains.
#
#   wasi-sdk  clang + wasm-ld + wasi-libc: compiles the game for wasm32, which is
#             what keeps guest pointers 32 bits wide on 64-bit hosts.
#   wabt      wasm2c: turns that module back into portable C for each host.
#
# Hashes are pinned; a mismatch aborts before anything is extracted.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${NP_TOOLCHAINS:-$ROOT/.cache/toolchains}"
mkdir -p "$DEST"

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) HOST_WASI=arm64-macos;  HOST_WABT=macos-arm64 ;;
    Darwin-x86_64) HOST_WASI=x86_64-macos; HOST_WABT=macos-x64 ;;
    Linux-x86_64) HOST_WASI=x86_64-linux; HOST_WABT=linux-x64 ;;
    Linux-aarch64) HOST_WASI=arm64-linux; HOST_WABT=linux-arm64 ;;
    *) echo "fetch_toolchains: unsupported host $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac

WASI_VER=34
WASI_NAME="wasi-sdk-${WASI_VER}.0-${HOST_WASI}"
WASI_URL="https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_VER}/${WASI_NAME}.tar.gz"

WABT_VER=1.0.42
WABT_NAME="wabt-${WABT_VER}"
WABT_URL="https://github.com/WebAssembly/wabt/releases/download/${WABT_VER}/${WABT_NAME}-${HOST_WABT}.tar.gz"

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
    tar -xzf "$tmp" -C "$DEST"
    rm -f "$tmp"
    echo "installed $marker"
}

# shellcheck source=/dev/null
. "$ROOT/tools/toolchains.lock"

fetch "$WASI_URL" "${WASI_NAME}.tar.gz" "$WASI_NAME"
fetch "$WABT_URL" "${WABT_NAME}-${HOST_WABT}.tar.gz" "$WABT_NAME"

ln -sfn "$WASI_NAME" "$DEST/wasi-sdk"
ln -sfn "$WABT_NAME" "$DEST/wabt"
echo "toolchains ready in $DEST"
