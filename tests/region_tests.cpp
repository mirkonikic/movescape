#include "test.hpp"

#include "movescape/cfg.hpp"
#include "movescape/expression_ir.hpp"
#include "movescape/graph_analysis.hpp"
#include "movescape/region.hpp"
#include "movescape/stackless_ir.hpp"

namespace {

movescape::Module regionModule(std::vector<movescape::Instruction> code) {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"M", "f"};
  module.addresses.push_back({});
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  module.signatures.push_back({});
  module.function_handles.push_back({
      .module = 0,
      .name = 1,
      .parameters = 0,
      .returns = 0,
  });
  movescape::CodeUnit unit;
  unit.locals = 0;
  unit.code = std::move(code);
  module.function_definitions.push_back({.handle = 0, .code = unit});
  return module;
}

movescape::StructuredFunction structure(const movescape::Module &module) {
  const auto &function = module.function_definitions[0];
  const auto cfg = movescape::buildControlFlowGraph(*function.code);
  const auto graph_analysis = movescape::analyzeControlFlowGraph(cfg);
  const auto stackless = movescape::liftToStackless(module, function, cfg);
  const auto expressions = movescape::recoverExpressions(module, function, cfg, stackless);
  return movescape::structureControlFlow(cfg, graph_analysis, expressions);
}

movescape::Module booleanParametersModule(std::size_t parameter_count, std::vector<movescape::Instruction> code) {
  auto module = regionModule({});
  const movescape::Type boolean{.kind = movescape::TypeKind::Bool};
  module.signatures.push_back(movescape::Signature(parameter_count, boolean));
  module.function_handles[0].parameters = 1;
  movescape::CodeUnit unit;
  unit.locals = 0;
  unit.code = std::move(code);
  module.function_definitions[0].code = std::move(unit);
  return module;
}

} // namespace

TEST(structures_a_diamond_without_gotos) {
  const auto module = regionModule({
      {.opcode = movescape::Opcode::LdTrue},
      {.opcode = movescape::Opcode::BrTrue, .operands = {4}},
      {.opcode = movescape::Opcode::Nop},
      {.opcode = movescape::Opcode::Branch, .operands = {5}},
      {.opcode = movescape::Opcode::Nop},
      {.opcode = movescape::Opcode::Ret},
  });
  const auto result = structure(module);
  REQUIRE(result.complete);
  const auto text = movescape::formatStructuredFunction(module, result);
  REQUIRE(text.find("if (true)") != std::string::npos);
  REQUIRE(text.find("unresolved control") == std::string::npos);
}

