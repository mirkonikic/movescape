#include "test.hpp"

#include "movescape/cfg.hpp"
#include "movescape/expression_ir.hpp"
#include "movescape/stackless_ir.hpp"

namespace {

movescape::ExpressionPtr atom(std::string value, movescape::TypeKind kind = movescape::TypeKind::U64) {
  return std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::Nop,
      .type = movescape::Type{.kind = kind},
      .atom = std::move(value),
  });
}

movescape::ExpressionPtr binary(movescape::Opcode opcode, movescape::ExpressionPtr left, movescape::ExpressionPtr right) {
  return std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = opcode,
      .operands = {std::move(left), std::move(right)},
  });
}

movescape::ExpressionPtr loadConstant(std::size_t index) {
  return std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::LdConst,
      .immediate_operands = {index},
  });
}

movescape::Module expressionModule() {
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
  module.function_definitions.push_back({.handle = 0, .code = unit});
  return module;
}

movescape::Module modernExpressionModule() {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"M", "E", "V", "x", "target"};
  module.addresses.push_back({});
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;

  movescape::StructHandle enum_handle{
      .module = 0,
      .name = 1,
  };
  enum_handle.type_parameters.push_back({});
  module.struct_handles.push_back(enum_handle);
  movescape::StructDefinition definition{
      .handle = 0,
      .field_kind = movescape::StructFieldKind::Variants,
  };
  definition.variants.push_back({
      .name = 2,
      .fields = {{
          .name = 3,
          .type =
              movescape::Type{
                  .kind = movescape::TypeKind::TypeParameter,
                  .index = 0,
              },
      }},
  });
  module.struct_definitions.push_back(definition);
  module.struct_variant_handles.push_back({
      .definition = 0,
      .variant = 0,
  });
  module.signatures.push_back({
      movescape::Type{.kind = movescape::TypeKind::U64},
  });
  module.signatures.push_back({
      movescape::Type{.kind = movescape::TypeKind::U64},
      movescape::Type{.kind = movescape::TypeKind::U64},
  });
  module.struct_variant_instantiations.push_back({
      .handle = 0,
      .type_parameters = 0,
  });

  module.function_handles.push_back({
      .module = 0,
      .name = 4,
      .parameters = 1,
      .returns = 0,
  });
  return module;
}

} // namespace

TEST(expression_recovery_builds_nested_arithmetic_return) {
  const auto module = expressionModule();
  const auto &function = module.function_definitions[0];
  const auto cfg = movescape::buildControlFlowGraph(*function.code);
  const auto stackless = movescape::liftToStackless(module, function, cfg);
  const auto expressions = movescape::recoverExpressions(module, function, cfg, stackless);
  REQUIRE_EQ(expressions.blocks.size(), 1U);
  REQUIRE_EQ(expressions.blocks[0].terminator.kind, movescape::ExpressionTerminatorKind::Return);
  const auto text = movescape::formatExpressionFunction(module, expressions);
  REQUIRE(text.find("return 10u64 + 20u64") != std::string::npos);
}

TEST(expression_rendering_uses_precedence_without_changing_associativity) {
  const movescape::Module module;
  REQUIRE_EQ(movescape::renderExpression(module, binary(movescape::Opcode::Add, atom("a"), binary(movescape::Opcode::Mul, atom("b"), atom("c")))),
             std::string("a + b * c"));
  REQUIRE_EQ(movescape::renderExpression(module, binary(movescape::Opcode::Mul, binary(movescape::Opcode::Add, atom("a"), atom("b")), atom("c"))),
             std::string("(a + b) * c"));
  REQUIRE_EQ(movescape::renderExpression(module, binary(movescape::Opcode::Sub, atom("a"), binary(movescape::Opcode::Sub, atom("b"), atom("c")))),
             std::string("a - (b - c)"));
  REQUIRE_EQ(movescape::renderExpression(module, binary(movescape::Opcode::Sub, binary(movescape::Opcode::Sub, atom("a"), atom("b")), atom("c"))),
             std::string("a - b - c"));
  REQUIRE_EQ(movescape::renderExpression(module, binary(movescape::Opcode::Or, atom("a"), binary(movescape::Opcode::And, atom("b"), atom("c")))),
             std::string("a || b && c"));
  REQUIRE_EQ(movescape::renderExpression(module, binary(movescape::Opcode::And, binary(movescape::Opcode::Or, atom("a"), atom("b")), atom("c"))),
             std::string("(a || b) && c"));
}

