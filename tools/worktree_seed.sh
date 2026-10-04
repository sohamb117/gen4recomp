#!/usr/bin/env bash
# Seed a new git worktree with this checkout's gitignored build inputs.
#
#   tools/worktree_seed.sh ../nativeplat-dp-audio
#
# A fresh worktree has the sources but none of the ROM builds, downloaded SDK
# subprojects, compiler wrappers or game build trees, which take an hour of
# containers to regenerate. Every ignored path is copied as an APFS clone
# (cp -c: no extra disk until a side writes) with timestamps preserved, so make
# in the new tree sees the same up-to-date state. The repository-root build/
# directory (per-agent native builds) is skipped, and .cache (read-only
# toolchains) is linked instead of copied.
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DST="$(cd "${1:?usage: worktree_seed.sh <worktree path>}" && pwd)"
[ "$SRC" != "$DST" ] || { echo "worktree_seed: refusing to seed the source checkout" >&2; exit 2; }
git -C "$DST" rev-parse --is-inside-work-tree >/dev/null

git -C "$SRC" status --ignored --porcelain=v1 -z |
while IFS= read -r -d '' entry; do
    case "$entry" in
        '!! '*) path="${entry#!! }" ;;
        *) continue ;;
    esac
    path="${path%/}"
    case "$path" in
        build|.cache|*/__pycache__|__pycache__|*.pyc) continue ;;
    esac
    [ -e "$DST/$path" ] && continue
    mkdir -p "$DST/$(dirname "$path")"
    cp -c -R -p "$SRC/$path" "$DST/$path"
done

[ -e "$DST/.cache" ] || ln -s "$SRC/.cache" "$DST/.cache"
echo "seeded $DST from $SRC"
