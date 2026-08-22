if(NOT EXISTS "${APTOS_EXECUTABLE}")
  message(FATAL_ERROR "Aptos executable does not exist: ${APTOS_EXECUTABLE}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/original")
file(COPY "${DEMO_SOURCE}/Move.toml" DESTINATION "${TEST_ROOT}/original")
file(COPY "${DEMO_SOURCE}/sources" DESTINATION "${TEST_ROOT}/original")
file(COPY "${DEMO_SOURCE}/tests" DESTINATION "${TEST_ROOT}/original")

execute_process(
  COMMAND "${APTOS_EXECUTABLE}" move compile --package-dir "${TEST_ROOT}/original"
  RESULT_VARIABLE original_status
  OUTPUT_VARIABLE original_output
  ERROR_VARIABLE original_error
)
if(NOT original_status EQUAL 0)
  message(FATAL_ERROR
    "official compiler rejected original demo:\n${original_output}${original_error}")
endif()

# Generate an actually decompiled irreducible function from an in-memory
# bytecode module, then require the official compiler to accept the typed
# dispatcher source. Ordinary Move source cannot express the original
# multiple-entry cycle directly, so compiling a hand-written fixture would not
# exercise Movescape's fallback renderer.
file(MAKE_DIRECTORY "${TEST_ROOT}/irreducible/sources")
file(WRITE "${TEST_ROOT}/irreducible/Move.toml"
  "[package]\nname = \"MovescapeIrreducibleFixture\"\nversion = \"0.0.0\"\n")
execute_process(
  COMMAND "${IRREDUCIBLE_FIXTURE_EXECUTABLE}"
  RESULT_VARIABLE irreducible_emit_status
  OUTPUT_FILE "${TEST_ROOT}/irreducible/sources/IrreducibleFixture.move"
  ERROR_VARIABLE irreducible_emit_error
)
if(NOT irreducible_emit_status EQUAL 0)
  message(FATAL_ERROR
    "movescape failed to emit the irreducible fixture:\n${irreducible_emit_error}")
endif()
execute_process(
  COMMAND "${APTOS_EXECUTABLE}" move compile
          --package-dir "${TEST_ROOT}/irreducible"
  RESULT_VARIABLE irreducible_compile_status
  OUTPUT_VARIABLE irreducible_compile_output
  ERROR_VARIABLE irreducible_compile_error
)
if(NOT irreducible_compile_status EQUAL 0)
  message(FATAL_ERROR
    "official compiler rejected the typed irreducible dispatcher:\n${irreducible_compile_output}${irreducible_compile_error}")
endif()
file(GLOB_RECURSE irreducible_modules
  "${TEST_ROOT}/irreducible/build/*/bytecode_modules/IrreducibleFixture.mv")
list(LENGTH irreducible_modules irreducible_module_count)
if(NOT irreducible_module_count EQUAL 1)
  message(FATAL_ERROR
    "typed irreducible fixture did not compile to exactly one module")
endif()

file(GLOB_RECURSE original_modules
  "${TEST_ROOT}/original/build/*/bytecode_modules/RoundTripDemo.mv")
file(GLOB_RECURSE original_maps
  "${TEST_ROOT}/original/build/*/source_maps/RoundTripDemo.mvsm")
file(GLOB_RECURSE helper_modules
  "${TEST_ROOT}/original/build/*/bytecode_modules/Helper.mv")
file(GLOB_RECURSE helper_maps
  "${TEST_ROOT}/original/build/*/source_maps/Helper.mvsm")
file(GLOB_RECURSE resource_modules
  "${TEST_ROOT}/original/build/*/bytecode_modules/ResourceDemo.mv")
file(GLOB_RECURSE resource_maps
  "${TEST_ROOT}/original/build/*/source_maps/ResourceDemo.mvsm")
file(GLOB_RECURSE bool_modules
  "${TEST_ROOT}/original/build/*/bytecode_modules/BoolDemo.mv")
file(GLOB_RECURSE bool_maps
  "${TEST_ROOT}/original/build/*/source_maps/BoolDemo.mvsm")
file(GLOB_RECURSE number_modules
  "${TEST_ROOT}/original/build/*/bytecode_modules/NumberDemo.mv")
file(GLOB_RECURSE number_maps
  "${TEST_ROOT}/original/build/*/source_maps/NumberDemo.mvsm")
list(LENGTH original_modules module_count)
list(LENGTH original_maps map_count)
list(LENGTH helper_modules helper_module_count)
list(LENGTH helper_maps helper_map_count)
list(LENGTH resource_modules resource_module_count)
list(LENGTH resource_maps resource_map_count)
list(LENGTH bool_modules bool_module_count)
list(LENGTH bool_maps bool_map_count)
list(LENGTH number_modules number_module_count)
list(LENGTH number_maps number_map_count)
if(NOT module_count EQUAL 1 OR NOT map_count EQUAL 1 OR
   NOT helper_module_count EQUAL 1 OR NOT helper_map_count EQUAL 1 OR
   NOT resource_module_count EQUAL 1 OR NOT resource_map_count EQUAL 1 OR
   NOT bool_module_count EQUAL 1 OR NOT bool_map_count EQUAL 1 OR
   NOT number_module_count EQUAL 1 OR NOT number_map_count EQUAL 1)
  message(FATAL_ERROR
    "official compiler did not emit all modules and source maps")
endif()
list(GET original_modules 0 original_module)
list(GET original_maps 0 original_map)
list(GET helper_modules 0 helper_module)
list(GET helper_maps 0 helper_map)
list(GET resource_modules 0 resource_module)
list(GET resource_maps 0 resource_map)
list(GET bool_modules 0 bool_module)
list(GET bool_maps 0 bool_map)
list(GET number_modules 0 number_module)
list(GET number_maps 0 number_map)

