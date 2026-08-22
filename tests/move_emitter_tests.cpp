#include "test.hpp"

#include "movescape/move_emitter.hpp"

TEST(emits_a_complete_minimal_move_module) {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"M", "f"};
  module.addresses.push_back({});
  module.addresses[0][31] = 1;
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
  unit.code.push_back({.opcode = movescape::Opcode::Ret});
  module.function_definitions.push_back({
      .handle = 0,
      .visibility = movescape::Visibility::Public,
      .code = unit,
  });

  const auto result = movescape::emitMoveModule(module);
  REQUIRE(result.allControlFlowComplete());
  REQUIRE(result.allSourceSemanticsComplete());
  REQUIRE(result.source.find("module 0x1::M") != std::string::npos);
  REQUIRE(result.source.find("public fun f()") != std::string::npos);
  REQUIRE(result.source.find("return;") != std::string::npos);
}

TEST(emits_concrete_arithmetic_move_source_exactly) {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"Math", "add"};
  module.addresses.push_back({});
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  const movescape::Type u64{.kind = movescape::TypeKind::U64};
  module.signatures = {{}, {u64, u64}, {u64}};
  module.function_handles.push_back({
      .module = 0,
      .name = 1,
      .parameters = 1,
      .returns = 2,
  });
  movescape::CodeUnit unit;
  unit.locals = 0;
  unit.code = {
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::CopyLoc, .operands = {1}},
      {.opcode = movescape::Opcode::Add},
      {.opcode = movescape::Opcode::Ret},
  };
  module.function_definitions.push_back({
      .handle = 0,
      .visibility = movescape::Visibility::Public,
      .code = unit,
  });

  const auto result = movescape::emitMoveModule(module);
  REQUIRE(result.allControlFlowComplete());
  REQUIRE(result.allSourceSemanticsComplete());
  REQUIRE_EQ(result.source, std::string("// Decompiled by movescape from Aptos Move bytecode v10.\n"
                                        "// Compiler policy: bytecode >= v10, Move language >= "
                                        "2.0.\n"
                                        "// Generated names were not present in the original "
                                        "source.\n"
                                        "module 0x0::Math {\n\n"
                                        "  public fun add(local0: u64, local1: u64): u64 {\n"
                                        "    return local0 + local1\n"
                                        "  }\n\n"
                                        "}\n"));
}

TEST(source_policy_tracks_version_gated_move_syntax) {
  movescape::Module module;
  module.version = 8;
  movescape::Type function_type{.kind = movescape::TypeKind::Function};
  function_type.abilities.bits = movescape::AbilitySet::Copy | movescape::AbilitySet::Drop;
  module.signatures.push_back({function_type});

  auto policy = movescape::sourcePolicy(module);
  REQUIRE_EQ(policy.minimum_bytecode_version, 8U);
  REQUIRE_EQ(policy.minimum_language_version, std::string("2.2"));

  module.version = 9;
  module.signatures.push_back({movescape::Type{.kind = movescape::TypeKind::I64}});
  policy = movescape::sourcePolicy(module);
  REQUIRE_EQ(policy.minimum_bytecode_version, 9U);
  REQUIRE_EQ(policy.minimum_language_version, std::string("2.3"));

  module.version = 10;
  movescape::CodeUnit code;
  code.code.push_back({.opcode = movescape::Opcode::AbortMsg});
  module.function_definitions.push_back({.code = code});
  policy = movescape::sourcePolicy(module);
  REQUIRE_EQ(policy.minimum_bytecode_version, 10U);
  REQUIRE_EQ(policy.minimum_language_version, std::string("2.4"));
}

TEST(emits_a_concrete_header_tested_loop_as_while_source) {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"Loops", "spin"};
  module.addresses.push_back({});
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  module.signatures = {
      {},
      {movescape::Type{.kind = movescape::TypeKind::Bool}},
  };
  module.function_handles.push_back({
      .module = 0,
      .name = 1,
      .parameters = 1,
      .returns = 0,
  });
  movescape::CodeUnit unit;
  unit.locals = 0;
  unit.code = {
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::BrFalse, .operands = {4}},
      {.opcode = movescape::Opcode::Nop},
      {.opcode = movescape::Opcode::Branch, .operands = {0}},
      {.opcode = movescape::Opcode::Ret},
  };
  module.function_definitions.push_back({.handle = 0, .code = unit});

  const auto result = movescape::emitMoveModule(module);
  REQUIRE(result.allControlFlowComplete());
  REQUIRE(result.allSourceSemanticsComplete());
  REQUIRE(result.source.find("while (local0) {\n    }\n    return;") != std::string::npos);
  REQUIRE(result.source.find("loop {") == std::string::npos);
}

