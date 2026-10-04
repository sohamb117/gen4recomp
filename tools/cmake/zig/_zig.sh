# Sourced by the zig wrappers: locates zig and keeps its cache in the repo.
#   NP_ZIG                 zig binary (default .cache/toolchains/zig/zig)
#   ZIG_GLOBAL_CACHE_DIR   zig's cache, incl. the mingw CRT it builds on
#                          first use (default .cache/zig)
_np_root="$(cd "$(dirname "$0")/../../.." && pwd)"
NP_ZIG="${NP_ZIG:-$_np_root/.cache/toolchains/zig/zig}"
export ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$_np_root/.cache/zig}"
export ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$ZIG_GLOBAL_CACHE_DIR}"