TEST(expression_rendering_parenthesizes_only_weaker_unary_operands) {
  const movescape::Module module;
  auto logical = std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::Not,
      .operands =
          {
              binary(movescape::Opcode::And, atom("a", movescape::TypeKind::Bool), atom("b", movescape::TypeKind::Bool)),
          },
  });
  REQUIRE_EQ(movescape::renderExpression(module, logical), std::string("!(a && b)"));

  auto arithmetic = std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::Negate,
      .operands =
          {
              binary(movescape::Opcode::Add, atom("a"), atom("b")),
          },
  });
  REQUIRE_EQ(movescape::renderExpression(module, arithmetic), std::string("-(a + b)"));
}

TEST(direct_boolean_bytecode_preserves_eager_operand_evaluation) {
  const movescape::Module module;
  auto left = atom("left()");
  left->effect = movescape::ExpressionEffect::MayAbort;
  auto right = atom("right()");
  right->effect = movescape::ExpressionEffect::SideEffect;
  auto expression = binary(movescape::Opcode::And, left, right);
  expression->bytecode_index = 17;

  REQUIRE_EQ(movescape::renderExpression(module, expression), std::string("({ let movescape_bool_lhs17 = left(); let "
                                                                         "movescape_bool_rhs17 = right(); movescape_bool_lhs17 && "
                                                                         "movescape_bool_rhs17 })"));

  expression->short_circuit = true;
  REQUIRE_EQ(movescape::renderExpression(module, expression), std::string("left() && right()"));
}

TEST(expression_rendering_decodes_concrete_bcs_constants) {
  movescape::Module module;
  std::vector<std::uint8_t> address_bytes(32, 0);
  address_bytes[30] = 0xca;
  address_bytes[31] = 0xfe;
  module.constants = {
      {.type = movescape::Type{.kind = movescape::TypeKind::Bool}, .data = {1}},
      {.type = movescape::Type{.kind = movescape::TypeKind::Vector, .arguments = {movescape::Type{.kind = movescape::TypeKind::U8}}}, .data = {3, 10, 20, 30}},
      {.type = movescape::Type{.kind = movescape::TypeKind::I128}, .data = std::vector<std::uint8_t>(16, 0xff)},
      {.type = movescape::Type{.kind = movescape::TypeKind::Address}, .data = std::move(address_bytes)},
      {.type = movescape::Type{.kind = movescape::TypeKind::Vector, .arguments = {movescape::Type{.kind = movescape::TypeKind::U8}}},
       .data = {5, 'h', 'e', 'l', 'l', 'o'}},
      {.type = movescape::Type{.kind = movescape::TypeKind::Vector, .arguments = {movescape::Type{.kind = movescape::TypeKind::U8}}},
       .data = {5, 'h', 'i', ' ', 0xc5, 0xbe}},
      {.type = movescape::Type{.kind = movescape::TypeKind::Vector, .arguments = {movescape::Type{.kind = movescape::TypeKind::U8}}},
       .data = {4, '"', '\\', '\n', 'A'}},
      {.type = movescape::Type{.kind = movescape::TypeKind::Vector, .arguments = {movescape::Type{.kind = movescape::TypeKind::U8}}}, .data = {2, 0xc0, 0x80}},
  };

  REQUIRE_EQ(movescape::renderExpression(module, loadConstant(0)), std::string("true"));
  REQUIRE_EQ(movescape::renderExpression(module, loadConstant(1)), std::string("x\"0a141e\""));
  REQUIRE_EQ(movescape::renderExpression(module, loadConstant(2)), std::string("-0x1i128"));
  REQUIRE_EQ(movescape::renderExpression(module, loadConstant(3)), std::string("@0xcafe"));
  REQUIRE_EQ(movescape::renderExpression(module, loadConstant(4)), std::string("b\"hello\""));
  REQUIRE_EQ(movescape::renderExpression(module, loadConstant(5)), std::string("b\"hi \\xc5\\xbe\""));
  REQUIRE_EQ(movescape::renderExpression(module, loadConstant(6)), std::string("b\"\\\"\\\\\\nA\""));
  REQUIRE_EQ(movescape::renderExpression(module, loadConstant(7)), std::string("x\"c080\""));
  REQUIRE(movescape::expressionSourceSemanticsComplete(module, loadConstant(2)));
  REQUIRE(movescape::expressionSourceSemanticsComplete(module, loadConstant(3)));
}