# The decoder remains raw-only unless explicitly enabled, then interprets the
# pinned Aptos compilation and runtime-v1 BCS schemas while retaining raw hex.
execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" metadata "${original_module}"
  RESULT_VARIABLE raw_metadata_status
  OUTPUT_VARIABLE raw_metadata_output
  ERROR_VARIABLE raw_metadata_error
)
if(NOT raw_metadata_status EQUAL 0 OR
   NOT raw_metadata_output MATCHES "decoder: raw-only" OR
   NOT raw_metadata_output MATCHES "value-hex: 0003322e3003322e34" OR
   raw_metadata_output MATCHES "compiler-version:")
  message(FATAL_ERROR
    "raw metadata mode was not lossless and opt-in:\n${raw_metadata_output}${raw_metadata_error}")
endif()

file(MAKE_DIRECTORY "${TEST_ROOT}/metadata-probe/sources")
file(WRITE "${TEST_ROOT}/metadata-probe/Move.toml"
  "[package]\nname = \"MovescapeMetadataProbe\"\nversion = \"0.0.0\"\n\n[addresses]\nprobe = \"0x42\"\n")
file(WRITE "${TEST_ROOT}/metadata-probe/sources/Probe.move"
  "module probe::Probe {\n    /// The requested value was seven.\n    const ESEVEN: u64 = 7;\n\n    #[view]\n    public fun answer(): u64 { ESEVEN }\n}\n")
execute_process(
  COMMAND "${APTOS_EXECUTABLE}" move compile
          --package-dir "${TEST_ROOT}/metadata-probe"
          --skip-fetch-latest-git-deps
  RESULT_VARIABLE metadata_compile_status
  OUTPUT_VARIABLE metadata_compile_output
  ERROR_VARIABLE metadata_compile_error
)
if(NOT metadata_compile_status EQUAL 0)
  message(FATAL_ERROR
    "official compiler rejected metadata fixture:\n${metadata_compile_output}${metadata_compile_error}")
endif()
file(GLOB_RECURSE metadata_modules
  "${TEST_ROOT}/metadata-probe/build/*/bytecode_modules/Probe.mv")
list(LENGTH metadata_modules metadata_module_count)
if(NOT metadata_module_count EQUAL 1)
  message(FATAL_ERROR "metadata fixture did not emit exactly one module")
endif()
list(GET metadata_modules 0 metadata_module)
execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" metadata "${metadata_module}"
          --decode-aptos-v1
  RESULT_VARIABLE metadata_decode_status
  OUTPUT_VARIABLE metadata_decode_output
  ERROR_VARIABLE metadata_decode_error
)
if(NOT metadata_decode_status EQUAL 0 OR
   NOT metadata_decode_output MATCHES "compiler-version: \"2.0\"" OR
   NOT metadata_decode_output MATCHES "language-version: \"2.4\"" OR
   NOT metadata_decode_output MATCHES "decoded-kind: aptos-runtime-v1" OR
   NOT metadata_decode_output MATCHES "7: \"ESEVEN\" - \"The requested value was seven.\"" OR
   NOT metadata_decode_output MATCHES "\"answer\": view-function\\(\\)")
  message(FATAL_ERROR
    "Movescape did not decode official Aptos metadata:\n${metadata_decode_output}${metadata_decode_error}")
endif()

file(MAKE_DIRECTORY "${TEST_ROOT}/decompiled/sources")
file(MAKE_DIRECTORY "${TEST_ROOT}/decompiled/tests")
file(COPY "${DEMO_SOURCE}/tests/" DESTINATION "${TEST_ROOT}/decompiled/tests")
file(WRITE "${TEST_ROOT}/decompiled/Move.toml"
  "[package]\nname = \"MovescapeRealCompilerTest\"\nversion = \"0.0.0\"\n")
execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" decompile "${original_module}"
          "${TEST_ROOT}/decompiled/sources/RoundTripDemo.move"
          --source-map "${original_map}"
  RESULT_VARIABLE decompile_status
  OUTPUT_VARIABLE decompile_output
  ERROR_VARIABLE decompile_error
)
if(NOT decompile_status EQUAL 0)
  message(FATAL_ERROR
    "movescape failed to decompile demo:\n${decompile_output}${decompile_error}")
endif()
execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" decompile "${helper_module}"
          "${TEST_ROOT}/decompiled/sources/Helper.move"
          --source-map "${helper_map}"
  RESULT_VARIABLE helper_decompile_status
  OUTPUT_VARIABLE helper_decompile_output
  ERROR_VARIABLE helper_decompile_error
)
if(NOT helper_decompile_status EQUAL 0)
  message(FATAL_ERROR
    "movescape failed to decompile helper module:\n${helper_decompile_output}${helper_decompile_error}")
endif()
execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" decompile "${resource_module}"
          "${TEST_ROOT}/decompiled/sources/ResourceDemo.move"
          --source-map "${resource_map}"
  RESULT_VARIABLE resource_decompile_status
  OUTPUT_VARIABLE resource_decompile_output
  ERROR_VARIABLE resource_decompile_error
)
if(NOT resource_decompile_status EQUAL 0)
  message(FATAL_ERROR
    "movescape failed to decompile resource module:\n${resource_decompile_output}${resource_decompile_error}")
endif()
execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" decompile "${bool_module}"
          "${TEST_ROOT}/decompiled/sources/BoolDemo.move"
          --source-map "${bool_map}"
  RESULT_VARIABLE bool_decompile_status
  OUTPUT_VARIABLE bool_decompile_output
  ERROR_VARIABLE bool_decompile_error
)
if(NOT bool_decompile_status EQUAL 0)
  message(FATAL_ERROR
    "movescape failed to decompile Boolean module:\n${bool_decompile_output}${bool_decompile_error}")
