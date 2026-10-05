#!/usr/bin/env bash
# Build a decompilation's matching ROM inside the romtools container.
#
#   tools/rom_build.sh platinum        -> games/platinum/build/rom/pokeplatinum.us.nds
#   tools/rom_build.sh diamond         -> games/diamond/build/diamond.us/pokediamond.us.nds
#   tools/rom_build.sh pearl           -> games/diamond/build/pearl.us/pokepearl.us.nds
#   tools/rom_build.sh heartgold       -> games/heartgold/build/heartgold.us/pokeheartgold.us.nds
#   tools/rom_build.sh soulsilver      -> games/heartgold/build/soulsilver.us/pokesoulsilver.us.nds
#   tools/rom_build.sh image           -> (re)build the container image only
#
# Diamond/Pearl and HeartGold/SoulSilver replace the decomps' proprietary
# toolchain (pret's mwccarm.zip and NitroSDK tools/bin) with open stand-ins;
# see tools/ntr/setup.sh. It needs games/platinum/tools/metroskrew, fetched by
# the Platinum build. Jobs: NP_JOBS (default 6).
#
# The repository is mounted at the same absolute path inside the container, so
# paths recorded in build trees (xMAP, depfiles) stay valid on the host.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=nativeplat-romtools:1
CONTEXT="${NP_DOCKER_CONTEXT:-orbstack}"
DOCKER=(docker --context "$CONTEXT")
JOBS="${NP_JOBS:-6}"

build_image() {
    "${DOCKER[@]}" build --platform linux/amd64 -t "$IMAGE" \
        -f "$ROOT/tools/docker/romtools.Dockerfile" "$ROOT/tools/docker"
}

run() { # workdir, command...
    local wd="$1"; shift
    "${DOCKER[@]}" run --rm --platform linux/amd64 \
        -v "$ROOT:$ROOT" -w "$wd" \
        -e HOME=/tmp -u "$(id -u):$(id -g)" \
        "$IMAGE" bash -c "$*"
}

case "${1:-}" in
    image)
        build_image ;;
    platinum)
        "${DOCKER[@]}" image inspect "$IMAGE" >/dev/null 2>&1 || build_image
        run "$ROOT/games/platinum" "make BUILD=build/rom -j$JOBS ${2:-}" ;;
    diamond|pearl)
        "${DOCKER[@]}" image inspect "$IMAGE" >/dev/null 2>&1 || build_image
        run "$ROOT/games/diamond" "$ROOT/tools/ntr/setup.sh diamond && make NOWINE=1 MWASMARM_PATCHER=true -j$JOBS $1 ${2:-}" ;;
    heartgold|soulsilver)
        # NOWINE=1 runs the tools directly; its WINPATH (wslpath) does not
        # exist here, so give common.mk the native project root it would map.
        "${DOCKER[@]}" image inspect "$IMAGE" >/dev/null 2>&1 || build_image
        run "$ROOT/games/heartgold" "$ROOT/tools/ntr/setup.sh heartgold && make NOWINE=1 PROJECT_ROOT_NT=$ROOT/games/heartgold/ -j$JOBS $1 ${2:-}" ;;
    *)
        echo "usage: $0 image|platinum|diamond|pearl|heartgold|soulsilver" >&2
        exit 2 ;;
esac