TEST(expression_rendering_rejects_unrepresentable_constants) {
  movescape::Module module;
  module.constants = {
      {.type = movescape::Type{.kind = movescape::TypeKind::U8}, .data = {7, 8}},
      {.type = movescape::Type{.kind = movescape::TypeKind::Bool}, .data = {2}},
      {.type = movescape::Type{.kind = movescape::TypeKind::Signer}, .data = {}},
  };

  REQUIRE_ERROR(movescape::renderExpression(module, loadConstant(0)), movescape::ErrorCode::UnsupportedFeature);
  REQUIRE_ERROR(movescape::renderExpression(module, loadConstant(1)), movescape::ErrorCode::UnsupportedFeature);
  REQUIRE_ERROR(movescape::renderExpression(module, loadConstant(2)), movescape::ErrorCode::UnsupportedFeature);
}

TEST(expression_rendering_rejects_unknown_pseudo_operations) {
  const movescape::Module module;
  const auto expression = std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::Nop,
  });
  REQUIRE_ERROR(movescape::renderExpression(module, expression), movescape::ErrorCode::UnsupportedFeature);
}

TEST(expression_rendering_formats_negative_wide_immediates) {
  const movescape::Module module;
  auto expression =
      std::make_shared<movescape::Expression>(movescape::Expression{.opcode = movescape::Opcode::LdI256, .wide_immediate = std::vector<std::uint8_t>(32, 0xff)});
  REQUIRE_EQ(movescape::renderExpression(module, expression), std::string("-0x1i256"));
}

TEST(expression_recovery_preserves_conditional_edge_meanings) {
  auto module = expressionModule();
  module.function_handles[0].returns = 0;
  module.function_definitions[0].code->code = {
      {.opcode = movescape::Opcode::LdTrue},
      {.opcode = movescape::Opcode::BrFalse, .operands = {3}},
      {.opcode = movescape::Opcode::Branch, .operands = {3}},
      {.opcode = movescape::Opcode::Ret},
  };
  const auto &function = module.function_definitions[0];
  const auto cfg = movescape::buildControlFlowGraph(*function.code);
  const auto stackless = movescape::liftToStackless(module, function, cfg);
  const auto expressions = movescape::recoverExpressions(module, function, cfg, stackless);
  const auto &terminator = expressions.blocks[0].terminator;
  REQUIRE_EQ(terminator.kind, movescape::ExpressionTerminatorKind::Conditional);
  REQUIRE_EQ(*terminator.true_target, 1U);
  REQUIRE_EQ(*terminator.false_target, 2U);
}

TEST(expression_recovery_materializes_a_value_surviving_an_assignment) {
  auto module = expressionModule();
  module.signatures.push_back({
      movescape::Type{.kind = movescape::TypeKind::U64},
      movescape::Type{.kind = movescape::TypeKind::U64},
  });
  module.function_handles[0].parameters = 2;
  module.function_handles[0].returns = 1;
  module.function_definitions[0].code->code = {
      {.opcode = movescape::Opcode::MoveLoc, .operands = {1}}, {.opcode = movescape::Opcode::MoveLoc, .operands = {0}},
      {.opcode = movescape::Opcode::StLoc, .operands = {1}},   {.opcode = movescape::Opcode::StLoc, .operands = {0}},
      {.opcode = movescape::Opcode::MoveLoc, .operands = {0}}, {.opcode = movescape::Opcode::Ret},
  };
  const auto &function = module.function_definitions[0];
  const auto cfg = movescape::buildControlFlowGraph(*function.code);
  const auto stackless = movescape::liftToStackless(module, function, cfg);
  const auto expressions = movescape::recoverExpressions(module, function, cfg, stackless);
  const auto text = movescape::formatExpressionFunction(module, expressions);
  REQUIRE(text.find("let tmp0 = local1") != std::string::npos);
  REQUIRE(text.find("local0 = tmp0") != std::string::npos);
}