endif()
execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" decompile "${number_module}"
          "${TEST_ROOT}/decompiled/sources/NumberDemo.move"
          --source-map "${number_map}"
  RESULT_VARIABLE number_decompile_status
  OUTPUT_VARIABLE number_decompile_output
  ERROR_VARIABLE number_decompile_error
)
if(NOT number_decompile_status EQUAL 0)
  message(FATAL_ERROR
    "movescape failed to decompile numeric module:\n${number_decompile_output}${number_decompile_error}")
endif()

execute_process(
  COMMAND "${APTOS_EXECUTABLE}" move compile --package-dir "${TEST_ROOT}/decompiled"
  RESULT_VARIABLE decompiled_status
  OUTPUT_VARIABLE decompiled_output
  ERROR_VARIABLE decompiled_error
)
if(NOT decompiled_status EQUAL 0)
  message(FATAL_ERROR
    "official compiler rejected decompiled demo:\n${decompiled_output}${decompiled_error}")
endif()

file(GLOB_RECURSE decompiled_modules
  "${TEST_ROOT}/decompiled/build/*/bytecode_modules/RoundTripDemo.mv")
list(LENGTH decompiled_modules decompiled_count)
if(NOT decompiled_count EQUAL 1)
  message(FATAL_ERROR "decompiled package did not emit exactly one module")
endif()
list(GET decompiled_modules 0 decompiled_module)
file(GLOB_RECURSE decompiled_resource_modules
  "${TEST_ROOT}/decompiled/build/*/bytecode_modules/ResourceDemo.mv")
list(LENGTH decompiled_resource_modules decompiled_resource_count)
if(NOT decompiled_resource_count EQUAL 1)
  message(FATAL_ERROR "decompiled package did not emit exactly one resource module")
endif()
list(GET decompiled_resource_modules 0 decompiled_resource_module)
file(GLOB_RECURSE decompiled_bool_modules
  "${TEST_ROOT}/decompiled/build/*/bytecode_modules/BoolDemo.mv")
list(LENGTH decompiled_bool_modules decompiled_bool_count)
if(NOT decompiled_bool_count EQUAL 1)
  message(FATAL_ERROR "decompiled package did not emit exactly one Boolean module")
endif()
list(GET decompiled_bool_modules 0 decompiled_bool_module)
file(GLOB_RECURSE decompiled_number_modules
  "${TEST_ROOT}/decompiled/build/*/bytecode_modules/NumberDemo.mv")
list(LENGTH decompiled_number_modules decompiled_number_count)
if(NOT decompiled_number_count EQUAL 1)
  message(FATAL_ERROR "decompiled package did not emit exactly one numeric module")
endif()
list(GET decompiled_number_modules 0 decompiled_number_module)
execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" compare-interface
          "${original_module}" "${decompiled_module}"
  RESULT_VARIABLE comparison_status
  OUTPUT_VARIABLE comparison_output
  ERROR_VARIABLE comparison_error
)
if(NOT comparison_status EQUAL 0)
  message(FATAL_ERROR
    "compiled interfaces differ:\n${comparison_output}${comparison_error}")
endif()

execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" compare-interface
          "${resource_module}" "${decompiled_resource_module}"
  RESULT_VARIABLE resource_comparison_status
  OUTPUT_VARIABLE resource_comparison_output
  ERROR_VARIABLE resource_comparison_error
)
if(NOT resource_comparison_status EQUAL 0)
  message(FATAL_ERROR
    "resource module interfaces differ:\n${resource_comparison_output}${resource_comparison_error}")
endif()

execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" compare-bodies
          "${resource_module}" "${decompiled_resource_module}"
  RESULT_VARIABLE resource_body_status
  OUTPUT_VARIABLE resource_body_output
  ERROR_VARIABLE resource_body_error
)
if(NOT resource_body_status EQUAL 0 AND NOT resource_body_status EQUAL 1)
  message(FATAL_ERROR
    "resource normalized body/CFG comparison failed to run:\n${resource_body_output}${resource_body_error}")
endif()
string(FIND "${resource_body_output}" "normalized function bodies and CFGs"
       resource_body_marker)
if(resource_body_marker EQUAL -1)
  message(FATAL_ERROR
    "resource body/CFG comparison produced no result:\n${resource_body_output}${resource_body_error}")
endif()

execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" compare-interface
          "${bool_module}" "${decompiled_bool_module}"
  RESULT_VARIABLE bool_comparison_status
  OUTPUT_VARIABLE bool_comparison_output
  ERROR_VARIABLE bool_comparison_error
)
if(NOT bool_comparison_status EQUAL 0)
  message(FATAL_ERROR
    "Boolean module interfaces differ:\n${bool_comparison_output}${bool_comparison_error}")
endif()

execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" compare-bodies
          "${bool_module}" "${decompiled_bool_module}"
  RESULT_VARIABLE bool_body_status
  OUTPUT_VARIABLE bool_body_output
  ERROR_VARIABLE bool_body_error
)
if(NOT bool_body_status EQUAL 0 AND NOT bool_body_status EQUAL 1)
  message(FATAL_ERROR
    "Boolean normalized body/CFG comparison failed to run:\n${bool_body_output}${bool_body_error}")
endif()
string(FIND "${bool_body_output}" "normalized function bodies and CFGs"
       bool_body_marker)
if(bool_body_marker EQUAL -1)
  message(FATAL_ERROR
    "Boolean body/CFG comparison produced no result:\n${bool_body_output}${bool_body_error}")
endif()

execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" compare-interface
          "${number_module}" "${decompiled_number_module}"
  RESULT_VARIABLE number_comparison_status
  OUTPUT_VARIABLE number_comparison_output
  ERROR_VARIABLE number_comparison_error
)
if(NOT number_comparison_status EQUAL 0)
  message(FATAL_ERROR
    "numeric module interfaces differ:\n${number_comparison_output}${number_comparison_error}")
