cmake_minimum_required(VERSION 3.20)

project(
  movescape
  VERSION 0.1.0
  DESCRIPTION "A pedagogical, DCC-style decompiler for Aptos Move bytecode"
  LANGUAGES CXX
)

option(MOVESCAPE_BUILD_TESTS "Build the movescape test suite" ON)
option(MOVESCAPE_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
set(MOVESCAPE_APTOS_EXECUTABLE "" CACHE FILEPATH
  "Optional official Aptos CLI used for real compiler round-trip tests")

add_library(movescape
  src/binary_reader.cpp
  src/format.cpp
  src/loader.cpp
  src/module_loader.cpp
  src/metadata.cpp
  src/package_loader.cpp
  src/opcode.cpp
  src/source_names.cpp
  src/source_map.cpp
  src/validator.cpp
  src/semantic.cpp
  src/module_compare.cpp
  src/body_compare.cpp
  src/behavior.cpp
  src/round_trip.cpp
  src/disassembler.cpp
  src/cfg.cpp
  src/graph_analysis.cpp
  src/stackless_ir.cpp
  src/type_analysis.cpp
  src/local_analysis.cpp
  src/borrow_analysis.cpp
  src/expression_ir.cpp
  src/region.cpp
  src/move_emitter.cpp
)
add_library(movescape::movescape ALIAS movescape)

target_compile_features(movescape PUBLIC cxx_std_20)
target_include_directories(movescape
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(movescape PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Werror=return-type
  )
endif()

if(MOVESCAPE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(movescape PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(movescape PUBLIC -fsanitize=address,undefined)
endif()

add_executable(movescape_cli tools/movescape.cpp)
set_target_properties(movescape_cli PROPERTIES OUTPUT_NAME movescape)
target_link_libraries(movescape_cli PRIVATE movescape::movescape)

if(MOVESCAPE_BUILD_TESTS)
  enable_testing()
  add_executable(movescape_tests
    tests/test_main.cpp
    tests/binary_reader_tests.cpp
    tests/envelope_tests.cpp
    tests/module_loader_tests.cpp
    tests/metadata_tests.cpp
    tests/package_loader_tests.cpp
    tests/source_names_tests.cpp
    tests/source_map_tests.cpp
    tests/validator_tests.cpp
    tests/verifier_parity_tests.cpp
    tests/semantic_tests.cpp
    tests/module_compare_tests.cpp
    tests/body_compare_tests.cpp
    tests/behavior_tests.cpp
    tests/round_trip_tests.cpp
    tests/disassembler_tests.cpp
    tests/cfg_tests.cpp
    tests/graph_analysis_tests.cpp
    tests/stackless_ir_tests.cpp
    tests/local_analysis_tests.cpp
    tests/borrow_analysis_tests.cpp
    tests/expression_ir_tests.cpp
    tests/region_tests.cpp
    tests/move_emitter_tests.cpp
  )
  target_link_libraries(movescape_tests PRIVATE movescape::movescape)
  add_executable(movescape_irreducible_fixture
    tests/irreducible_fixture.cpp
  )
  target_link_libraries(movescape_irreducible_fixture PRIVATE movescape::movescape)
  add_test(NAME movescape_tests COMMAND movescape_tests)
  if(MOVESCAPE_APTOS_EXECUTABLE)
    add_test(NAME movescape_real_compiler_round_trip
      COMMAND ${CMAKE_COMMAND}
        -DAPTOS_EXECUTABLE=${MOVESCAPE_APTOS_EXECUTABLE}
        -DMOVESCAPE_EXECUTABLE=$<TARGET_FILE:movescape_cli>
        -DIRREDUCIBLE_FIXTURE_EXECUTABLE=$<TARGET_FILE:movescape_irreducible_fixture>
        -DDEMO_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/round_trip_demo/original
        -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/real-compiler-round-trip
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/real_compiler_round_trip.cmake
    )
  endif()
  if(MOVESCAPE_ENABLE_SANITIZERS)
    set_tests_properties(movescape_tests PROPERTIES
      ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
    )
    if(MOVESCAPE_APTOS_EXECUTABLE)
      set_tests_properties(movescape_real_compiler_round_trip PROPERTIES
        ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
      )
    endif()
  endif()
endif()
