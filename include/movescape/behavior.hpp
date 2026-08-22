#pragma once

#include "movescape/module.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace movescape {

enum class BehaviorObservationKind {
  BooleanReturn,
  UnsignedReturnBit,
  StatefulBooleanReturn,
  StatefulUnsignedReturnBit,
  StatefulStorageSnapshot,
};

struct BehaviorProbe {
  std::size_t function_definition = 0;
  std::string function_name;
  std::string test_name;
  std::vector<std::string> argument_literals;
  BehaviorObservationKind observation = BehaviorObservationKind::BooleanReturn;
  std::optional<std::size_t> result_bit;
  std::uint64_t false_abort_code = 0;
  std::uint64_t true_abort_code = 0;
};

struct StatefulBehaviorScenario {
  TableIndex resource_definition = 0;
  std::string resource_name;
  std::string publisher;
  std::string existence_checker;
  std::string reader;
  std::string mutator;
  std::string remover;
  std::size_t probe_count = 0;
};

struct GeneratedBehaviorHarness {
  std::string source;
  std::vector<BehaviorProbe> probes;
  std::vector<StatefulBehaviorScenario> stateful_scenarios;
  std::vector<std::string> skipped_functions;
  std::vector<std::string> skipped_stateful_resources;
};

struct BehaviorGenerationOptions {
  std::size_t max_parameters = 4;
  std::size_t max_input_cases_per_function = 64;
  std::size_t max_probes = 4096;
  std::size_t max_stateful_scenarios = 8;
};

// Generates deterministic Move unit-test probes for public, non-generic,
// non-native functions over bounded bool, unsigned-integer, and address input
// domains. A sole bool return receives one observation per input; u8/u16/u32/
// u64 returns receive one observation per result bit. It also recognizes a
// conservative resource lifecycle consisting of public move_to, exists,
// immutable/mutable global borrow, and move_from functions over one key type.
// Stateful probes recreate isolated accounts, observe exact result bits,
// exercise account isolation, and retain one failure storage snapshot. Every
// probe intentionally aborts with a distinct code, while an earlier function
// abort remains directly observable to the comparison runner.
[[nodiscard]] GeneratedBehaviorHarness generateBehaviorHarness(const Module &module, const BehaviorGenerationOptions &options = {});

} // namespace movescape
