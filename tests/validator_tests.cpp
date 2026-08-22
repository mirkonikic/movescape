#include "test.hpp"

#include "movescape/module.hpp"
#include "movescape/validator.hpp"

namespace {

movescape::Module validModule() {
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
  unit.code.push_back({.opcode = movescape::Opcode::Ret});
  module.function_definitions.push_back({
      .handle = 0,
      .visibility = movescape::Visibility::Public,
      .code = unit,
  });
  return module;
}

void addStructHandle(movescape::Module &module, std::size_t type_parameters) {
  module.identifiers.push_back("S");
  movescape::StructHandle handle{
      .module = 0,
      .name = static_cast<movescape::TableIndex>(module.identifiers.size() - 1),
  };
  handle.type_parameters.resize(type_parameters);
  module.struct_handles.push_back(std::move(handle));
}

movescape::Module globalBorrowModule(bool key, bool declares_acquires) {
  auto module = validModule();
  module.identifiers.push_back("Resource");
  module.struct_handles.push_back({
      .module = 0,
      .name = 2,
      .abilities = key ? movescape::AbilitySet{movescape::AbilitySet::Key} : movescape::AbilitySet{},
  });
  module.struct_definitions.push_back({
      .handle = 0,
      .field_kind = movescape::StructFieldKind::Native,
  });
  module.signatures.push_back({movescape::Type{.kind = movescape::TypeKind::Address}});
  module.function_handles[0].parameters = 1;
  module.function_definitions[0].acquires = declares_acquires ? std::vector<movescape::TableIndex>{0} : std::vector<movescape::TableIndex>{};
  module.function_definitions[0].code->code = {
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::ImmBorrowGlobal, .operands = {0}},
      {.opcode = movescape::Opcode::Pop},
      {.opcode = movescape::Opcode::Ret},
  };
  return module;
}

} // namespace

TEST(validate_minimal_module) {
  const auto module = validModule();
  movescape::validateModule(module);
}

TEST(reject_invalid_self_handle) {
  auto module = validModule();
  module.self_module_handle = 1;
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidIndex);
}

TEST(reject_invalid_branch_target) {
  auto module = validModule();
  module.function_definitions[0].code->code = {
      {.opcode = movescape::Opcode::Branch, .operands = {2}},
  };
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidIndex);
}

TEST(reject_conditional_without_fallthrough) {
  auto module = validModule();
  module.function_definitions[0].code->code = {
      {.opcode = movescape::Opcode::BrTrue, .operands = {0}},
  };
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidIndex);
}

TEST(reject_local_outside_locals_signature) {
  auto module = validModule();
  module.function_definitions[0].code->code = {
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::Ret},
  };
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidIndex);
}

TEST(function_parameters_are_part_of_the_local_index_space) {
  auto module = validModule();
  module.signatures.push_back({movescape::Type{.kind = movescape::TypeKind::U64}});
  module.function_handles[0].parameters = 1;
  module.function_definitions[0].code->code = {
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::Pop},
      {.opcode = movescape::Opcode::Ret},
  };
  movescape::validateModule(module);
}

TEST(reject_code_which_falls_off_the_end) {
  auto module = validModule();
  module.function_definitions[0].code->code = {
      {.opcode = movescape::Opcode::Nop},
  };
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidIndex);
}

TEST(reject_duplicate_struct_and_function_definitions) {
  auto duplicate_function = validModule();
  duplicate_function.function_definitions.push_back(duplicate_function.function_definitions.front());
  REQUIRE_ERROR(movescape::validateModule(duplicate_function), movescape::ErrorCode::InvalidIndex);

  auto duplicate_struct = validModule();
  addStructHandle(duplicate_struct, 0);
  duplicate_struct.struct_definitions = {
      {.handle = 0, .field_kind = movescape::StructFieldKind::Native},
      {.handle = 0, .field_kind = movescape::StructFieldKind::Native},
  };
  REQUIRE_ERROR(movescape::validateModule(duplicate_struct), movescape::ErrorCode::InvalidIndex);
}

TEST(reject_duplicate_friend_declarations) {
  auto module = validModule();
  const movescape::ModuleHandle friend_handle{.address = 0, .name = 0};
  module.friends = {friend_handle, friend_handle};
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidIndex);
}

