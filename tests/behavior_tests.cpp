#include "test.hpp"

#include "movescape/behavior.hpp"

namespace {

movescape::Module resourceLifecycleModule() {
  movescape::Module module;
  module.identifiers = {"ResourceDemo", "Counter", "publish", "contains", "value", "increment", "remove"};
  module.addresses.push_back({});
  module.addresses[0].back() = 0x42;
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  module.struct_handles.push_back({
      .module = 0,
      .name = 1,
      .abilities = movescape::AbilitySet{movescape::AbilitySet::Key},
  });
  module.struct_definitions.push_back({
      .handle = 0,
      .field_kind = movescape::StructFieldKind::Native,
  });

  const movescape::Type signer{.kind = movescape::TypeKind::Signer};
  movescape::Type signer_reference{.kind = movescape::TypeKind::Reference};
  signer_reference.arguments.push_back(signer);
  const movescape::Type address{.kind = movescape::TypeKind::Address};
  const movescape::Type boolean{.kind = movescape::TypeKind::Bool};
  const movescape::Type u64{.kind = movescape::TypeKind::U64};
  module.signatures = {{}, {signer_reference, u64}, {address}, {boolean}, {u64}};
  module.function_handles = {
      {.module = 0, .name = 2, .parameters = 1, .returns = 0}, {.module = 0, .name = 3, .parameters = 2, .returns = 3},
      {.module = 0, .name = 4, .parameters = 2, .returns = 4}, {.module = 0, .name = 5, .parameters = 2, .returns = 0},
      {.module = 0, .name = 6, .parameters = 2, .returns = 4},
  };
  const auto definition = [](movescape::TableIndex handle, movescape::Opcode operation) {
    return movescape::FunctionDefinition{
        .handle = handle,
        .visibility = movescape::Visibility::Public,
        .code =
            movescape::CodeUnit{
                .locals = 0,
                .code = {{.opcode = operation, .operands = {0}}, {.opcode = movescape::Opcode::Ret}},
            },
    };
  };
  module.function_definitions = {
      definition(0, movescape::Opcode::MoveTo),          definition(1, movescape::Opcode::Exists),   definition(2, movescape::Opcode::ImmBorrowGlobal),
      definition(3, movescape::Opcode::MutBorrowGlobal), definition(4, movescape::Opcode::MoveFrom),
  };
  module.function_definitions[2].acquires = {0};
  module.function_definitions[3].acquires = {0};
  module.function_definitions[4].acquires = {0};
  return module;
}

} // namespace

TEST(generates_bounded_boolean_behavior_probes_deterministically) {
  movescape::Module module;
  module.identifiers = {"ProbeTarget", "xor", "private_bool"};
  module.addresses.push_back({});
  module.addresses[0].back() = 0x42;
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  const movescape::Type boolean{.kind = movescape::TypeKind::Bool};
  module.signatures = {{}, {boolean, boolean}, {boolean}};
  module.function_handles = {
      {.module = 0, .name = 1, .parameters = 1, .returns = 2},
      {.module = 0, .name = 2, .parameters = 1, .returns = 2},
  };
  module.function_definitions = {
      {.handle = 0, .visibility = movescape::Visibility::Public, .code = movescape::CodeUnit{}},
      {.handle = 1, .visibility = movescape::Visibility::Private, .code = movescape::CodeUnit{}},
  };

  const auto harness = movescape::generateBehaviorHarness(module);
  REQUIRE_EQ(harness.probes.size(), 4U);
  REQUIRE_EQ(harness.skipped_functions.size(), 1U);
  REQUIRE_EQ(harness.probes[0].argument_literals, (std::vector<std::string>{"false", "false"}));
  REQUIRE_EQ(harness.probes[1].argument_literals, (std::vector<std::string>{"true", "false"}));
  REQUIRE_EQ(harness.probes[2].argument_literals, (std::vector<std::string>{"false", "true"}));
  REQUIRE_EQ(harness.probes[3].argument_literals, (std::vector<std::string>{"true", "true"}));
  REQUIRE(harness.source.find("0x42::ProbeTarget::xor(false, false)") != std::string::npos);
  REQUIRE(harness.source.find("0x42::ProbeTarget::xor(true, true)") != std::string::npos);
  REQUIRE(harness.source.find("private_bool") == std::string::npos);
  REQUIRE(harness.probes[0].true_abort_code == harness.probes[0].false_abort_code + 1U);
}

TEST(boolean_behavior_generation_enforces_case_limits) {
  movescape::Module module;
  module.identifiers = {"ProbeTarget", "many"};
  module.addresses.push_back({});
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  const movescape::Type boolean{.kind = movescape::TypeKind::Bool};
  module.signatures = {{}, {boolean, boolean, boolean}, {boolean}};
  module.function_handles = {
      {.module = 0, .name = 1, .parameters = 1, .returns = 2},
  };
  module.function_definitions = {
      {.handle = 0, .visibility = movescape::Visibility::Public, .code = movescape::CodeUnit{}},
  };

  const auto harness = movescape::generateBehaviorHarness(module, {.max_parameters = 3, .max_input_cases_per_function = 4, .max_probes = 64});
  REQUIRE(harness.probes.empty());
  REQUIRE_EQ(harness.skipped_functions.size(), 1U);
  REQUIRE(harness.skipped_functions[0].find("unsupported domain") != std::string::npos);
}

