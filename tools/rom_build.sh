#!/usr/bin/env bash
# Build a decompilation's matching ROM inside the romtools container.
#
#   tools/rom_build.sh platinum        -> games/platinum/build/rom/pokeplatinum.us.nds
#   tools/rom_build.sh diamond         -> games/diamond/build/diamond.us/pokediamond.us.nds
#   tools/rom_build.sh pearl           -> games/diamond/build/pearl.us/pokepearl.us.nds
#   tools/rom_build.sh emerald         -> .cache/gba/pokeemerald/pokeemerald.{gba,elf}
#   tools/rom_build.sh ruby|sapphire   -> .cache/gba/pokeruby/poke{ruby,sapphire}.{gba,elf}
#   tools/rom_build.sh image           -> (re)build the container image only
#
# The GBA decomps live outside git in .cache/gba (pinned clones of
# pret/pokeemerald and pret/pokeruby, see games/gba-common/README.md) with
# pret/agbcc built beside them; the guest build needs each ROM's ELF, which
# places every symbol at its cartridge address.
#
# Diamond/Pearl replace pokediamond's proprietary toolchain (pret's mwccarm.zip
# and NitroSDK 3.2 tools/bin) with open stand-ins; see tools/diamond/setup.sh.
# It needs games/platinum/tools/metroskrew, fetched by the Platinum build.
#
# The repository is mounted at the same absolute path inside the container, so
# paths recorded in build trees (xMAP, depfiles) stay valid on the host.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=nativeplat-romtools:1
CONTEXT="${NP_DOCKER_CONTEXT:-orbstack}"
DOCKER=(docker --context "$CONTEXT")

build_image() {
    "${DOCKER[@]}" build --platform linux/amd64 -t "$IMAGE" \
        -f "$ROOT/tools/docker/romtools.Dockerfile" "$ROOT/tools/docker"
}

run() { # workdir, command...
    local wd="$1"; shift
    # .cache is a link into the main checkout in worktrees: mount its target
    local cache; cache="$(cd "$ROOT/.cache" 2>/dev/null && pwd -P || true)"
    local extra=()
    [ -n "$cache" ] && [ "$cache" != "$ROOT/.cache" ] && extra=(-v "$cache:$cache")
    "${DOCKER[@]}" run --rm --platform linux/amd64 \
        -v "$ROOT:$ROOT" "${extra[@]}" -w "$wd" \
        -e HOME=/tmp -u "$(id -u):$(id -g)" \
        "$IMAGE" bash -c "$*"
}

case "${1:-}" in
    image)
        build_image ;;
    platinum)
        "${DOCKER[@]}" image inspect "$IMAGE" >/dev/null 2>&1 || build_image
        run "$ROOT/games/platinum" "make BUILD=build/rom -j\$(nproc) ${2:-}" ;;
    diamond|pearl)
        "${DOCKER[@]}" image inspect "$IMAGE" >/dev/null 2>&1 || build_image
        run "$ROOT/games/diamond" "$ROOT/tools/diamond/setup.sh && make NOWINE=1 MWASMARM_PATCHER=true -j\$(nproc) $1 ${2:-}" ;;
    emerald|ruby|sapphire)
        "${DOCKER[@]}" image inspect "$IMAGE" >/dev/null 2>&1 || build_image
        dir="$(cd "$ROOT/.cache/gba/$([ "$1" = emerald ] && echo pokeemerald || echo pokeruby)" && pwd -P)"
        agbcc="$(cd "$ROOT/.cache/gba/agbcc" && pwd -P)"
        target=$([ "$1" = emerald ] && echo "" || echo "$1")
        run "$dir" "[ -x tools/agbcc/bin/agbcc ] || (cd '$agbcc' && ./install.sh '$dir'); make -j4 $target ${2:-}" ;;
    *)
        echo "usage: $0 image|platinum|diamond|pearl|emerald|ruby|sapphire" >&2
        exit 2 ;;
esac