endif()
execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" compare-bodies
          "${number_module}" "${decompiled_number_module}"
  RESULT_VARIABLE number_body_status
  OUTPUT_VARIABLE number_body_output
  ERROR_VARIABLE number_body_error
)
if(NOT number_body_status EQUAL 0 AND NOT number_body_status EQUAL 1)
  message(FATAL_ERROR
    "numeric normalized body/CFG comparison failed to run:\n${number_body_output}${number_body_error}")
endif()
string(FIND "${number_body_output}" "normalized function bodies and CFGs"
       number_body_marker)
if(number_body_marker EQUAL -1)
  message(FATAL_ERROR
    "numeric body/CFG comparison produced no result:\n${number_body_output}${number_body_error}")
endif()

# Generate bounded Boolean probes from the original bytecode. Each probe
# intentionally aborts with a code encoding its observed return value. Run the
# exact harness against isolated copies of both source packages and compare the
# four VM-observed codes; a function abort remains observable as its own code.
execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" generate-behavior-harness "${bool_module}"
          "${TEST_ROOT}/GeneratedBehavior.move"
  RESULT_VARIABLE generated_harness_status
  OUTPUT_VARIABLE generated_harness_output
  ERROR_VARIABLE generated_harness_error
)
if(NOT generated_harness_status EQUAL 0)
  message(FATAL_ERROR
    "movescape failed to generate Boolean probes:\n${generated_harness_output}${generated_harness_error}")
endif()
foreach(probe_package IN ITEMS generated-reference generated-candidate)
  file(MAKE_DIRECTORY "${TEST_ROOT}/${probe_package}/tests")
endforeach()
file(COPY "${TEST_ROOT}/original/Move.toml"
     DESTINATION "${TEST_ROOT}/generated-reference")
file(COPY "${TEST_ROOT}/original/sources"
     DESTINATION "${TEST_ROOT}/generated-reference")
file(COPY "${TEST_ROOT}/decompiled/Move.toml"
     DESTINATION "${TEST_ROOT}/generated-candidate")
file(COPY "${TEST_ROOT}/decompiled/sources"
     DESTINATION "${TEST_ROOT}/generated-candidate")
file(COPY "${TEST_ROOT}/GeneratedBehavior.move"
     DESTINATION "${TEST_ROOT}/generated-reference/tests")
file(COPY "${TEST_ROOT}/GeneratedBehavior.move"
     DESTINATION "${TEST_ROOT}/generated-candidate/tests")
execute_process(
  COMMAND "${APTOS_EXECUTABLE}" move test
          --package-dir "${TEST_ROOT}/generated-reference"
  RESULT_VARIABLE generated_reference_status
  OUTPUT_VARIABLE generated_reference_output
  ERROR_VARIABLE generated_reference_error
)
execute_process(
  COMMAND "${APTOS_EXECUTABLE}" move test
          --package-dir "${TEST_ROOT}/generated-candidate"
  RESULT_VARIABLE generated_candidate_status
  OUTPUT_VARIABLE generated_candidate_output
  ERROR_VARIABLE generated_candidate_error
)
if(generated_reference_status EQUAL 0 OR generated_candidate_status EQUAL 0)
  message(FATAL_ERROR "generated probes must expose outcomes through aborts")
endif()
set(generated_reference_log
    "${generated_reference_output}${generated_reference_error}")
set(generated_candidate_log
    "${generated_candidate_output}${generated_candidate_error}")
string(REGEX MATCHALL
       "aborted with code [0-9]+ originating in the module [0-9a-f]+::[A-Za-z0-9_]+"
       generated_reference_codes
       "${generated_reference_log}")
string(REGEX MATCHALL
       "aborted with code [0-9]+ originating in the module [0-9a-f]+::[A-Za-z0-9_]+"
       generated_candidate_codes
       "${generated_candidate_log}")
list(LENGTH generated_reference_codes generated_reference_code_count)
list(LENGTH generated_candidate_codes generated_candidate_code_count)
if(NOT generated_reference_code_count EQUAL 4 OR
   NOT generated_candidate_code_count EQUAL 4 OR
   NOT generated_reference_codes STREQUAL generated_candidate_codes)
  message(FATAL_ERROR
    "generated Boolean VM outcomes differ:\nreference=${generated_reference_codes}\ncandidate=${generated_candidate_codes}")
endif()

# Repeat the generated comparison for two functions over the bounded u8/address
# input domain. Eighteen input tuples times eight observed result bits produce
# 144 exact probes, including target aborts whose originating module is checked.
execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" generate-behavior-harness "${number_module}"
          "${TEST_ROOT}/GeneratedNumericBehavior.move"
  RESULT_VARIABLE numeric_harness_status
  OUTPUT_VARIABLE numeric_harness_output
  ERROR_VARIABLE numeric_harness_error
)
if(NOT numeric_harness_status EQUAL 0)
  message(FATAL_ERROR
    "movescape failed to generate numeric probes:\n${numeric_harness_output}${numeric_harness_error}")
endif()
foreach(probe_package IN ITEMS numeric-reference numeric-candidate)
  file(MAKE_DIRECTORY "${TEST_ROOT}/${probe_package}/tests")
endforeach()
file(COPY "${TEST_ROOT}/original/Move.toml"
     DESTINATION "${TEST_ROOT}/numeric-reference")
file(COPY "${TEST_ROOT}/original/sources"
     DESTINATION "${TEST_ROOT}/numeric-reference")
file(COPY "${TEST_ROOT}/decompiled/Move.toml"
     DESTINATION "${TEST_ROOT}/numeric-candidate")
file(COPY "${TEST_ROOT}/decompiled/sources"
     DESTINATION "${TEST_ROOT}/numeric-candidate")
file(COPY "${TEST_ROOT}/GeneratedNumericBehavior.move"
     DESTINATION "${TEST_ROOT}/numeric-reference/tests")
file(COPY "${TEST_ROOT}/GeneratedNumericBehavior.move"
     DESTINATION "${TEST_ROOT}/numeric-candidate/tests")
