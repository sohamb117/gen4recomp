#!/bin/sh
# tools/ccache.sh COMPILER ARGS...: run one compile through ccache, with one
# cache shared by every worktree, so a fresh worktree's first build reuses
# what main or another worktree already compiled.
# tools/ccache.sh --enabled: print this script's path if builds should use
# it, nothing if not: NP_NO_CCACHE=1, no ccache on PATH, or less than
# NP_CCACHE_MIN_FREE_GB (default 8) GiB free on the cache's disk.
#
# The compiler launcher of the wasm guest builds (games/*/pc/Makefile.wasm,
# games/gba-common/tools/gbabuild.py) and of the native CMake builds (core/,
# shell/), each asking --enabled once when it configures (a make run, a cmake
# configure, a gbabuild run). A compile with NP_NO_CCACHE=1 or no ccache runs
# the compiler directly; so does ccache for what it does not cache (links,
# -E, LLVM IR input).
#
#   cache dir    $NP_CCACHE_DIR, default ~/Library/Caches/nativeplat-ccache;
#                its own ccache.conf caps it (max_size, 2.5G when --enabled
#                creates it; ccache -M SIZE changes it)
#   base_dir     the directory holding the worktrees ($NP_CCACHE_BASEDIR,
#                default this checkout's parent): absolute paths under it
#                reach the compiler relative to its working directory, so
#                the same file in two worktrees hashes the same (and the
#                wasm modules no longer embed the worktree's path)
#   hash_dir     off: matters only for -g compiles, whose working directory
#                is then not hashed; a -g hit can name another worktree's
#                build directory as its compilation directory (no guest or
#                Release core build uses -g)
#   direct_mode  off: preprocessor mode hashes what each compile actually
#                includes, while direct mode misses a new header that shadows
#                one earlier on the search path (games/platinum/pc/Makefile
#                saw that miscompile with pc/include's register shadows)
#   sloppiness   none: no source uses __DATE__ or __TIME__, and the
#                __FILE__/__BASE_FILE__ a TU expands (sinit.h's static-init
#                records) is its relative path, which ccache hashes
case $0 in */*) here=${0%/*} ;; *) here=. ;; esac
CCACHE_DIR=${NP_CCACHE_DIR:-$HOME/Library/Caches/nativeplat-ccache}
if [ "${1:-}" = --enabled ]; then
    case ${NP_NO_CCACHE:-0} in 0 | '') ;; *) exit 0 ;; esac
    command -v ccache >/dev/null 2>&1 || exit 0
    mkdir -p "$CCACHE_DIR" 2>/dev/null || exit 0
    free=$(df -k "$CCACHE_DIR" | awk 'NR == 2 { print $4 }')
    [ "${free:-0}" -ge $((${NP_CCACHE_MIN_FREE_GB:-8} * 1024 * 1024)) ] || exit 0
    [ -f "$CCACHE_DIR/ccache.conf" ] || CCACHE_DIR=$CCACHE_DIR ccache -M 2.5G >/dev/null 2>&1
    echo "$(cd "$here" && pwd)/ccache.sh"
    exit 0
fi
case ${NP_NO_CCACHE:-0} in 0 | '') ;; *) exec "$@" ;; esac
command -v ccache >/dev/null 2>&1 || exec "$@"
CCACHE_BASEDIR=${NP_CCACHE_BASEDIR:-$(cd "$here/../.." && pwd)}
CCACHE_NOHASHDIR=1
CCACHE_NODIRECT=1
export CCACHE_DIR CCACHE_BASEDIR CCACHE_NOHASHDIR CCACHE_NODIRECT
exec ccache "$@"