TEST(emits_a_concrete_abort_guard_as_an_assertion) {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"Checks", "require"};
  module.addresses.push_back({});
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  module.signatures = {
      {},
      {movescape::Type{.kind = movescape::TypeKind::Bool}},
  };
  module.function_handles.push_back({
      .module = 0,
      .name = 1,
      .parameters = 1,
      .returns = 0,
  });
  movescape::CodeUnit unit;
  unit.locals = 0;
  unit.code = {
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::BrFalse, .operands = {3}},
      {.opcode = movescape::Opcode::Ret},
      {.opcode = movescape::Opcode::LdU64, .operands = {42}},
      {.opcode = movescape::Opcode::Abort},
  };
  module.function_definitions.push_back({.handle = 0, .code = unit});

  const auto result = movescape::emitMoveModule(module);
  REQUIRE(result.allControlFlowComplete());
  REQUIRE(result.allSourceSemanticsComplete());
  REQUIRE(result.source.find("assert!(local0, 42u64);\n"
                             "    return;") != std::string::npos);
  REQUIRE(result.source.find("if (local0)") == std::string::npos);
}

TEST(emits_value_returning_branches_as_a_conditional_expression) {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"Conditions", "choose"};
  module.addresses.push_back({});
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
  unit.code = {
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}}, {.opcode = movescape::Opcode::BrFalse, .operands = {4}},
      {.opcode = movescape::Opcode::LdU64, .operands = {10}},  {.opcode = movescape::Opcode::Ret},
      {.opcode = movescape::Opcode::LdU64, .operands = {20}},  {.opcode = movescape::Opcode::Ret},
  };
  module.function_definitions.push_back({.handle = 0, .code = unit});

  const auto result = movescape::emitMoveModule(module);
  REQUIRE(result.allControlFlowComplete());
  REQUIRE(result.allSourceSemanticsComplete());
  REQUIRE(result.source.find("return if (local0) { 10u64 } else { 20u64 }") != std::string::npos);
}

TEST(emits_a_concrete_latch_tested_loop_with_a_tail_break) {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"Loops", "repeat"};
  module.addresses.push_back({});
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  const movescape::Type boolean{.kind = movescape::TypeKind::Bool};
  module.signatures = {{}, {boolean}, {boolean}};
  module.function_handles.push_back({
      .module = 0,
      .name = 1,
      .parameters = 1,
      .returns = 0,
  });
  movescape::CodeUnit unit;
  unit.locals = 2;
  unit.code = {
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::StLoc, .operands = {1}},
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::BrTrue, .operands = {0}},
      {.opcode = movescape::Opcode::Ret},
  };
  module.function_definitions.push_back({.handle = 0, .code = unit});

  const auto result = movescape::emitMoveModule(module);
  REQUIRE(result.allControlFlowComplete());
  REQUIRE(result.allSourceSemanticsComplete());
  REQUIRE(result.source.find("loop {\n"
                             "      local1 = local0;\n"
                             "      if (!local0) {\n"
                             "        break;\n"
                             "      }\n"
                             "    }\n"
                             "    return;") != std::string::npos);
  REQUIRE(result.source.find("while (") == std::string::npos);
}

TEST(emits_concrete_short_circuit_control_flow_as_a_compound_condition) {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"Conditions", "guard"};
  module.addresses.push_back({});
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  const movescape::Type boolean{.kind = movescape::TypeKind::Bool};
  module.signatures = {{}, {boolean, boolean}};
  module.function_handles.push_back({
      .module = 0,
      .name = 1,
      .parameters = 1,
      .returns = 0,
  });
  movescape::CodeUnit unit;
  unit.locals = 0;
  unit.code = {
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::BrFalse, .operands = {6}},
      {.opcode = movescape::Opcode::CopyLoc, .operands = {1}},
      {.opcode = movescape::Opcode::BrFalse, .operands = {6}},
      {.opcode = movescape::Opcode::LdU64, .operands = {42}},
      {.opcode = movescape::Opcode::Abort},
      {.opcode = movescape::Opcode::Ret},
  };
  module.function_definitions.push_back({.handle = 0, .code = unit});

  const auto result = movescape::emitMoveModule(module);
  REQUIRE(result.allControlFlowComplete());
  REQUIRE(result.allSourceSemanticsComplete());
  REQUIRE(result.source.find("assert!(!(local0 && local1), 42u64);\n"
                             "    return;") != std::string::npos);
  REQUIRE(result.source.find("if (local1)") == std::string::npos);
}

TEST(emits_source_visible_function_attributes) {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"M", "f"};
  module.addresses.push_back({});
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  module.signatures.push_back({});
  movescape::FunctionHandle handle{
      .module = 0,
      .name = 1,
      .parameters = 0,
      .returns = 0,
  };
  handle.attributes.push_back({
      .kind = movescape::FunctionAttributeKind::Persistent,
  });
  module.function_handles.push_back(std::move(handle));
  movescape::CodeUnit unit;
  unit.locals = 0;
  unit.code.push_back({.opcode = movescape::Opcode::Ret});
  module.function_definitions.push_back({
      .handle = 0,
      .code = unit,
  });

  const auto result = movescape::emitMoveModule(module);
  REQUIRE(result.source.find("#[persistent]\n  fun f()") != std::string::npos);
  REQUIRE_EQ(result.policy.minimum_language_version, std::string("2.2"));
}

