# Build helpers for wasm2c'd guest modules.
#
#   np_guest_link_flags(<out-var>)
#       wasm-ld flags every guest must link with, derived from the constants
#       in core/include/np_guest_abi.h so the layout has one source of truth.
#
#   np_add_guest_module(<module> <wasm-file>
#                       [TARGET <lib-target>]      default np_guest_<module>
#                       [NUM_OUTPUTS <n>]          wasm2c --num-outputs, for parallel builds
#                       [POSTPROCESS <script>]     default ${NP_GUEST_POSTPROCESS_<module>}
#                                                  or ${NP_GUEST_POSTPROCESS}, else none
#                       [DEPENDS <targets/files>...])
#       Runs wasm2c on <wasm-file> at build time (module name <module>, one of
#       diamond, pearl, platinum) and builds a static library from the
#       generated C plus np_module_glue.c.in. The optional post-processing
#       script runs on the generated C before it is compiled, e.g. to give
#       integer division ARM semantics instead of wasm traps:
#         *.cmake scripts run as  cmake -DNP_GUEST_MODULE=<module>
#                                       -DNP_GUEST_DIR=<dir with the .c/.h> -P <script>
#         anything else runs as   <script> <generated .c files...>   (edit in place)
#
#   np_link_guest_modules(<executable> [<guest lib targets>...])
#       Generates the module registry (np_guest_registry) for <executable>
#       and links np_runtime plus the given guest libraries into it. Every
#       binary that uses np_core needs exactly one call, even with no guests.

include_guard(GLOBAL)

# The functions are called from other directories (the shell's top level),
# where directory-scope variables of core/ are not visible, so anything they
# need is defined inside them or cached (NP_CORE_DIR, NP_WASM2C).
macro(_np_games OUT)
  set(${OUT} diamond pearl platinum black white heartgold soulsilver)
endmacro()

function(np_guest_link_flags OUT)
  file(STRINGS "${NP_CORE_DIR}/include/np_guest_abi.h" _lines
       REGEX "^#define NP_GUEST_(C_BASE|MEMORY_BYTES) ")
  foreach(_l IN LISTS _lines)
    if(_l MATCHES "^#define (NP_GUEST_[A-Z_]+) (0x[0-9A-Fa-f]+)u")
      math(EXPR _v "${CMAKE_MATCH_2}" OUTPUT_FORMAT DECIMAL)
      set(_${CMAKE_MATCH_1} ${_v})
    endif()
  endforeach()
  if(NOT _NP_GUEST_C_BASE OR NOT _NP_GUEST_MEMORY_BYTES)
    message(FATAL_ERROR "np_guest_link_flags: could not parse np_guest_abi.h")
  endif()
  # wasm-ld (wasi-sdk 34) puts the shadow stack at address 0 by default,
  # i.e. inside the DS map regardless of --global-base; --no-stack-first
  # moves it after the static data, above NP_GUEST_C_BASE.
  set(${OUT}
      -Wl,--global-base=${_NP_GUEST_C_BASE}
      -Wl,--no-stack-first
      -Wl,--initial-memory=${_NP_GUEST_MEMORY_BYTES}
      -Wl,--max-memory=${_NP_GUEST_MEMORY_BYTES}
      -Wl,-z,stack-size=1048576
      PARENT_SCOPE)
endfunction()