TEST(reject_cross_address_and_self_friend_declarations) {
  auto cross_address = validModule();
  cross_address.identifiers.push_back("Friend");
  movescape::Address other{};
  other.back() = 1;
  cross_address.addresses.push_back(other);
  cross_address.friends.push_back({.address = 1, .name = 2});
  REQUIRE_ERROR(movescape::validateModule(cross_address), movescape::ErrorCode::InvalidIndex);

  auto self_friend = validModule();
  self_friend.friends.push_back(self_friend.module_handles[0]);
  REQUIRE_ERROR(movescape::validateModule(self_friend), movescape::ErrorCode::InvalidIndex);
}

TEST(global_resources_require_key_and_exact_acquires_annotations) {
  auto valid = globalBorrowModule(true, true);
  movescape::validateModule(valid);

  auto missing_key = globalBorrowModule(false, true);
  REQUIRE_ERROR(movescape::validateModule(missing_key), movescape::ErrorCode::InvalidIndex);

  auto missing_acquires = globalBorrowModule(true, false);
  REQUIRE_ERROR(movescape::validateModule(missing_acquires), movescape::ErrorCode::InvalidIndex);

  auto duplicate_acquires = globalBorrowModule(true, true);
  duplicate_acquires.function_definitions[0].acquires.push_back(0);
  REQUIRE_ERROR(movescape::validateModule(duplicate_acquires), movescape::ErrorCode::InvalidIndex);
}

TEST(same_module_calls_propagate_required_acquires_annotations) {
  auto module = globalBorrowModule(true, true);
  module.identifiers.push_back("caller");
  module.function_handles.push_back({
      .module = 0,
      .name = 3,
      .parameters = 1,
      .returns = 0,
  });
  module.function_definitions.push_back({
      .handle = 1,
      .visibility = movescape::Visibility::Public,
      .code =
          movescape::CodeUnit{
              .locals = 0,
              .code =
                  {
                      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
                      {.opcode = movescape::Opcode::Call, .operands = {0}},
                      {.opcode = movescape::Opcode::Ret},
                  },
          },
  });
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidIndex);

  module.function_definitions[1].acquires = {0};
  movescape::validateModule(module);
}

TEST(generic_calls_enforce_type_argument_ability_constraints) {
  auto module = validModule();
  module.identifiers.insert(module.identifiers.end(), {"Dependency", "g"});
  movescape::Address dependency_address{};
  dependency_address.back() = 2;
  module.addresses.push_back(dependency_address);
  module.module_handles.push_back({.address = 1, .name = 2});
  module.function_handles.push_back({
      .module = 1,
      .name = 3,
      .parameters = 0,
      .returns = 0,
      .type_parameters =
          {
              movescape::AbilitySet{movescape::AbilitySet::Key},
          },
  });
  module.signatures.push_back({movescape::Type{.kind = movescape::TypeKind::U8}});
  module.function_instantiations.push_back({.handle = 1, .type_parameters = 1});
  module.function_definitions[0].code->code = {
      {.opcode = movescape::Opcode::CallGeneric, .operands = {0}},
      {.opcode = movescape::Opcode::Ret},
  };

  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::TypeMismatch);
}

TEST(generic_instantiations_reject_out_of_scope_type_parameters) {
  auto module = validModule();
  module.identifiers.insert(module.identifiers.end(), {"Dependency", "g"});
  movescape::Address dependency_address{};
  dependency_address.back() = 2;
  module.addresses.push_back(dependency_address);
  module.module_handles.push_back({.address = 1, .name = 2});
  module.function_handles.push_back({
      .module = 1,
      .name = 3,
      .parameters = 0,
      .returns = 0,
      .type_parameters = {movescape::AbilitySet{}},
  });
  module.signatures.push_back({movescape::Type{
      .kind = movescape::TypeKind::TypeParameter,
      .index = 0,
  }});
  module.function_instantiations.push_back({.handle = 1, .type_parameters = 1});
  module.function_definitions[0].code->code = {
      {.opcode = movescape::Opcode::CallGeneric, .operands = {0}},
      {.opcode = movescape::Opcode::Ret},
  };

  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::TypeMismatch);
}

TEST(reject_invalid_unary_and_leaf_type_shapes) {
  auto unary = validModule();
  unary.signatures.push_back({
      movescape::Type{.kind = movescape::TypeKind::Vector},
  });
  REQUIRE_ERROR(movescape::validateModule(unary), movescape::ErrorCode::InvalidIndex);

  auto leaf = validModule();
  movescape::Type invalid_leaf{.kind = movescape::TypeKind::U8};
  invalid_leaf.arguments.push_back(movescape::Type{.kind = movescape::TypeKind::Bool});
  leaf.signatures.push_back({invalid_leaf});
  REQUIRE_ERROR(movescape::validateModule(leaf), movescape::ErrorCode::InvalidIndex);
}

