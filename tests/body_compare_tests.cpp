#include "test.hpp"

#include "movescape/body_compare.hpp"

#include <string>

namespace {

movescape::Module branchingModule(bool reversed_layout) {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"M", "choose"};
  module.addresses.push_back({});
  module.addresses[0].back() = 1;
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  const movescape::Type boolean{.kind = movescape::TypeKind::Bool};
  const movescape::Type u64{.kind = movescape::TypeKind::U64};
  module.signatures = {{}, {boolean}, {u64}};
  module.function_handles.push_back({
      .module = 0,
      .name = 1,
      .parameters = 1,
      .returns = 2,
  });
  movescape::CodeUnit unit;
  unit.locals = 0;
  if (reversed_layout) {
    unit.code = {
        {.opcode = movescape::Opcode::CopyLoc, .operands = {0}}, {.opcode = movescape::Opcode::BrTrue, .operands = {4}},
        {.opcode = movescape::Opcode::LdU64, .operands = {2}},   {.opcode = movescape::Opcode::Ret},
        {.opcode = movescape::Opcode::LdU64, .operands = {1}},   {.opcode = movescape::Opcode::Ret},
    };
  } else {
    unit.code = {
        {.opcode = movescape::Opcode::CopyLoc, .operands = {0}}, {.opcode = movescape::Opcode::BrFalse, .operands = {4}},
        {.opcode = movescape::Opcode::LdU64, .operands = {1}},   {.opcode = movescape::Opcode::Ret},
        {.opcode = movescape::Opcode::LdU64, .operands = {2}},   {.opcode = movescape::Opcode::Ret},
    };
  }
  module.function_definitions.push_back({
      .handle = 0,
      .visibility = movescape::Visibility::Public,
      .code = unit,
  });
  return module;
}

} // namespace

TEST(normalized_bodies_ignore_block_layout_and_branch_polarity_encoding) {
  const auto reference = branchingModule(false);
  const auto candidate = branchingModule(true);

  const auto normalized_reference = movescape::normalizeModuleBodies(reference);
  const auto normalized_candidate = movescape::normalizeModuleBodies(candidate);
  REQUIRE_EQ(normalized_reference, normalized_candidate);
  REQUIRE(movescape::compareModuleBodies(reference, candidate).equivalent());

  const auto formatted = movescape::formatNormalizedModuleBodies(normalized_reference);
  REQUIRE(formatted.find("CopyLoc arg#0:bool") != std::string::npos);
  REQUIRE(formatted.find("true->bb1") != std::string::npos);
  REQUIRE(formatted.find("false->bb2") != std::string::npos);
}

TEST(normalized_body_comparison_reports_a_concrete_opcode_difference) {
  const auto reference = branchingModule(false);
  auto candidate = branchingModule(true);
  candidate.function_definitions[0].code->code[4].operands[0] = 3;

  const auto comparison = movescape::compareModuleBodies(reference, candidate);
  REQUIRE(!comparison.equivalent());
  REQUIRE_EQ(comparison.differences.size(), 1U);
  REQUIRE_EQ(comparison.differences[0].identity, std::string("0x1::M::choose"));
  REQUIRE(comparison.differences[0].reference->find("LdU64 1") != std::string::npos);
  REQUIRE(comparison.differences[0].candidate->find("LdU64 3") != std::string::npos);
  REQUIRE(movescape::formatModuleBodyComparison(comparison).starts_with("normalized function bodies and CFGs differ (1)\n"));
}