TEST(emitter_rejects_malformed_constants_instead_of_manufacturing_behavior) {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"M", "read_constant"};
  module.addresses.push_back({});
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  module.signatures.push_back({});
  module.signatures.push_back({movescape::Type{.kind = movescape::TypeKind::U8}});
  module.constants.push_back({
      .type = movescape::Type{.kind = movescape::TypeKind::U8},
      .data = {},
  });
  module.function_handles.push_back({
      .module = 0,
      .name = 1,
      .parameters = 0,
      .returns = 1,
  });
  movescape::CodeUnit unit;
  unit.locals = 0;
  unit.code = {
      {.opcode = movescape::Opcode::LdConst, .operands = {0}},
      {.opcode = movescape::Opcode::Ret},
  };
  module.function_definitions.push_back({.handle = 0, .code = unit});

  REQUIRE_ERROR(movescape::emitMoveModule(module), movescape::ErrorCode::UnsupportedFeature);
}

TEST(emits_a_typed_dispatcher_for_safe_irreducible_control_flow) {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"Irreducible", "run"};
  module.addresses.push_back({});
  module.addresses[0].back() = 0x42;
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  const movescape::Type boolean{.kind = movescape::TypeKind::Bool};
  module.signatures = {{}, {boolean, boolean}};
  module.function_handles.push_back({
      .module = 0,
      .name = 1,
      .parameters = 1,
      .returns = 0,
  });

  // bb0 enters either bb1 or bb2. bb1 -> bb2, while bb2 can return to bb1,
  // making {bb1, bb2} a genuine multiple-entry cyclic SCC.
  movescape::CodeUnit unit;
  unit.locals = 0;
  unit.code = {
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}}, {.opcode = movescape::Opcode::BrTrue, .operands = {3}},
      {.opcode = movescape::Opcode::Branch, .operands = {3}},  {.opcode = movescape::Opcode::CopyLoc, .operands = {1}},
      {.opcode = movescape::Opcode::BrTrue, .operands = {2}},  {.opcode = movescape::Opcode::Ret},
  };
  module.function_definitions.push_back({
      .handle = 0,
      .visibility = movescape::Visibility::Public,
      .code = unit,
  });

  const auto result = movescape::emitMoveModule(module);
  REQUIRE(result.allControlFlowComplete());
  REQUIRE(result.allSourceSemanticsComplete());
  REQUIRE(result.source.find("let movescape_dispatch_state: u64;") != std::string::npos);
  REQUIRE(result.source.find("movescape_dispatch_state = 0u64;") != std::string::npos);
  REQUIRE(result.source.find("copy movescape_dispatch_state == 1u64") != std::string::npos);
  REQUIRE(result.source.find("movescape_dispatch_state = 2u64;") != std::string::npos);
  REQUIRE(result.source.find("} else if (copy movescape_dispatch_state") != std::string::npos);
  REQUIRE(result.source.find("abort") == std::string::npos);
}

TEST(irreducible_dispatcher_rejects_path_sensitive_declared_locals) {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"Irreducible", "unsafe_local"};
  module.addresses.push_back({});
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  const movescape::Type boolean{.kind = movescape::TypeKind::Bool};
  module.signatures = {{}, {boolean, boolean}, {boolean}};
  module.function_handles.push_back({
      .module = 0,
      .name = 1,
      .parameters = 1,
      .returns = 0,
  });
  movescape::CodeUnit unit;
  unit.locals = 2;
  unit.code = {
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}}, {.opcode = movescape::Opcode::BrTrue, .operands = {3}},
      {.opcode = movescape::Opcode::Branch, .operands = {3}},  {.opcode = movescape::Opcode::CopyLoc, .operands = {1}},
      {.opcode = movescape::Opcode::BrTrue, .operands = {2}},  {.opcode = movescape::Opcode::Ret},
  };
  module.function_definitions.push_back({
      .handle = 0,
      .visibility = movescape::Visibility::Public,
      .code = unit,
  });

  REQUIRE_ERROR(movescape::emitMoveModule(module), movescape::ErrorCode::UnsupportedFeature);
}

TEST(emitter_rejects_bytecode_only_names_instead_of_changing_the_abi) {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"$M", "caller", "$f", "_f", "$S", "$x"};
  module.addresses.push_back({});
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  module.signatures.push_back({});

  module.struct_handles.push_back({.module = 0, .name = 4});
  module.struct_definitions.push_back({
      .handle = 0,
      .field_kind = movescape::StructFieldKind::Declared,
      .fields = {{.name = 5, .type = movescape::Type{.kind = movescape::TypeKind::U8}}},
  });

  module.function_handles = {
      {.module = 0, .name = 1, .parameters = 0, .returns = 0},
      {.module = 0, .name = 2, .parameters = 0, .returns = 0},
      {.module = 0, .name = 3, .parameters = 0, .returns = 0},
  };
  movescape::CodeUnit caller;
  caller.locals = 0;
  caller.code = {
      {.opcode = movescape::Opcode::Call, .operands = {1}},
      {.opcode = movescape::Opcode::Ret},
  };
  module.function_definitions = {
      {.handle = 0, .code = caller},
      {.handle = 1},
      {.handle = 2},
  };

  REQUIRE_ERROR(movescape::emitMoveModule(module), movescape::ErrorCode::UnsupportedFeature);
}