TEST(generates_numeric_and_address_inputs_with_exact_unsigned_bit_probes) {
  movescape::Module module;
  module.identifiers = {"ProbeTarget", "measure"};
  module.addresses.push_back({});
  module.addresses[0].back() = 0x42;
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  const movescape::Type u8{.kind = movescape::TypeKind::U8};
  const movescape::Type address{.kind = movescape::TypeKind::Address};
  module.signatures = {{}, {u8, address}, {u8}};
  module.function_handles = {
      {.module = 0, .name = 1, .parameters = 1, .returns = 2},
  };
  module.function_definitions = {
      {.handle = 0, .visibility = movescape::Visibility::Public, .code = movescape::CodeUnit{}},
  };

  const auto harness = movescape::generateBehaviorHarness(module);
  REQUIRE_EQ(harness.probes.size(), 72U);
  REQUIRE(harness.skipped_functions.empty());
  REQUIRE_EQ(harness.probes[0].argument_literals, (std::vector<std::string>{"0u8", "@0x0"}));
  REQUIRE_EQ(harness.probes[0].result_bit, std::optional<std::size_t>{0});
  REQUIRE_EQ(harness.probes[7].result_bit, std::optional<std::size_t>{7});
  REQUIRE_EQ(harness.probes[8].argument_literals, (std::vector<std::string>{"1u8", "@0x0"}));
  REQUIRE(harness.source.find("ProbeTarget::measure(255u8, @0x42)") != std::string::npos);
  REQUIRE(harness.source.find("& 128u8) != 0u8") != std::string::npos);
}

TEST(unsigned_u64_probe_covers_the_high_bit_without_shift_overflow) {
  movescape::Module module;
  module.identifiers = {"ProbeTarget", "identity"};
  module.addresses.push_back({});
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  const movescape::Type u64{.kind = movescape::TypeKind::U64};
  module.signatures = {{}, {u64}, {u64}};
  module.function_handles = {
      {.module = 0, .name = 1, .parameters = 1, .returns = 2},
  };
  module.function_definitions = {
      {.handle = 0, .visibility = movescape::Visibility::Public, .code = movescape::CodeUnit{}},
  };

  const auto harness = movescape::generateBehaviorHarness(module);
  REQUIRE_EQ(harness.probes.size(), 192U);
  REQUIRE_EQ(harness.probes[63].result_bit, std::optional<std::size_t>{63});
  REQUIRE(harness.source.find("& 9223372036854775808u64) != 0u64") != std::string::npos);
}

TEST(generates_stateful_resource_lifecycle_and_storage_probes) {
  const auto harness = movescape::generateBehaviorHarness(resourceLifecycleModule());
  REQUIRE_EQ(harness.stateful_scenarios.size(), 1U);
  REQUIRE_EQ(harness.stateful_scenarios[0].probe_count, 324U);
  REQUIRE_EQ(harness.probes.size(), 327U);
  REQUIRE(harness.skipped_stateful_resources.empty());
  REQUIRE(harness.source.find("#[test(account = @0xcafe)]\n  fun "
                              "stateful_0_value_after_mutation_bit_0(account: signer)") != std::string::npos);
  REQUIRE(harness.source.find("0x42::ResourceDemo::publish(&account, 41u64);") != std::string::npos);
  REQUIRE(harness.source.find("0x42::ResourceDemo::increment(@0xcafe);") != std::string::npos);
  REQUIRE(harness.source.find("stateful_0_isolated_bob_bit_63") != std::string::npos);
  REQUIRE(harness.source.find("stateful_0_storage_after_mutation") != std::string::npos);
  REQUIRE(harness.source.find("abort 557076582208058") != std::string::npos);
}

TEST(stateful_generation_fails_closed_on_incomplete_lifecycles_and_limits) {
  auto incomplete = resourceLifecycleModule();
  incomplete.function_definitions.erase(incomplete.function_definitions.begin() + 3);
  const auto missing_mutator = movescape::generateBehaviorHarness(incomplete);
  REQUIRE(missing_mutator.stateful_scenarios.empty());
  REQUIRE_EQ(missing_mutator.skipped_stateful_resources.size(), 1U);

  const auto limited = movescape::generateBehaviorHarness(
      resourceLifecycleModule(), {.max_parameters = 4, .max_input_cases_per_function = 64, .max_probes = 100, .max_stateful_scenarios = 8});
  REQUIRE(limited.stateful_scenarios.empty());
  REQUIRE_EQ(limited.probes.size(), 3U);
  REQUIRE_EQ(limited.skipped_stateful_resources.size(), 1U);
  REQUIRE(limited.skipped_stateful_resources[0].find("probe limit") != std::string::npos);
}
