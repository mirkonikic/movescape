#include "test.hpp"

#include "movescape/cfg.hpp"
#include "movescape/local_analysis.hpp"

namespace {

movescape::Module localModule(std::vector<movescape::Instruction> code, bool parameter = false) {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"M", "f"};
  module.addresses.push_back({});
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  module.signatures.push_back({});
  module.signatures.push_back({movescape::Type{.kind = movescape::TypeKind::U64}});
  module.function_handles.push_back({
      .module = 0,
      .name = 1,
      .parameters = parameter ? movescape::TableIndex{1} : movescape::TableIndex{0},
      .returns = 0,
  });
  movescape::CodeUnit unit;
  unit.locals = parameter ? 0 : 1;
  unit.code = std::move(code);
  module.function_definitions.push_back({
      .handle = 0,
      .code = unit,
  });
  return module;
}

} // namespace

TEST(reaching_definitions_merge_at_a_diamond) {
  const auto module = localModule({
      {.opcode = movescape::Opcode::LdTrue},
      {.opcode = movescape::Opcode::BrFalse, .operands = {5}},
      {.opcode = movescape::Opcode::LdU64, .operands = {1}},
      {.opcode = movescape::Opcode::StLoc, .operands = {0}},
      {.opcode = movescape::Opcode::Branch, .operands = {7}},
      {.opcode = movescape::Opcode::LdU64, .operands = {2}},
      {.opcode = movescape::Opcode::StLoc, .operands = {0}},
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::Pop},
      {.opcode = movescape::Opcode::Ret},
  });
  const auto &function = module.function_definitions[0];
  const auto cfg = movescape::buildControlFlowGraph(*function.code);
  const auto analysis = movescape::analyzeLocals(module, function, cfg);
  REQUIRE_EQ(analysis.definitions.size(), 2U);
  REQUIRE_EQ(analysis.uses.size(), 1U);
  REQUIRE_EQ(analysis.uses[0].reaching_definitions, (std::vector<movescape::DefinitionId>{0, 1}));
  REQUIRE_EQ(analysis.availability_in[3][0], movescape::Availability::Available);
}

TEST(local_analysis_rejects_use_after_move) {
  const auto module = localModule(
      {
          {.opcode = movescape::Opcode::MoveLoc, .operands = {0}},
          {.opcode = movescape::Opcode::Pop},
          {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
          {.opcode = movescape::Opcode::Pop},
          {.opcode = movescape::Opcode::Ret},
      },
      true);
  const auto &function = module.function_definitions[0];
  const auto cfg = movescape::buildControlFlowGraph(*function.code);
  REQUIRE_ERROR(movescape::analyzeLocals(module, function, cfg), movescape::ErrorCode::InvalidLocalState);
}

TEST(liveness_flows_back_to_each_definition_arm) {
  const auto module = localModule({
      {.opcode = movescape::Opcode::LdTrue},
      {.opcode = movescape::Opcode::BrFalse, .operands = {5}},
      {.opcode = movescape::Opcode::LdU64, .operands = {1}},
      {.opcode = movescape::Opcode::StLoc, .operands = {0}},
      {.opcode = movescape::Opcode::Branch, .operands = {7}},
      {.opcode = movescape::Opcode::LdU64, .operands = {2}},
      {.opcode = movescape::Opcode::StLoc, .operands = {0}},
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::Pop},
      {.opcode = movescape::Opcode::Ret},
  });
  const auto &function = module.function_definitions[0];
  const auto cfg = movescape::buildControlFlowGraph(*function.code);
  const auto analysis = movescape::analyzeLocals(module, function, cfg);
  REQUIRE(analysis.live_out[1][0]);
  REQUIRE(analysis.live_out[2][0]);
  REQUIRE(!analysis.live_in[1][0]);
  REQUIRE(!analysis.live_in[2][0]);
}