TEST(validate_exact_struct_type_arity) {
  auto valid = validModule();
  addStructHandle(valid, 1);
  movescape::Type instantiated{
      .kind = movescape::TypeKind::StructInstantiation,
      .index = 0,
  };
  instantiated.arguments.push_back(movescape::Type{.kind = movescape::TypeKind::U8});
  valid.signatures.push_back({instantiated});
  valid.struct_definitions.push_back({
      .handle = 0,
      .field_kind = movescape::StructFieldKind::Native,
  });
  movescape::validateModule(valid);

  auto missing_arguments = validModule();
  addStructHandle(missing_arguments, 1);
  missing_arguments.signatures.push_back({
      movescape::Type{.kind = movescape::TypeKind::Struct, .index = 0},
  });
  REQUIRE_ERROR(movescape::validateModule(missing_arguments), movescape::ErrorCode::InvalidIndex);

  auto wrong_instantiation = validModule();
  addStructHandle(wrong_instantiation, 1);
  wrong_instantiation.signatures.push_back({
      movescape::Type{
          .kind = movescape::TypeKind::StructInstantiation,
          .index = 0,
      },
  });
  REQUIRE_ERROR(movescape::validateModule(wrong_instantiation), movescape::ErrorCode::InvalidIndex);
}

TEST(reject_more_than_255_parameters_and_locals) {
  auto module = validModule();
  const movescape::Type u8{.kind = movescape::TypeKind::U8};
  module.signatures.push_back(movescape::Signature(255, u8));
  module.signatures.push_back(movescape::Signature(1, u8));
  module.function_handles[0].parameters = 1;
  module.function_definitions[0].code->locals = 2;
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidIndex);
}

TEST(reject_empty_enum_and_variant_field_sets) {
  auto empty_enum = validModule();
  addStructHandle(empty_enum, 0);
  empty_enum.struct_definitions.push_back({
      .handle = 0,
      .field_kind = movescape::StructFieldKind::Variants,
  });
  REQUIRE_ERROR(movescape::validateModule(empty_enum), movescape::ErrorCode::InvalidIndex);

  auto empty_variant_set = validModule();
  addStructHandle(empty_variant_set, 0);
  empty_variant_set.identifiers.insert(empty_variant_set.identifiers.end(), {"V", "field"});
  empty_variant_set.struct_definitions.push_back({
      .handle = 0,
      .field_kind = movescape::StructFieldKind::Variants,
      .variants = {{
          .name = 3,
          .fields = {{
              .name = 4,
              .type = movescape::Type{.kind = movescape::TypeKind::U8},
          }},
      }},
  });
  empty_variant_set.variant_field_handles.push_back({
      .owner = 0,
      .variants = {},
      .field = 0,
  });
  REQUIRE_ERROR(movescape::validateModule(empty_variant_set), movescape::ErrorCode::InvalidIndex);
}

TEST(reject_inconsistent_and_duplicate_variant_field_handles) {
  auto module = validModule();
  addStructHandle(module, 0);
  module.identifiers.insert(module.identifiers.end(), {"V0", "V1", "x", "y"});
  module.struct_definitions.push_back({
      .handle = 0,
      .field_kind = movescape::StructFieldKind::Variants,
      .variants =
          {
              {
                  .name = 3,
                  .fields = {{
                      .name = 5,
                      .type = movescape::Type{.kind = movescape::TypeKind::U8},
                  }},
              },
              {
                  .name = 4,
                  .fields = {{
                      .name = 6,
                      .type = movescape::Type{.kind = movescape::TypeKind::U64},
                  }},
              },
          },
  });
  module.variant_field_handles.push_back({
      .owner = 0,
      .variants = {0, 1},
      .field = 0,
  });
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidIndex);

  module.variant_field_handles[0].variants = {0, 0};
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidIndex);
}

TEST(reject_struct_variant_handle_for_non_enum_definition) {
  auto module = validModule();
  addStructHandle(module, 0);
  module.struct_definitions.push_back({
      .handle = 0,
      .field_kind = movescape::StructFieldKind::Declared,
  });
  module.struct_variant_handles.push_back({
      .definition = 0,
      .variant = 0,
  });

  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidIndex);
}

TEST(native_function_without_code_is_structurally_valid) {
  auto module = validModule();
  module.function_definitions[0].code.reset();
  movescape::validateModule(module);
}