execute_process(
  COMMAND "${APTOS_EXECUTABLE}" move test
          --package-dir "${TEST_ROOT}/numeric-reference"
  RESULT_VARIABLE numeric_reference_status
  OUTPUT_VARIABLE numeric_reference_output
  ERROR_VARIABLE numeric_reference_error
)
execute_process(
  COMMAND "${APTOS_EXECUTABLE}" move test
          --package-dir "${TEST_ROOT}/numeric-candidate"
  RESULT_VARIABLE numeric_candidate_status
  OUTPUT_VARIABLE numeric_candidate_output
  ERROR_VARIABLE numeric_candidate_error
)
if(numeric_reference_status EQUAL 0 OR numeric_candidate_status EQUAL 0)
  message(FATAL_ERROR "numeric probes must expose outcomes through aborts")
endif()
set(numeric_reference_log
    "${numeric_reference_output}${numeric_reference_error}")
set(numeric_candidate_log
    "${numeric_candidate_output}${numeric_candidate_error}")
string(REGEX MATCHALL
       "aborted with code [0-9]+ originating in the module [0-9a-f]+::[A-Za-z0-9_]+"
       numeric_reference_codes
       "${numeric_reference_log}")
string(REGEX MATCHALL
       "aborted with code [0-9]+ originating in the module [0-9a-f]+::[A-Za-z0-9_]+"
       numeric_candidate_codes
       "${numeric_candidate_log}")
list(LENGTH numeric_reference_codes numeric_reference_code_count)
list(LENGTH numeric_candidate_codes numeric_candidate_code_count)
string(REGEX MATCHALL
       "aborted with code 77 originating in the module [0-9a-f]+::NumberDemo"
       numeric_reference_target_aborts "${numeric_reference_log}")
string(REGEX MATCHALL
       "aborted with code 77 originating in the module [0-9a-f]+::NumberDemo"
       numeric_candidate_target_aborts "${numeric_candidate_log}")
list(LENGTH numeric_reference_target_aborts numeric_reference_target_count)
list(LENGTH numeric_candidate_target_aborts numeric_candidate_target_count)
if(NOT numeric_reference_code_count EQUAL 144 OR
   NOT numeric_candidate_code_count EQUAL 144 OR
   NOT numeric_reference_target_count EQUAL 24 OR
   NOT numeric_candidate_target_count EQUAL 24 OR
   NOT numeric_reference_codes STREQUAL numeric_candidate_codes)
  message(FATAL_ERROR
    "generated numeric VM outcomes differ:\nreference=${numeric_reference_codes}\ncandidate=${numeric_candidate_codes}")
endif()

# Recognize the ResourceDemo lifecycle directly from verified bytecode and
# generate isolated publish/exists/read/mutate/remove scenarios. Exact u64
# observations use one abort probe per result bit, two-account cases check
# isolation, and the final probe intentionally leaves the resource in storage
# so Aptos --dump exposes a structured failure snapshot.
execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" generate-behavior-harness
          "${resource_module}" "${TEST_ROOT}/GeneratedStatefulBehavior.move"
  RESULT_VARIABLE stateful_harness_status
  OUTPUT_VARIABLE stateful_harness_output
  ERROR_VARIABLE stateful_harness_error
)
if(NOT stateful_harness_status EQUAL 0)
  message(FATAL_ERROR
    "movescape failed to generate stateful probes:\n${stateful_harness_output}${stateful_harness_error}")
endif()
string(FIND "${stateful_harness_output}" "generated-probes: 327"
       stateful_probe_marker)
string(FIND "${stateful_harness_output}" "stateful-scenarios: 1"
       stateful_scenario_marker)
if(stateful_probe_marker EQUAL -1 OR stateful_scenario_marker EQUAL -1)
  message(FATAL_ERROR
    "stateful generator did not recognize the complete lifecycle:\n${stateful_harness_output}")
endif()
foreach(probe_package IN ITEMS stateful-reference stateful-candidate)
  file(MAKE_DIRECTORY "${TEST_ROOT}/${probe_package}/tests")
endforeach()
file(COPY "${TEST_ROOT}/original/Move.toml"
     DESTINATION "${TEST_ROOT}/stateful-reference")
file(COPY "${TEST_ROOT}/original/sources"
     DESTINATION "${TEST_ROOT}/stateful-reference")
file(COPY "${TEST_ROOT}/decompiled/Move.toml"
     DESTINATION "${TEST_ROOT}/stateful-candidate")
file(COPY "${TEST_ROOT}/decompiled/sources"
     DESTINATION "${TEST_ROOT}/stateful-candidate")
file(COPY "${TEST_ROOT}/GeneratedStatefulBehavior.move"
     DESTINATION "${TEST_ROOT}/stateful-reference/tests")
file(COPY "${TEST_ROOT}/GeneratedStatefulBehavior.move"
     DESTINATION "${TEST_ROOT}/stateful-candidate/tests")
execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" compare-behavior-outcomes
          "${TEST_ROOT}/stateful-reference"
          "${TEST_ROOT}/stateful-candidate"
          "${TEST_ROOT}/stateful-results" "${APTOS_EXECUTABLE}"
  RESULT_VARIABLE stateful_comparison_status
  OUTPUT_VARIABLE stateful_comparison_output
  ERROR_VARIABLE stateful_comparison_error
)
if(NOT stateful_comparison_status EQUAL 0)
  message(FATAL_ERROR
    "generated stateful VM outcomes differ:\n${stateful_comparison_output}${stateful_comparison_error}")
endif()
string(FIND "${stateful_comparison_output}"
       "all-observed-outcomes-equivalent: yes" stateful_equivalent_marker)
string(FIND "${stateful_comparison_output}" "observed-cases: 327"
       stateful_case_marker)
string(FIND "${stateful_comparison_output}" "abort-observations: 327"
       stateful_abort_marker)
string(FIND "${stateful_comparison_output}" "storage-snapshots: 327"
       stateful_storage_marker)