TEST(render_generic_enum_construction_and_destruction) {
  const auto module = modernExpressionModule();
  auto packed = std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::PackVariantGeneric,
      .operands = {atom("7u64")},
      .immediate_operands = {0},
  });
  REQUIRE_EQ(movescape::renderExpression(module, packed), std::string("0x0::M::E::V<u64> { x: 7u64 }"));

  movescape::ExpressionStatement unpack{
      .kind = movescape::ExpressionStatementKind::Destructure,
      .generated_values = {7},
      .expression = std::make_shared<movescape::Expression>(movescape::Expression{
          .opcode = movescape::Opcode::UnpackVariantGeneric,
          .operands = {atom("value")},
          .immediate_operands = {0},
      }),
  };
  REQUIRE_EQ(movescape::renderExpressionStatement(module, unpack), std::string("let 0x0::M::E::V<u64> { x: tmp7 } = value;"));
}

TEST(render_variant_test_and_closure_pack) {
  const auto module = modernExpressionModule();
  auto test = std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::TestVariantGeneric,
      .operands = {atom("&value")},
      .immediate_operands = {0},
  });
  REQUIRE_EQ(movescape::renderExpression(module, test), std::string("(&value is V)"));

  auto closure = std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::PackClosure,
      .operands = {atom("captured")},
      .immediate_operands = {0, 1},
  });
  REQUIRE_EQ(movescape::renderExpression(module, closure), std::string("(|closure_arg1| 0x0::M::target(captured, closure_arg1))"));

  auto direct = std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::PackClosure,
      .immediate_operands = {0, 0},
  });
  REQUIRE_EQ(movescape::renderExpression(module, direct), std::string("0x0::M::target"));

  auto captured_local = std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::MoveLoc,
      .immediate_operands = {0},
      .local_name = "x",
  });
  auto curried = std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::PackClosure,
      .operands = {captured_local},
      .immediate_operands = {0, 1},
  });
  REQUIRE_EQ(movescape::renderExpression(module, curried), std::string("(|closure_arg1| 0x0::M::target(x, closure_arg1))"));
}

TEST(folds_short_circuit_tests_of_one_enum_into_a_variant_list) {
  auto module = modernExpressionModule();
  module.identifiers.push_back("W");
  module.struct_definitions[0].variants.push_back({.name = 5});
  module.struct_variant_handles.push_back({.definition = 0, .variant = 1});
  module.struct_variant_instantiations.push_back({
      .handle = 1,
      .type_parameters = 0,
  });

  auto value = atom("&value");
  auto first = std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::TestVariantGeneric,
      .operands = {value},
      .immediate_operands = {0},
  });
  auto second = std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::TestVariantGeneric,
      .operands = {value},
      .immediate_operands = {1},
  });
  auto either = std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::Or,
      .operands = {first, second},
      .short_circuit = true,
  });
  REQUIRE_EQ(movescape::renderExpression(module, either), std::string("(&value is V|W)"));
}

TEST(render_field_reads_and_writes_as_places) {
  auto module = modernExpressionModule();
  module.struct_definitions[0].field_kind = movescape::StructFieldKind::Declared;
  module.struct_definitions[0].fields = {{
      .name = 3,
      .type = movescape::Type{.kind = movescape::TypeKind::U64},
  }};
  module.field_handles.push_back({.owner = 0, .field = 0});

  auto local = std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::MoveLoc,
      .immediate_operands = {0},
  });
  auto borrow = std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::MutBorrowField,
      .operands = {local},
      .immediate_operands = {0},
  });
  auto read = std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::ReadRef,
      .operands = {borrow},
  });
  REQUIRE_EQ(movescape::renderExpression(module, read), std::string("local0.x"));

  auto write = std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::WriteRef,
      .operands = {atom("9u64"), borrow},
  });
  REQUIRE_EQ(movescape::renderExpression(module, write), std::string("local0.x = 9u64"));

  auto direct_write = std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::WriteRef,
      .operands = {atom("10u64"), local},
  });
  REQUIRE_EQ(movescape::renderExpression(module, direct_write), std::string("*local0 = 10u64"));
}

TEST(render_vm_vector_operations_with_a_bound_module_name) {
  movescape::Module module;
  auto length = std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::VecLen,
      .operands = {atom("&values")},
  });
  REQUIRE_EQ(movescape::renderExpression(module, length), std::string("0x1::vector::length(&values)"));

  auto push = std::make_shared<movescape::Expression>(movescape::Expression{
      .opcode = movescape::Opcode::VecPushBack,
      .operands = {atom("&mut values"), atom("7u64")},
  });
  REQUIRE_EQ(movescape::renderExpression(module, push), std::string("0x1::vector::push_back(&mut values, 7u64)"));
}