TEST(renders_assignment_arms_as_move_assignment_expressions) {
  auto module = booleanParametersModule(1, {
                                               {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
                                               {.opcode = movescape::Opcode::BrFalse, .operands = {5}},
                                               {.opcode = movescape::Opcode::LdU64, .operands = {1}},
                                               {.opcode = movescape::Opcode::StLoc, .operands = {1}},
                                               {.opcode = movescape::Opcode::Branch, .operands = {7}},
                                               {.opcode = movescape::Opcode::LdU64, .operands = {2}},
                                               {.opcode = movescape::Opcode::StLoc, .operands = {2}},
                                               {.opcode = movescape::Opcode::Ret},
                                           });
  const movescape::Type u64{.kind = movescape::TypeKind::U64};
  module.signatures.push_back({u64, u64});
  module.function_definitions[0].code->locals = 2;

  const auto result = structure(module);
  REQUIRE(result.complete);
  REQUIRE(result.missing_blocks.empty());
  const auto text = movescape::formatStructuredFunction(module, result);
  REQUIRE(text.find("if (local0) local1 = 1u64 else local2 = 2u64;") != std::string::npos);
}

TEST(structures_multiple_enum_tests_as_one_variant_list) {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"M", "matches", "E", "A", "B"};
  module.addresses.push_back({});
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  module.struct_handles.push_back({.module = 0, .name = 2});
  movescape::StructDefinition definition{
      .handle = 0,
      .field_kind = movescape::StructFieldKind::Variants,
  };
  definition.variants = {{.name = 3}, {.name = 4}};
  module.struct_definitions.push_back(std::move(definition));
  module.struct_variant_handles = {
      {.definition = 0, .variant = 0},
      {.definition = 0, .variant = 1},
  };
  movescape::Type enum_type{.kind = movescape::TypeKind::Struct, .index = 0};
  movescape::Type reference{.kind = movescape::TypeKind::Reference};
  reference.arguments.push_back(enum_type);
  const movescape::Type boolean{.kind = movescape::TypeKind::Bool};
  module.signatures = {{}, {reference}, {boolean}};
  module.signatures.push_back({boolean});
  module.function_handles.push_back({
      .module = 0,
      .name = 1,
      .parameters = 1,
      .returns = 2,
  });
  movescape::CodeUnit unit;
  unit.locals = 3;
  unit.code = {
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::TestVariant, .operands = {0}},
      {.opcode = movescape::Opcode::StLoc, .operands = {1}},
      {.opcode = movescape::Opcode::CopyLoc, .operands = {1}},
      {.opcode = movescape::Opcode::BrFalse, .operands = {9}},
      {.opcode = movescape::Opcode::MoveLoc, .operands = {0}},
      {.opcode = movescape::Opcode::Pop},
      {.opcode = movescape::Opcode::MoveLoc, .operands = {1}},
      {.opcode = movescape::Opcode::Ret},
      {.opcode = movescape::Opcode::MoveLoc, .operands = {0}},
      {.opcode = movescape::Opcode::TestVariant, .operands = {1}},
      {.opcode = movescape::Opcode::StLoc, .operands = {1}},
      {.opcode = movescape::Opcode::CopyLoc, .operands = {1}},
      {.opcode = movescape::Opcode::BrTrue, .operands = {16}},
      {.opcode = movescape::Opcode::MoveLoc, .operands = {1}},
      {.opcode = movescape::Opcode::Ret},
      {.opcode = movescape::Opcode::MoveLoc, .operands = {1}},
      {.opcode = movescape::Opcode::Ret},
  };
  module.function_definitions.push_back({.handle = 0, .code = unit});

  const auto result = structure(module);
  REQUIRE(result.complete);
  REQUIRE(result.missing_blocks.empty());
  REQUIRE(result.duplicated_blocks.empty());
  const auto text = movescape::formatStructuredFunction(module, result);
  REQUIRE(text.find("local0 is A|B") != std::string::npos);
  REQUIRE(text.find("||") == std::string::npos);
}

TEST(structures_a_natural_loop_with_break_and_continue) {
  const auto module = regionModule({
      {.opcode = movescape::Opcode::LdTrue},
      {.opcode = movescape::Opcode::BrFalse, .operands = {4}},
      {.opcode = movescape::Opcode::Nop},
      {.opcode = movescape::Opcode::Branch, .operands = {0}},
      {.opcode = movescape::Opcode::Ret},
  });
  const auto result = structure(module);
  REQUIRE(result.complete);
  const auto text = movescape::formatStructuredFunction(module, result);
  REQUIRE(text.find("while (true) {") != std::string::npos);
  REQUIRE(text.find("loop {") == std::string::npos);
  REQUIRE(text.find("break;") == std::string::npos);
  REQUIRE(text.find("continue;") == std::string::npos);
}

TEST(structures_an_inverted_header_test_as_a_negated_while_condition) {
  const auto module = regionModule({
      {.opcode = movescape::Opcode::LdTrue},
      {.opcode = movescape::Opcode::BrTrue, .operands = {4}},
      {.opcode = movescape::Opcode::Nop},
      {.opcode = movescape::Opcode::Branch, .operands = {0}},
      {.opcode = movescape::Opcode::Ret},
  });
  const auto result = structure(module);
  REQUIRE(result.complete);
  const auto text = movescape::formatStructuredFunction(module, result);
  REQUIRE(text.find("while (!true) {") != std::string::npos);
  REQUIRE(text.find("loop {") == std::string::npos);
}

TEST(pre_tested_while_preserves_an_internal_early_break) {
  auto module = regionModule({});
  const movescape::Type boolean{.kind = movescape::TypeKind::Bool};
  module.signatures.push_back({boolean, boolean});
  module.function_handles[0].parameters = 1;
  movescape::CodeUnit unit;
  unit.locals = 0;
  unit.code = {
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::BrFalse, .operands = {6}},
      {.opcode = movescape::Opcode::CopyLoc, .operands = {1}},
      {.opcode = movescape::Opcode::BrFalse, .operands = {6}},
      {.opcode = movescape::Opcode::Nop},
      {.opcode = movescape::Opcode::Branch, .operands = {0}},
      {.opcode = movescape::Opcode::Ret},
  };
  module.function_definitions[0].code = std::move(unit);

  const auto result = structure(module);
  REQUIRE(result.complete);
  const auto text = movescape::formatStructuredFunction(module, result);
  REQUIRE(text.find("while (local0) {") != std::string::npos);
  REQUIRE(text.find("if (local1)") != std::string::npos);
  REQUIRE(text.find("break;") != std::string::npos);
}