if(stateful_equivalent_marker EQUAL -1 OR stateful_case_marker EQUAL -1 OR
   stateful_abort_marker EQUAL -1 OR stateful_storage_marker EQUAL -1)
  message(FATAL_ERROR
    "stateful structured trace is incomplete:\n${stateful_comparison_output}")
endif()

execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" compare-bodies
          "${original_module}" "${decompiled_module}"
  RESULT_VARIABLE body_status
  OUTPUT_VARIABLE body_output
  ERROR_VARIABLE body_error
)
if(NOT body_status EQUAL 0 AND NOT body_status EQUAL 1)
  message(FATAL_ERROR
    "normalized body/CFG comparison failed to run:\n${body_output}${body_error}")
endif()
string(FIND "${body_output}" "normalized function bodies and CFGs" body_marker)
if(body_marker EQUAL -1)
  message(FATAL_ERROR
    "normalized body/CFG comparison produced no result:\n${body_output}${body_error}")
endif()

execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" compare-behavior
          "${TEST_ROOT}/original" "${TEST_ROOT}/decompiled"
          "${TEST_ROOT}/behavior-results" "${APTOS_EXECUTABLE}"
  RESULT_VARIABLE behavior_status
  OUTPUT_VARIABLE behavior_output
  ERROR_VARIABLE behavior_error
)
if(NOT behavior_status EQUAL 0)
  message(FATAL_ERROR
    "original and decompiled modules failed shared behavioral tests:\n${behavior_output}${behavior_error}")
endif()

execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" round-trip-package
          "${TEST_ROOT}/original/build/MovescapeRoundTripDemo/bytecode_modules"
          "${TEST_ROOT}/multi-module-round-trip" "${APTOS_EXECUTABLE}"
  RESULT_VARIABLE multi_status
  OUTPUT_VARIABLE multi_output
  ERROR_VARIABLE multi_error
)
if(NOT multi_status EQUAL 0)
  message(FATAL_ERROR
    "dependency-aware multi-module round trip failed:\n${multi_output}${multi_error}")
endif()

# Compile a package with a real local Move.toml dependency, then hand the
# package root (not its build directory) to Movescape. The dependency package is
# intentionally not compiled separately: Aptos places its artifact under the
# primary package build, while manifest traversal proves the local graph and
# resolves the complete bytecode closure without network access.
file(MAKE_DIRECTORY "${TEST_ROOT}/manifest-dependency/sources")
file(MAKE_DIRECTORY "${TEST_ROOT}/manifest-consumer/sources")
file(WRITE "${TEST_ROOT}/manifest-dependency/Move.toml"
  "[package]\nname = \"ManifestDependency\"\nversion = \"0.0.0\"\n")
file(WRITE "${TEST_ROOT}/manifest-dependency/sources/ManifestDependency.move"
  "module 0x77::ManifestDependency {\n"
  "    public fun increment(value: u64): u64 { value + 1 }\n"
  "}\n")
file(WRITE "${TEST_ROOT}/manifest-consumer/Move.toml"
  "[package]\nname = \"ManifestConsumer\"\nversion = \"0.0.0\"\n"
  "[dependencies]\n"
  "ManifestDependency = { local = \"../manifest-dependency\" }\n")
file(WRITE "${TEST_ROOT}/manifest-consumer/sources/ManifestConsumer.move"
  "module 0x78::ManifestConsumer {\n"
  "    use 0x77::ManifestDependency;\n"
  "    public fun call(value: u64): u64 {\n"
  "        ManifestDependency::increment(value)\n"
  "    }\n"
  "}\n")
execute_process(
  COMMAND "${APTOS_EXECUTABLE}" move compile
          --package-dir "${TEST_ROOT}/manifest-consumer"
  RESULT_VARIABLE manifest_compile_status
  OUTPUT_VARIABLE manifest_compile_output
  ERROR_VARIABLE manifest_compile_error
)
if(NOT manifest_compile_status EQUAL 0)
  message(FATAL_ERROR
    "official compiler rejected local manifest dependency fixture:\n${manifest_compile_output}${manifest_compile_error}")
endif()
execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" round-trip-package
          "${TEST_ROOT}/manifest-consumer"
          "${TEST_ROOT}/manifest-round-trip" "${APTOS_EXECUTABLE}"
  RESULT_VARIABLE manifest_round_trip_status
  OUTPUT_VARIABLE manifest_round_trip_output
  ERROR_VARIABLE manifest_round_trip_error
)
if(NOT manifest_round_trip_status EQUAL 0)
  message(FATAL_ERROR
    "manifest-aware dependency round trip failed:\n${manifest_round_trip_output}${manifest_round_trip_error}")
endif()
if(NOT manifest_round_trip_output MATCHES "modules: 2" OR
   NOT manifest_round_trip_output MATCHES "minimum-bytecode-version: 10" OR
   NOT manifest_round_trip_output MATCHES "minimum-language-version: 2.2" OR
   NOT manifest_round_trip_output MATCHES "unresolved-external-modules: 0" OR
   NOT manifest_round_trip_output MATCHES "all-interfaces-equivalent: yes")
  message(FATAL_ERROR
    "manifest-aware dependency round trip did not prove both modules:\n${manifest_round_trip_output}")
endif()

# A framework-style package may own native declarations that must remain in the
# authoritative dependency package. Its bytecode is still used to verify the
# recovered consumer ABI, but Movescape emits only the consumer and retains the
# native package as a local compiler dependency.
file(MAKE_DIRECTORY "${TEST_ROOT}/native-dependency/sources")
file(MAKE_DIRECTORY "${TEST_ROOT}/native-consumer/sources")
file(WRITE "${TEST_ROOT}/native-dependency/Move.toml"
  "[package]\nname = \"NativeFramework\"\nversion = \"0.0.0\"\n")
