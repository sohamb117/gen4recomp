#!/usr/bin/env bash
# Build a decompilation's matching ROM inside the romtools container.
#
#   tools/rom_build.sh platinum        -> games/platinum/build/rom/pokeplatinum.us.nds
#   tools/rom_build.sh image           -> (re)build the container image only
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
        run "$ROOT/games/platinum" "make BUILD=build/rom -j\$(nproc) ${2:-}" ;;
    *)
        echo "usage: $0 image|platinum" >&2
        exit 2 ;;
esac