TEST(effectful_loop_headers_remain_general_loops) {
  auto module = regionModule({});
  const movescape::Type boolean{.kind = movescape::TypeKind::Bool};
  module.signatures.push_back({boolean});
  module.signatures.push_back({boolean});
  module.function_handles[0].parameters = 1;
  movescape::CodeUnit unit;
  unit.locals = 2;
  unit.code = {
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::StLoc, .operands = {1}},
      {.opcode = movescape::Opcode::CopyLoc, .operands = {1}},
      {.opcode = movescape::Opcode::BrFalse, .operands = {6}},
      {.opcode = movescape::Opcode::Nop},
      {.opcode = movescape::Opcode::Branch, .operands = {0}},
      {.opcode = movescape::Opcode::Ret},
  };
  module.function_definitions[0].code = std::move(unit);

  const auto result = structure(module);
  REQUIRE(result.complete);
  const auto text = movescape::formatStructuredFunction(module, result);
  REQUIRE(text.find("loop {") != std::string::npos);
  REQUIRE(text.find("local1 = local0;") != std::string::npos);
  REQUIRE(text.find("while (") == std::string::npos);
}

TEST(recovers_a_true_back_edge_as_a_post_tested_loop) {
  auto module = booleanParametersModule(1, {
                                               {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
                                               {.opcode = movescape::Opcode::StLoc, .operands = {1}},
                                               {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
                                               {.opcode = movescape::Opcode::BrTrue, .operands = {0}},
                                               {.opcode = movescape::Opcode::Ret},
                                           });
  module.signatures.push_back({movescape::Type{.kind = movescape::TypeKind::Bool}});
  module.function_definitions[0].code->locals = 2;

  const auto result = structure(module);
  REQUIRE(result.complete);
  REQUIRE(result.missing_blocks.empty());
  REQUIRE(result.duplicated_blocks.empty());
  const auto text = movescape::formatStructuredFunction(module, result);
  REQUIRE(text.find("loop {\n"
                    "    local1 = local0;\n"
                    "    if (!local0) {\n"
                    "      break;\n"
                    "    }\n"
                    "  }") != std::string::npos);
  REQUIRE(text.find("while (") == std::string::npos);
}

TEST(recovers_a_false_back_edge_with_non_inverted_exit_condition) {
  auto module = booleanParametersModule(1, {
                                               {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
                                               {.opcode = movescape::Opcode::StLoc, .operands = {1}},
                                               {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
                                               {.opcode = movescape::Opcode::BrFalse, .operands = {0}},
                                               {.opcode = movescape::Opcode::Ret},
                                           });
  module.signatures.push_back({movescape::Type{.kind = movescape::TypeKind::Bool}});
  module.function_definitions[0].code->locals = 2;

  const auto result = structure(module);
  REQUIRE(result.complete);
  const auto text = movescape::formatStructuredFunction(module, result);
  REQUIRE(text.find("if (local0) {\n      break;") != std::string::npos);
}

TEST(folds_nested_false_paths_into_a_short_circuit_and_condition) {
  const auto module = booleanParametersModule(2, {
                                                     {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
                                                     {.opcode = movescape::Opcode::BrFalse, .operands = {6}},
                                                     {.opcode = movescape::Opcode::CopyLoc, .operands = {1}},
                                                     {.opcode = movescape::Opcode::BrFalse, .operands = {6}},
                                                     {.opcode = movescape::Opcode::LdU64, .operands = {42}},
                                                     {.opcode = movescape::Opcode::Abort},
                                                     {.opcode = movescape::Opcode::Ret},
                                                 });

  const auto result = structure(module);
  REQUIRE(result.complete);
  REQUIRE(result.missing_blocks.empty());
  REQUIRE(result.duplicated_blocks.empty());
  const auto text = movescape::formatStructuredFunction(module, result);
  REQUIRE(text.find("assert!(!(local0 && local1), 42u64);") != std::string::npos);
  REQUIRE(text.find("if (local1)") == std::string::npos);
}

TEST(folds_nested_true_paths_into_a_short_circuit_or_condition) {
  const auto module = booleanParametersModule(2, {
                                                     {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
                                                     {.opcode = movescape::Opcode::BrTrue, .operands = {5}},
                                                     {.opcode = movescape::Opcode::CopyLoc, .operands = {1}},
                                                     {.opcode = movescape::Opcode::BrTrue, .operands = {5}},
                                                     {.opcode = movescape::Opcode::Ret},
                                                     {.opcode = movescape::Opcode::LdU64, .operands = {42}},
                                                     {.opcode = movescape::Opcode::Abort},
                                                 });

  const auto result = structure(module);
  REQUIRE(result.complete);
  REQUIRE(result.missing_blocks.empty());
  REQUIRE(result.duplicated_blocks.empty());
  const auto text = movescape::formatStructuredFunction(module, result);
  REQUIRE(text.find("assert!(!(local0 || local1), 42u64);") != std::string::npos);
  REQUIRE(text.find("if (local1)") == std::string::npos);
}

TEST(does_not_fold_nested_conditions_with_different_common_paths) {
  const auto module = booleanParametersModule(2, {
                                                     {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
                                                     {.opcode = movescape::Opcode::BrFalse, .operands = {8}},
                                                     {.opcode = movescape::Opcode::CopyLoc, .operands = {1}},
                                                     {.opcode = movescape::Opcode::BrFalse, .operands = {6}},
                                                     {.opcode = movescape::Opcode::LdU64, .operands = {42}},
                                                     {.opcode = movescape::Opcode::Abort},
                                                     {.opcode = movescape::Opcode::LdU64, .operands = {7}},
                                                     {.opcode = movescape::Opcode::Abort},
                                                     {.opcode = movescape::Opcode::Ret},
                                                 });

  const auto result = structure(module);
  REQUIRE(result.complete);
  const auto text = movescape::formatStructuredFunction(module, result);
  REQUIRE(text.find("if (local0)") != std::string::npos);
  REQUIRE(text.find("if (local1)") != std::string::npos);
  REQUIRE(text.find("&&") == std::string::npos);
  REQUIRE(text.find("||") == std::string::npos);
}

TEST(recovers_a_false_abort_branch_as_assert) {
  const auto module = booleanParametersModule(1, {
                                                     {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
                                                     {.opcode = movescape::Opcode::BrFalse, .operands = {3}},
                                                     {.opcode = movescape::Opcode::Ret},
                                                     {.opcode = movescape::Opcode::LdU64, .operands = {42}},
                                                     {.opcode = movescape::Opcode::Abort},
                                                 });

  const auto result = structure(module);
  REQUIRE(result.complete);
  REQUIRE(result.missing_blocks.empty());
  REQUIRE(result.duplicated_blocks.empty());
  const auto text = movescape::formatStructuredFunction(module, result);
  REQUIRE(text.find("assert!(local0, 42u64);") != std::string::npos);
  REQUIRE(text.find("if (") == std::string::npos);
  REQUIRE(text.find("return;") != std::string::npos);
}

TEST(recovers_a_true_abort_branch_as_an_inverted_assert) {
  const auto module = booleanParametersModule(1, {
                                                     {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
                                                     {.opcode = movescape::Opcode::BrTrue, .operands = {3}},
                                                     {.opcode = movescape::Opcode::Ret},
                                                     {.opcode = movescape::Opcode::LdU64, .operands = {7}},
                                                     {.opcode = movescape::Opcode::Abort},
                                                 });

  const auto result = structure(module);
  REQUIRE(result.complete);
  const auto text = movescape::formatStructuredFunction(module, result);
  REQUIRE(text.find("assert!(!local0, 7u64);") != std::string::npos);
  REQUIRE(text.find("if (") == std::string::npos);
}

TEST(does_not_rewrite_abort_message_control_flow_as_assert) {
  auto module = booleanParametersModule(1, {
                                               {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
                                               {.opcode = movescape::Opcode::BrFalse, .operands = {3}},
                                               {.opcode = movescape::Opcode::Ret},
                                               {.opcode = movescape::Opcode::LdU64, .operands = {42}},
                                               {.opcode = movescape::Opcode::VecPack, .operands = {2, 0}},
                                               {.opcode = movescape::Opcode::AbortMsg},
                                           });
  module.version = 10;
  module.signatures.push_back({movescape::Type{.kind = movescape::TypeKind::U8}});

  const auto result = structure(module);
  REQUIRE(result.complete);
  const auto text = movescape::formatStructuredFunction(module, result);
  REQUIRE(text.find("assert!(") == std::string::npos);
  REQUIRE(text.find("if (local0)") != std::string::npos);
}

TEST(recovers_value_returning_branches_as_a_conditional_expression) {
  auto module = booleanParametersModule(1, {
                                               {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
                                               {.opcode = movescape::Opcode::BrFalse, .operands = {4}},
                                               {.opcode = movescape::Opcode::LdU64, .operands = {10}},
                                               {.opcode = movescape::Opcode::Ret},
                                               {.opcode = movescape::Opcode::LdU64, .operands = {20}},
                                               {.opcode = movescape::Opcode::Ret},
                                           });
  module.signatures.push_back({movescape::Type{.kind = movescape::TypeKind::U64}});
  module.function_handles[0].returns = 2;

  const auto result = structure(module);
  REQUIRE(result.complete);
  REQUIRE(result.missing_blocks.empty());
  REQUIRE(result.duplicated_blocks.empty());
  const auto text = movescape::formatStructuredFunction(module, result);
  REQUIRE(text.find("return if (local0) { 10u64 } else { 20u64 }") != std::string::npos);
  REQUIRE(text.find("if (local0) {\n") == std::string::npos);
}

TEST(recovers_matching_branch_assignments_as_a_conditional_expression) {
  auto module = booleanParametersModule(1, {
                                               {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
                                               {.opcode = movescape::Opcode::BrFalse, .operands = {5}},
                                               {.opcode = movescape::Opcode::LdU64, .operands = {10}},
                                               {.opcode = movescape::Opcode::StLoc, .operands = {1}},
                                               {.opcode = movescape::Opcode::Branch, .operands = {7}},
                                               {.opcode = movescape::Opcode::LdU64, .operands = {20}},
                                               {.opcode = movescape::Opcode::StLoc, .operands = {1}},
                                               {.opcode = movescape::Opcode::MoveLoc, .operands = {1}},
                                               {.opcode = movescape::Opcode::Ret},
                                           });
  const movescape::Type u64{.kind = movescape::TypeKind::U64};
  module.signatures.push_back({u64});
  module.signatures.push_back({u64});
  module.function_handles[0].returns = 3;
  module.function_definitions[0].code->locals = 2;

  const auto result = structure(module);
  REQUIRE(result.complete);
  REQUIRE(result.missing_blocks.empty());
  REQUIRE(result.duplicated_blocks.empty());
  const auto text = movescape::formatStructuredFunction(module, result);
  REQUIRE(text.find("local1 = if (local0) { 10u64 } else { 20u64 };") != std::string::npos);
  REQUIRE(text.find("return local1") != std::string::npos);
}

TEST(renders_abort_message_only_for_the_well_known_code) {
  movescape::Module module;
  auto make_value = [](movescape::Opcode opcode, std::uint64_t immediate, std::string atom = {}) {
    auto expression = std::make_shared<movescape::Expression>(movescape::Expression{
        .opcode = opcode,
        .immediate_operands = {immediate},
    });
    if (!atom.empty()) {
      expression->atom = std::move(atom);
    }
    return expression;
  };

  movescape::StructuredFunction function;
  function.complete = true;
  function.root = std::make_shared<movescape::Region>();
  function.root->kind = movescape::RegionKind::Abort;
  function.root->values = {
      make_value(movescape::Opcode::LdU64, 0xCA26CBD9BE0B0000ULL),
      make_value(movescape::Opcode::Nop, 0, "message"),
  };
  REQUIRE_EQ(movescape::renderStructuredBody(module, function), std::string("abort message\n"));

  function.root->values[0] = make_value(movescape::Opcode::LdU64, 7);
  const auto explicit_code = movescape::renderStructuredBody(module, function);
  REQUIRE(explicit_code.find("abort 7u64") != std::string::npos);
  REQUIRE(explicit_code.find("WARNING") != std::string::npos);
}

TEST(unresolved_control_transfer_is_rejected_instead_of_changing_behavior) {
  const movescape::Module module;
  movescape::StructuredFunction function;
  function.complete = false;
  function.root = std::make_shared<movescape::Region>();
  function.root->kind = movescape::RegionKind::GotoFallback;
  function.root->target = 7;

  REQUIRE_ERROR(movescape::renderStructuredBody(module, function), movescape::ErrorCode::UnsupportedFeature);
}