function(np_add_guest_module MODULE WASM_FILE)
  cmake_parse_arguments(ARG "" "TARGET;NUM_OUTPUTS;POSTPROCESS" "DEPENDS" ${ARGN})
  _np_games(games)
  if(NOT MODULE IN_LIST games)
    message(FATAL_ERROR "np_add_guest_module: module must be one of ${games}, got '${MODULE}'")
  endif()
  if(NOT NP_WASM2C)
    message(FATAL_ERROR "np_add_guest_module: wasm2c not found; set NP_WABT_ROOT or NP_WASM2C")
  endif()
  set(target np_guest_${MODULE})
  if(ARG_TARGET)
    set(target ${ARG_TARGET})
  endif()
  set(num 1)
  if(ARG_NUM_OUTPUTS)
    set(num ${ARG_NUM_OUTPUTS})
  endif()
  set(post "")
  if(ARG_POSTPROCESS)
    set(post "${ARG_POSTPROCESS}")
  elseif(NP_GUEST_POSTPROCESS_${MODULE})
    set(post "${NP_GUEST_POSTPROCESS_${MODULE}}")
  elseif(NP_GUEST_POSTPROCESS)
    set(post "${NP_GUEST_POSTPROCESS}")
  endif()

  set(dir "${CMAKE_CURRENT_BINARY_DIR}/${target}")
  file(MAKE_DIRECTORY "${dir}")
  if(num GREATER 1)
    set(sources "")
    math(EXPR last "${num} - 1")
    foreach(i RANGE ${last})
      list(APPEND sources "${dir}/${MODULE}_${i}.c")
    endforeach()
    set(headers "${dir}/${MODULE}.h" "${dir}/${MODULE}-impl.h")
    set(num_flag --num-outputs=${num})
  else()
    set(sources "${dir}/${MODULE}.c")
    set(headers "${dir}/${MODULE}.h")
    set(num_flag "")
  endif()

  set(post_cmd "")
  set(post_dep "")
  if(post)
    get_filename_component(post "${post}" ABSOLUTE)
    set(post_dep "${post}")
    if(post MATCHES "\\.cmake$")
      set(post_cmd COMMAND "${CMAKE_COMMAND}" -DNP_GUEST_MODULE=${MODULE} "-DNP_GUEST_DIR=${dir}" -P "${post}")
    else()
      set(post_cmd COMMAND "${post}" ${sources})
    endif()
  endif()

  add_custom_command(
    OUTPUT ${sources} ${headers}
    COMMAND "${NP_WASM2C}" "${WASM_FILE}" -n ${MODULE} ${num_flag} -o "${dir}/${MODULE}.c"
    ${post_cmd}
    DEPENDS "${WASM_FILE}" ${post_dep} ${ARG_DEPENDS}
    COMMENT "wasm2c ${MODULE}: ${WASM_FILE}"
    VERBATIM)
  set(NP_MODULE ${MODULE})
  configure_file("${NP_CORE_DIR}/runtime/np_module_glue.c.in" "${dir}/np_glue_${MODULE}.c" @ONLY)

  add_library(${target} STATIC ${sources} "${dir}/np_glue_${MODULE}.c")
  target_include_directories(${target} PRIVATE "${dir}")
  target_link_libraries(${target} PUBLIC np_runtime)
  set_target_properties(${target} PROPERTIES NP_GUEST_MODULE ${MODULE} C_STANDARD 11 C_STANDARD_REQUIRED ON)
  # Generated code: always optimised (it is unusably slow at -O0), wasm
  # loads/stores alias freely, and its warnings are not ours to fix.
  if(MSVC)
    set_source_files_properties(${sources} PROPERTIES COMPILE_OPTIONS "/O2;/w")
  else()
    set_source_files_properties(${sources} PROPERTIES COMPILE_OPTIONS "-O2;-fno-strict-aliasing;-w")
  endif()
endfunction()

function(np_link_guest_modules EXE)
  set(decls "")
  _np_games(games)
  foreach(game IN LISTS games)
    set(entry_${game} "NULL")
  endforeach()
  foreach(t IN LISTS ARGN)
    get_target_property(m ${t} NP_GUEST_MODULE)
    if(NOT m)
      message(FATAL_ERROR "np_link_guest_modules: ${t} was not made by np_add_guest_module")
    endif()
    if(NOT entry_${m} STREQUAL "NULL")
      message(FATAL_ERROR "np_link_guest_modules: module ${m} given twice")
    endif()
    string(APPEND decls "extern const np_guest_module np_guest_module_${m};\n")
    set(entry_${m} "&np_guest_module_${m}")
  endforeach()
  set(out "${CMAKE_CURRENT_BINARY_DIR}/${EXE}_np_registry.c")
  file(CONFIGURE OUTPUT "${out}" CONTENT
"/* Generated by np_link_guest_modules() for ${EXE}. */
#include <stddef.h>
#include \"np_guest_module.h\"
${decls}
const np_guest_module *const np_guest_registry[NP_GAME_COUNT] = {
    ${entry_diamond}, /* NP_GAME_DIAMOND */
    ${entry_pearl}, /* NP_GAME_PEARL */
    ${entry_platinum}, /* NP_GAME_PLATINUM */
    ${entry_black}, /* NP_GAME_BLACK */
    ${entry_white}, /* NP_GAME_WHITE */
    ${entry_heartgold}, /* NP_GAME_HEARTGOLD */
    ${entry_soulsilver}, /* NP_GAME_SOULSILVER */
};
" @ONLY)
  target_sources(${EXE} PRIVATE "${out}")
  # Guest libraries carry np_runtime as a PUBLIC dependency (after
  # themselves); name it directly only when there are none.
  if(ARGN)
    target_link_libraries(${EXE} PRIVATE ${ARGN})
  else()
    target_link_libraries(${EXE} PRIVATE np_runtime)
  endif()
endfunction()