file(WRITE "${TEST_ROOT}/native-dependency/sources/NativeFramework.move"
  "module 0x1::NativeFramework {\n"
  "    native public fun increment(value: u64): u64;\n"
  "}\n")
file(WRITE "${TEST_ROOT}/native-consumer/Move.toml"
  "[package]\nname = \"NativeConsumer\"\nversion = \"0.0.0\"\n"
  "[dependencies]\n"
  "NativeFramework = { local = \"../native-dependency\" }\n")
file(WRITE "${TEST_ROOT}/native-consumer/sources/NativeConsumer.move"
  "module 0x78::NativeConsumer {\n"
  "    use 0x1::NativeFramework;\n"
  "    public fun call(value: u64): u64 {\n"
  "        NativeFramework::increment(value)\n"
  "    }\n"
  "}\n")
foreach(native_package IN ITEMS native-dependency native-consumer)
  execute_process(
    COMMAND "${APTOS_EXECUTABLE}" move compile
            --package-dir "${TEST_ROOT}/${native_package}"
    RESULT_VARIABLE native_compile_status
    OUTPUT_VARIABLE native_compile_output
    ERROR_VARIABLE native_compile_error
  )
  if(NOT native_compile_status EQUAL 0)
    message(FATAL_ERROR
      "official compiler rejected ${native_package}:\n${native_compile_output}${native_compile_error}")
  endif()
endforeach()
execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" round-trip-package
          "${TEST_ROOT}/native-consumer"
          "${TEST_ROOT}/native-round-trip"
          --external-package "${TEST_ROOT}/native-dependency"
          "${APTOS_EXECUTABLE}"
  RESULT_VARIABLE native_round_trip_status
  OUTPUT_VARIABLE native_round_trip_output
  ERROR_VARIABLE native_round_trip_error
)
if(NOT native_round_trip_status EQUAL 0)
  message(FATAL_ERROR
    "external native dependency round trip failed:\n${native_round_trip_output}${native_round_trip_error}")
endif()
if(NOT native_round_trip_output MATCHES "modules: 1" OR
   NOT native_round_trip_output MATCHES "external-compiler-packages: 1" OR
   NOT native_round_trip_output MATCHES "unresolved-external-modules: 0" OR
   NOT native_round_trip_output MATCHES "all-interfaces-equivalent: yes")
  message(FATAL_ERROR
    "external native dependency was not preserved correctly:\n${native_round_trip_output}")
endif()

# Text-like vector<u8> constants should recover as readable, reversible byte
# strings, while binary data remains hexadecimal. Recompile the recovered
# source and compare both its declaration interface and normalized body.
file(MAKE_DIRECTORY "${TEST_ROOT}/string-original/sources")
file(MAKE_DIRECTORY "${TEST_ROOT}/string-candidate/sources")
foreach(string_package IN ITEMS string-original string-candidate)
  file(WRITE "${TEST_ROOT}/${string_package}/Move.toml"
    "[package]\nname = \"StringRoundTrip\"\nversion = \"0.0.0\"\n")
endforeach()
file(WRITE "${TEST_ROOT}/string-original/sources/StringConstants.move"
  "module 0x42::StringConstants {\n"
  "    const TEXT: vector<u8> = b\"hello \\\"movescape\\\"\\\\path\\n\";\n"
  "    const UTF8: vector<u8> = b\"zdravo \\xc5\\xbeivote \\xf0\\x9f\\x8c\\x8d\";\n"
  "    const BINARY: vector<u8> = x\"ff0001\";\n"
  "    public fun text(): vector<u8> { TEXT }\n"
  "    public fun utf8(): vector<u8> { UTF8 }\n"
  "    public fun binary(): vector<u8> { BINARY }\n"
  "}\n")
execute_process(
  COMMAND "${APTOS_EXECUTABLE}" move compile
          --package-dir "${TEST_ROOT}/string-original"
          --bytecode-version 10 --language-version 2.4
  RESULT_VARIABLE string_original_status
  OUTPUT_VARIABLE string_original_output
  ERROR_VARIABLE string_original_error
)
if(NOT string_original_status EQUAL 0)
  message(FATAL_ERROR
    "official compiler rejected string constant fixture:\n${string_original_output}${string_original_error}")
endif()
file(GLOB_RECURSE string_original_modules
  "${TEST_ROOT}/string-original/build/*/bytecode_modules/StringConstants.mv")
file(GLOB_RECURSE string_original_maps
  "${TEST_ROOT}/string-original/build/*/source_maps/StringConstants.mvsm")
list(LENGTH string_original_modules string_module_count)
list(LENGTH string_original_maps string_map_count)
if(NOT string_module_count EQUAL 1 OR NOT string_map_count EQUAL 1)
  message(FATAL_ERROR "string fixture did not emit one module and source map")
endif()
list(GET string_original_modules 0 string_original_module)
list(GET string_original_maps 0 string_original_map)
execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" decompile "${string_original_module}"
          "${TEST_ROOT}/string-candidate/sources/StringConstants.move"
          --source-map "${string_original_map}"
  RESULT_VARIABLE string_decompile_status
  OUTPUT_VARIABLE string_decompile_output
  ERROR_VARIABLE string_decompile_error
)
if(NOT string_decompile_status EQUAL 0)
  message(FATAL_ERROR
    "string constant decompilation failed:\n${string_decompile_output}${string_decompile_error}")
endif()
file(STRINGS "${TEST_ROOT}/string-candidate/sources/StringConstants.move"
  recovered_text_lines REGEX "return b\"")
file(STRINGS "${TEST_ROOT}/string-candidate/sources/StringConstants.move"
  recovered_binary_lines REGEX "return x\"")
list(LENGTH recovered_text_lines recovered_text_count)
list(LENGTH recovered_binary_lines recovered_binary_count)
if(NOT recovered_text_count EQUAL 2 OR NOT recovered_binary_count EQUAL 1)
  message(FATAL_ERROR
    "string constants did not select readable/exact literals correctly")
