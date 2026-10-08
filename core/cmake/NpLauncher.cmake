# np_compiler_launcher(REPO): the C compiler launcher of a native build
# (core/, shell/), unless the caller set CMAKE_C_COMPILER_LAUNCHER; none for
# MSVC or cross builds (iOS, zig's Windows target).
#
#  - On Apple Silicon, /usr/bin/arch -arm64 first. Under an x86_64 cmake or
#    ninja (/usr/local Homebrew), Rosetta also translates the compiler, which
#    then takes ~1.8x as long for the same object (platinum_7.c: 13.0 s
#    against 7.3 s, byte-identical .o).
#  - REPO/tools/ccache.sh when it says so at configure time (its header:
#    ccache present, no NP_NO_CCACHE=1, enough free disk): one compile cache
#    for every worktree.
macro(np_compiler_launcher repo)
  if(NOT CMAKE_C_COMPILER_LAUNCHER AND NOT MSVC AND NOT CMAKE_CROSSCOMPILING)
    set(_np_launcher)
    if(CMAKE_HOST_APPLE)
      execute_process(COMMAND sysctl -n hw.optional.arm64 OUTPUT_VARIABLE _np_host_arm64
                      ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
      execute_process(COMMAND lipo -archs "${CMAKE_C_COMPILER}" OUTPUT_VARIABLE _np_cc_archs
                      ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
      if(_np_host_arm64 STREQUAL "1" AND _np_cc_archs MATCHES "arm64")
        list(APPEND _np_launcher /usr/bin/arch -arm64)
      endif()
    endif()
    execute_process(COMMAND "${repo}/tools/ccache.sh" --enabled OUTPUT_VARIABLE _np_ccache
                    ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_np_ccache)
      list(APPEND _np_launcher "${_np_ccache}")
    endif()
    if(_np_launcher)
      set(CMAKE_C_COMPILER_LAUNCHER ${_np_launcher})
    endif()
  endif()
endmacro()
