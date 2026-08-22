#include "test.hpp"

#include "movescape/module.hpp"
#include "movescape/validator.hpp"

#include <cstdint>
#include <utility>

namespace {

movescape::Module canonicalModule() {
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
  module.function_definitions.push_back({
      .handle = 0,
      .visibility = movescape::Visibility::Public,
      .code =
          movescape::CodeUnit{
              .locals = 0,
              .code = {{.opcode = movescape::Opcode::Ret}},
          },
  });
  return module;
}

movescape::Type unary(movescape::TypeKind kind, movescape::Type argument) {
  movescape::Type result{.kind = kind};
  result.arguments.push_back(std::move(argument));
  return result;
}

} // namespace

TEST(verifier_parity_accepts_canonical_constant_bcs) {
  auto module = canonicalModule();
  module.constants.push_back({
      .type = movescape::Type{.kind = movescape::TypeKind::U64},
      .data = {0x78, 0x56, 0x34, 0x12, 0, 0, 0, 0},
  });
  movescape::validateModule(module);
}

TEST(verifier_parity_rejects_duplicate_canonical_pool_entries) {
  auto module = canonicalModule();
  module.identifiers.push_back("f");
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidIndex);

  module = canonicalModule();
  module.signatures.push_back({});
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidIndex);
}

TEST(verifier_parity_rejects_invalid_constant_types_and_bcs) {
  auto malformed = canonicalModule();
  malformed.constants.push_back({
      .type = movescape::Type{.kind = movescape::TypeKind::U64},
      .data = {1, 2, 3},
  });
  REQUIRE_ERROR(movescape::validateModule(malformed), movescape::ErrorCode::InvalidIndex);

  auto invalid_type = canonicalModule();
  invalid_type.constants.push_back({
      .type = movescape::Type{.kind = movescape::TypeKind::Signer},
      .data = {},
  });
  REQUIRE_ERROR(movescape::validateModule(invalid_type), movescape::ErrorCode::InvalidIndex);
}

TEST(verifier_parity_rejects_nested_references) {
  auto module = canonicalModule();
  module.signatures.push_back({unary(movescape::TypeKind::Vector, unary(movescape::TypeKind::Reference, movescape::Type{.kind = movescape::TypeKind::U8}))});
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidIndex);
}

TEST(verifier_parity_rejects_missing_field_abilities) {
  auto module = canonicalModule();
  module.identifiers.insert(module.identifiers.end(), {"S", "value"});
  module.struct_handles.push_back({
      .module = 0,
      .name = 2,
      .abilities = movescape::AbilitySet{movescape::AbilitySet::Store},
  });
  module.struct_definitions.push_back({
      .handle = 0,
      .field_kind = movescape::StructFieldKind::Declared,
      .fields = {{
          .name = 3,
          .type = movescape::Type{.kind = movescape::TypeKind::Signer},
      }},
  });
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::TypeMismatch);
}

TEST(verifier_parity_rejects_phantom_parameters_in_real_fields) {
  auto module = canonicalModule();
  module.identifiers.insert(module.identifiers.end(), {"S", "value"});
  module.struct_handles.push_back({
      .module = 0,
      .name = 2,
      .type_parameters = {{.constraints = {}, .is_phantom = true}},
  });
  module.struct_definitions.push_back({
      .handle = 0,
      .field_kind = movescape::StructFieldKind::Declared,
      .fields = {{
          .name = 3,
          .type =
              movescape::Type{
                  .kind = movescape::TypeKind::TypeParameter,
                  .index = 0,
              },
      }},
  });
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidIndex);
}

TEST(verifier_parity_rejects_recursive_struct_definitions) {
  auto module = canonicalModule();
  module.identifiers.insert(module.identifiers.end(), {"S", "next"});
  module.struct_handles.push_back({.module = 0, .name = 2});
  module.struct_definitions.push_back({
      .handle = 0,
      .field_kind = movescape::StructFieldKind::Declared,
      .fields = {{
          .name = 3,
          .type =
              movescape::Type{
                  .kind = movescape::TypeKind::Struct,
                  .index = 0,
              },
      }},
  });
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidIndex);
}

TEST(verifier_parity_rejects_expanding_generic_call_cycles) {
  auto module = canonicalModule();
  module.function_handles[0].type_parameters.push_back({});
  module.signatures.push_back({unary(movescape::TypeKind::Vector, movescape::Type{
                                                                     .kind = movescape::TypeKind::TypeParameter,
                                                                     .index = 0,
                                                                 })});
  module.function_instantiations.push_back({
      .handle = 0,
      .type_parameters = 1,
  });
  module.function_definitions[0].code->code = {
      {.opcode = movescape::Opcode::CallGeneric, .operands = {0}},
      {.opcode = movescape::Opcode::Ret},
  };
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidIndex);
}

TEST(verifier_parity_rejects_generic_member_opcode_mismatches) {
  auto module = canonicalModule();
  module.identifiers.insert(module.identifiers.end(), {"Dependency", "g"});
  movescape::Address dependency{};
  dependency.back() = 1;
  module.addresses.push_back(dependency);
  module.module_handles.push_back({.address = 1, .name = 2});
  module.function_handles.push_back({
      .module = 1,
      .name = 3,
      .parameters = 0,
      .returns = 0,
      .type_parameters = {{}},
  });
  module.function_definitions[0].code->code = {
      {.opcode = movescape::Opcode::Call, .operands = {1}},
      {.opcode = movescape::Opcode::Ret},
  };
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidIndex);
}

TEST(verifier_parity_rejects_unimplemented_self_handles) {
  auto module = canonicalModule();
  module.identifiers.push_back("missing");
  module.function_handles.push_back({
      .module = 0,
      .name = 2,
      .parameters = 0,
      .returns = 0,
  });
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidIndex);
}