endif()
execute_process(
  COMMAND "${APTOS_EXECUTABLE}" move compile
          --package-dir "${TEST_ROOT}/string-candidate"
          --bytecode-version 10 --language-version 2.4
  RESULT_VARIABLE string_candidate_status
  OUTPUT_VARIABLE string_candidate_output
  ERROR_VARIABLE string_candidate_error
)
if(NOT string_candidate_status EQUAL 0)
  message(FATAL_ERROR
    "official compiler rejected recovered string constants:\n${string_candidate_output}${string_candidate_error}")
endif()
file(GLOB_RECURSE string_candidate_modules
  "${TEST_ROOT}/string-candidate/build/*/bytecode_modules/StringConstants.mv")
list(LENGTH string_candidate_modules string_candidate_count)
if(NOT string_candidate_count EQUAL 1)
  message(FATAL_ERROR "recovered string fixture did not emit one module")
endif()
list(GET string_candidate_modules 0 string_candidate_module)
execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" compare-interface
          "${string_original_module}" "${string_candidate_module}"
  RESULT_VARIABLE string_interface_status
  OUTPUT_VARIABLE string_interface_output
  ERROR_VARIABLE string_interface_error
)
execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" compare-bodies
          "${string_original_module}" "${string_candidate_module}"
  RESULT_VARIABLE string_body_status
  OUTPUT_VARIABLE string_body_output
  ERROR_VARIABLE string_body_error
)
if(NOT string_interface_status EQUAL 0 OR NOT string_body_status EQUAL 0)
  message(FATAL_ERROR
    "string constant round trip changed semantics:\n${string_interface_output}${string_interface_error}${string_body_output}${string_body_error}")
endif()

# Compile a transaction script, decompile it through the script footer path,
# recompile it, and require byte-for-byte equality.
foreach(script_package IN ITEMS script-original script-candidate)
  file(MAKE_DIRECTORY "${TEST_ROOT}/${script_package}/sources")
  file(MAKE_DIRECTORY "${TEST_ROOT}/${script_package}/scripts")
endforeach()
file(WRITE "${TEST_ROOT}/script-original/Move.toml"
  "[package]\nname = \"MovescapeScriptRoundTrip\"\nversion = \"0.0.0\"\n")
file(COPY "${TEST_ROOT}/script-original/Move.toml"
     DESTINATION "${TEST_ROOT}/script-candidate")
file(WRITE "${TEST_ROOT}/script-original/scripts/main.move"
  "script {\n  fun main(value: u64) {\n    assert!(value < 1000, 7);\n  }\n}\n")
execute_process(
  COMMAND "${APTOS_EXECUTABLE}" move compile
          --package-dir "${TEST_ROOT}/script-original"
          --bytecode-version 10 --language-version 2.2
          --skip-fetch-latest-git-deps
  RESULT_VARIABLE script_original_status
  OUTPUT_VARIABLE script_original_output
  ERROR_VARIABLE script_original_error
)
if(NOT script_original_status EQUAL 0)
  message(FATAL_ERROR
    "official compiler failed script fixture:\n${script_original_output}${script_original_error}")
endif()
set(script_original_bytecode
    "${TEST_ROOT}/script-original/build/MovescapeScriptRoundTrip/bytecode_scripts/main.mv")
execute_process(
  COMMAND "${MOVESCAPE_EXECUTABLE}" decompile
          "${script_original_bytecode}"
          "${TEST_ROOT}/script-candidate/scripts/main.move" --script
  RESULT_VARIABLE script_decompile_status
  OUTPUT_VARIABLE script_decompile_output
  ERROR_VARIABLE script_decompile_error
)
if(NOT script_decompile_status EQUAL 0)
  message(FATAL_ERROR
    "script decompilation failed:\n${script_decompile_output}${script_decompile_error}")
endif()
execute_process(
  COMMAND "${APTOS_EXECUTABLE}" move compile
          --package-dir "${TEST_ROOT}/script-candidate"
          --bytecode-version 10 --language-version 2.2
          --skip-fetch-latest-git-deps
  RESULT_VARIABLE script_candidate_status
  OUTPUT_VARIABLE script_candidate_output
  ERROR_VARIABLE script_candidate_error
)
if(NOT script_candidate_status EQUAL 0)
  message(FATAL_ERROR
    "official compiler rejected decompiled script:\n${script_candidate_output}${script_candidate_error}")
endif()
set(script_candidate_bytecode
    "${TEST_ROOT}/script-candidate/build/MovescapeScriptRoundTrip/bytecode_scripts/main.mv")
file(SHA256 "${script_original_bytecode}" script_original_hash)
file(SHA256 "${script_candidate_bytecode}" script_candidate_hash)
if(NOT script_original_hash STREQUAL script_candidate_hash)
  message(FATAL_ERROR
    "script bytecode changed after round trip: ${script_original_hash} != ${script_candidate_hash}")
endif()

message(STATUS "real compiler accepted original and decompiled modules")
message(STATUS "official compiler accepted the typed irreducible dispatcher")
message(STATUS "${comparison_output}")
message(STATUS "${body_output}")
message(STATUS "${resource_comparison_output}")
message(STATUS "${resource_body_output}")
message(STATUS "${bool_comparison_output}")
message(STATUS "${bool_body_output}")
message(STATUS "generated Boolean VM outcomes: ${generated_reference_codes}")
message(STATUS "${number_comparison_output}")
message(STATUS "${number_body_output}")
message(STATUS "generated numeric VM outcomes matched: ${numeric_reference_code_count}")
message(STATUS "${behavior_output}")
message(STATUS "${multi_output}")
message(STATUS "${manifest_round_trip_output}")
message(STATUS "${native_round_trip_output}")
message(STATUS "string constants preserved interface and normalized bodies")
message(STATUS "script round trip is byte-identical: ${script_original_hash}")
