# Core runtime tests: mock_guest.c is compiled to wasm with wasi-sdk, run
# through wasm2c twice (as "diamond" and as "platinum", which also proves two
# modules link side by side), and driven by test_core.c through np_core.h.
set(_wasi_clang "${NP_WASI_SDK}/bin/clang")
if(NOT EXISTS "${_wasi_clang}")
  message(FATAL_ERROR "wasi-sdk clang not found at ${_wasi_clang}; run tools/fetch_toolchains.sh or set NP_BUILD_TESTS=OFF")
endif()

np_guest_link_flags(_np_guest_ldflags)
set(_mock_wasm "${CMAKE_CURRENT_BINARY_DIR}/mock_guest.wasm")
add_custom_command(
  OUTPUT "${_mock_wasm}"
  COMMAND "${_wasi_clang}" --target=wasm32-wasip1 -std=c11 -O2 -Wall -Wextra -Werror
          "-I${NP_CORE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/mock_guest.c"
          ${_np_guest_ldflags} -o "${_mock_wasm}"
  DEPENDS mock_guest.c "${NP_CORE_DIR}/include/np_guest_abi.h"
  COMMENT "wasi-sdk: mock_guest.wasm"
  VERBATIM)
add_custom_target(np_mock_guest_wasm DEPENDS "${_mock_wasm}")

np_add_guest_module(diamond "${_mock_wasm}" TARGET np_test_guest_diamond DEPENDS np_mock_guest_wasm)
np_add_guest_module(platinum "${_mock_wasm}" TARGET np_test_guest_platinum NUM_OUTPUTS 2 DEPENDS np_mock_guest_wasm)

add_executable(test_core test_core.c)
set_target_properties(test_core PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
np_link_guest_modules(test_core np_test_guest_diamond np_test_guest_platinum)
if(NOT MSVC)
  target_compile_options(test_core PRIVATE -Wall -Wextra)
  target_link_libraries(test_core PRIVATE m)
endif()

add_test(NAME core_runtime COMMAND test_core)
