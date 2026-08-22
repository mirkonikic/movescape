#include "test.hpp"

#include "movescape/cfg.hpp"
#include "movescape/stackless_ir.hpp"
#include "movescape/type_analysis.hpp"

namespace {

movescape::Module arithmeticModule() {
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
      .parameters = 0,
      .returns = 1,
  });
  movescape::CodeUnit unit;
  unit.locals = 0;
  unit.code = {
      {.opcode = movescape::Opcode::LdU64, .operands = {10}},
      {.opcode = movescape::Opcode::LdU64, .operands = {20}},
      {.opcode = movescape::Opcode::Add},
      {.opcode = movescape::Opcode::Ret},
  };
  module.function_definitions.push_back({
      .handle = 0,
      .visibility = movescape::Visibility::Public,
      .code = unit,
  });
  return module;
}

} // namespace

TEST(stack_effect_for_call_uses_signatures) {
  auto module = arithmeticModule();
  module.function_handles.push_back({
      .module = 0,
      .name = 1,
      .parameters = 1,
      .returns = 1,
  });
  const movescape::Instruction call{
      .opcode = movescape::Opcode::Call,
      .operands = {1},
  };
  REQUIRE_EQ(movescape::instructionStackEffect(module, module.function_definitions[0], call), (movescape::StackEffect{.pops = 1, .pushes = 1}));
}

TEST(stackless_lift_assigns_stable_values_and_operands) {
  const auto module = arithmeticModule();
  const auto &function = module.function_definitions[0];
  const auto cfg = movescape::buildControlFlowGraph(*function.code);
  const auto lifted = movescape::liftToStackless(module, function, cfg);
  REQUIRE_EQ(lifted.value_count, 3U);
  REQUIRE_EQ(lifted.maximum_stack_height, 2U);
  REQUIRE_EQ(lifted.blocks[0].instructions[2].inputs, (std::vector<movescape::ValueId>{0, 1}));
  REQUIRE_EQ(lifted.blocks[0].instructions[2].outputs, (std::vector<movescape::ValueId>{2}));
  REQUIRE_EQ(lifted.blocks[0].instructions[3].inputs, (std::vector<movescape::ValueId>{2}));
}

TEST(stack_usage_rejects_underflow) {
  auto module = arithmeticModule();
  module.function_handles[0].returns = 0;
  module.function_definitions[0].code->code = {
      {.opcode = movescape::Opcode::Pop},
      {.opcode = movescape::Opcode::Ret},
  };
  const auto &function = module.function_definitions[0];
  const auto cfg = movescape::buildControlFlowGraph(*function.code);
  REQUIRE_ERROR(movescape::validateStackUsage(module, function, cfg), movescape::ErrorCode::Malformed);
}

TEST(stack_usage_requires_empty_branch_boundaries) {
  auto module = arithmeticModule();
  module.function_handles[0].returns = 0;
  module.function_definitions[0].code->code = {
      {.opcode = movescape::Opcode::LdTrue},
      {.opcode = movescape::Opcode::Branch, .operands = {2}},
      {.opcode = movescape::Opcode::Ret},
  };
  const auto &function = module.function_definitions[0];
  const auto cfg = movescape::buildControlFlowGraph(*function.code);
  REQUIRE_ERROR(movescape::validateStackUsage(module, function, cfg), movescape::ErrorCode::Malformed);
}

TEST(type_substitution_walks_nested_types) {
  movescape::Type parameter{.kind = movescape::TypeKind::TypeParameter, .index = 0};
  movescape::Type vector{
      .kind = movescape::TypeKind::Vector,
      .arguments = {parameter},
  };
  const auto result = movescape::substituteType(vector, {movescape::Type{.kind = movescape::TypeKind::U64}});
  REQUIRE_EQ(result.kind, movescape::TypeKind::Vector);
  REQUIRE_EQ(result.arguments.at(0).kind, movescape::TypeKind::U64);
}

TEST(function_type_assignability_accepts_an_ability_superset) {
  movescape::Type expected{
      .kind = movescape::TypeKind::Function,
      .abilities = movescape::AbilitySet{static_cast<std::uint8_t>(movescape::AbilitySet::Copy | movescape::AbilitySet::Store)},
      .arguments =
          {
              movescape::Type{.kind = movescape::TypeKind::U64},
          },
      .results =
          {
              movescape::Type{.kind = movescape::TypeKind::Bool},
          },
  };
  auto actual = expected;
  actual.abilities.bits = static_cast<std::uint8_t>(actual.abilities.bits | movescape::AbilitySet::Drop);
  REQUIRE(movescape::isTypeAssignable(expected, actual));
  REQUIRE(!movescape::isTypeAssignable(actual, expected));
}

TEST(typed_lift_rejects_boolean_arithmetic) {
  auto module = arithmeticModule();
  module.function_definitions[0].code->code = {
      {.opcode = movescape::Opcode::LdTrue},
      {.opcode = movescape::Opcode::LdFalse},
      {.opcode = movescape::Opcode::Add},
      {.opcode = movescape::Opcode::Ret},
  };
  const auto &function = module.function_definitions[0];
  const auto cfg = movescape::buildControlFlowGraph(*function.code);
  REQUIRE_ERROR(movescape::liftToStackless(module, function, cfg), movescape::ErrorCode::TypeMismatch);
}

TEST(equality_requires_the_drop_ability) {
  auto module = arithmeticModule();
  const movescape::Type parameter{
      .kind = movescape::TypeKind::TypeParameter,
      .index = 0,
  };
  module.signatures[1] = {movescape::Type{.kind = movescape::TypeKind::Bool}};
  module.signatures.push_back({parameter, parameter});
  module.function_handles[0].parameters = 2;
  module.function_handles[0].type_parameters = {movescape::AbilitySet{}};
  module.function_definitions[0].code->code = {
      {.opcode = movescape::Opcode::MoveLoc, .operands = {0}},
      {.opcode = movescape::Opcode::MoveLoc, .operands = {1}},
      {.opcode = movescape::Opcode::Eq},
      {.opcode = movescape::Opcode::Ret},
  };
  const auto &function = module.function_definitions[0];
  const auto cfg = movescape::buildControlFlowGraph(*function.code);
  REQUIRE_ERROR(movescape::liftToStackless(module, function, cfg), movescape::ErrorCode::TypeMismatch);

  module.function_handles[0].type_parameters[0] = movescape::AbilitySet{movescape::AbilitySet::Drop};
  const auto valid_cfg = movescape::buildControlFlowGraph(*function.code);
  (void)movescape::liftToStackless(module, function, valid_cfg);
}
