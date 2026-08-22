#include "test.hpp"

#include "movescape/module.hpp"
#include "movescape/validator.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] movescape::Type type(movescape::TypeKind kind) { return movescape::Type{.kind = kind}; }

[[nodiscard]] movescape::Type reference(movescape::TypeKind kind, movescape::Type inner) {
  movescape::Type result{.kind = kind};
  result.arguments.push_back(std::move(inner));
  return result;
}

movescape::Module borrowModule(movescape::Signature parameters, movescape::Signature returns, movescape::Signature locals,
                              std::vector<movescape::Instruction> code) {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"M", "f"};
  module.addresses.push_back({});
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  const auto internSignature = [&](movescape::Signature signature) {
    const auto existing = std::find(module.signatures.begin(), module.signatures.end(), signature);
    if (existing != module.signatures.end()) {
      return static_cast<movescape::TableIndex>(std::distance(module.signatures.begin(), existing));
    }
    const auto index = static_cast<movescape::TableIndex>(module.signatures.size());
    module.signatures.push_back(std::move(signature));
    return index;
  };
  const auto empty_signature = internSignature({});
  const auto parameter_signature = internSignature(std::move(parameters));
  const auto return_signature = internSignature(std::move(returns));
  const auto locals_signature = internSignature(std::move(locals));
  module.function_handles.push_back({
      .module = 0,
      .name = 1,
      .parameters = parameter_signature,
      .returns = return_signature,
  });
  module.function_definitions.push_back({
      .handle = 0,
      .visibility = movescape::Visibility::Public,
      .code = movescape::CodeUnit{.locals = locals_signature, .code = std::move(code)},
  });
  (void)empty_signature;
  return module;
}

movescape::Module twoFieldModule(std::vector<movescape::Instruction> code) {
  auto structure = type(movescape::TypeKind::Struct);
  structure.index = 0;
  auto module = borrowModule({reference(movescape::TypeKind::MutableReference, structure)}, {},
                             {reference(movescape::TypeKind::MutableReference, type(movescape::TypeKind::U64)),
                              reference(movescape::TypeKind::MutableReference, type(movescape::TypeKind::U64))},
                             std::move(code));
  module.identifiers.insert(module.identifiers.end(), {"S", "first", "second"});
  module.struct_handles.push_back({
      .module = 0,
      .name = 2,
      .abilities = movescape::AbilitySet{static_cast<std::uint8_t>(movescape::AbilitySet::Copy | movescape::AbilitySet::Drop | movescape::AbilitySet::Store)},
  });
  module.struct_definitions.push_back({
      .handle = 0,
      .field_kind = movescape::StructFieldKind::Declared,
      .fields =
          {
              {.name = 3, .type = type(movescape::TypeKind::U64)},
              {.name = 4, .type = type(movescape::TypeKind::U64)},
          },
  });
  module.field_handles = {
      {.owner = 0, .field = 0},
      {.owner = 0, .field = 1},
  };
  return module;
}

movescape::Module globalResourceModule(std::vector<movescape::Instruction> code) {
  auto resource = type(movescape::TypeKind::Struct);
  resource.index = 0;
  auto module = borrowModule({type(movescape::TypeKind::Address)}, {}, {reference(movescape::TypeKind::Reference, resource)}, std::move(code));
  module.identifiers.push_back("Resource");
  module.struct_handles.push_back({
      .module = 0,
      .name = 2,
      .abilities = movescape::AbilitySet{static_cast<std::uint8_t>(movescape::AbilitySet::Key | movescape::AbilitySet::Drop)},
  });
  module.struct_definitions.push_back({
      .handle = 0,
      .field_kind = movescape::StructFieldKind::Native,
  });
  module.function_definitions[0].acquires = {0};
  return module;
}

} // namespace

TEST(borrow_safety_allows_reading_an_immutable_local_borrow) {
  const auto module = borrowModule({type(movescape::TypeKind::U64)}, {}, {},
                                   {
                                       {.opcode = movescape::Opcode::ImmBorrowLoc, .operands = {0}},
                                       {.opcode = movescape::Opcode::ReadRef},
                                       {.opcode = movescape::Opcode::Pop},
                                       {.opcode = movescape::Opcode::Ret},
                                   });
  movescape::validateModule(module);
}

TEST(borrow_safety_rejects_moving_a_borrowed_local) {
  const auto module = borrowModule({type(movescape::TypeKind::U64)}, {}, {reference(movescape::TypeKind::Reference, type(movescape::TypeKind::U64))},
                                   {
                                       {.opcode = movescape::Opcode::ImmBorrowLoc, .operands = {0}},
                                       {.opcode = movescape::Opcode::StLoc, .operands = {1}},
                                       {.opcode = movescape::Opcode::MoveLoc, .operands = {0}},
                                       {.opcode = movescape::Opcode::Pop},
                                       {.opcode = movescape::Opcode::Ret},
                                   });
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidBorrowState);
}

TEST(borrow_safety_rejects_overwriting_a_borrowed_local) {
  const auto module = borrowModule({type(movescape::TypeKind::U64)}, {}, {reference(movescape::TypeKind::Reference, type(movescape::TypeKind::U64))},
                                   {
                                       {.opcode = movescape::Opcode::ImmBorrowLoc, .operands = {0}},
                                       {.opcode = movescape::Opcode::StLoc, .operands = {1}},
                                       {.opcode = movescape::Opcode::LdU64, .operands = {9}},
                                       {.opcode = movescape::Opcode::StLoc, .operands = {0}},
                                       {.opcode = movescape::Opcode::Ret},
                                   });
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidBorrowState);
}

TEST(borrow_safety_rejects_copying_through_a_mutable_local_borrow) {
  const auto module = borrowModule({type(movescape::TypeKind::U64)}, {}, {reference(movescape::TypeKind::MutableReference, type(movescape::TypeKind::U64))},
                                   {
                                       {.opcode = movescape::Opcode::MutBorrowLoc, .operands = {0}},
                                       {.opcode = movescape::Opcode::StLoc, .operands = {1}},
                                       {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
                                       {.opcode = movescape::Opcode::Pop},
                                       {.opcode = movescape::Opcode::Ret},
                                   });
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidBorrowState);
}

TEST(borrow_safety_rejects_writing_through_an_aliased_mutable_reference) {
  const auto mutable_u64 = reference(movescape::TypeKind::MutableReference, type(movescape::TypeKind::U64));
  const auto module = borrowModule({mutable_u64}, {}, {mutable_u64},
                                   {
                                       {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
                                       {.opcode = movescape::Opcode::StLoc, .operands = {1}},
                                       {.opcode = movescape::Opcode::LdU64, .operands = {7}},
                                       {.opcode = movescape::Opcode::MoveLoc, .operands = {0}},
                                       {.opcode = movescape::Opcode::WriteRef},
                                       {.opcode = movescape::Opcode::Ret},
                                   });
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidBorrowState);
}

TEST(borrow_safety_allows_disjoint_mutable_field_borrows) {
  const auto module = twoFieldModule({
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::MutBorrowField, .operands = {0}},
      {.opcode = movescape::Opcode::StLoc, .operands = {1}},
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::MutBorrowField, .operands = {1}},
      {.opcode = movescape::Opcode::StLoc, .operands = {2}},
      {.opcode = movescape::Opcode::Ret},
  });
  movescape::validateModule(module);
}

TEST(borrow_safety_rejects_conflicting_global_borrows) {
  const auto module = globalResourceModule({
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::ImmBorrowGlobal, .operands = {0}},
      {.opcode = movescape::Opcode::StLoc, .operands = {1}},
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::MutBorrowGlobal, .operands = {0}},
      {.opcode = movescape::Opcode::Pop},
      {.opcode = movescape::Opcode::Ret},
  });
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidBorrowState);
}

TEST(borrow_safety_rejects_move_from_while_global_is_borrowed) {
  const auto module = globalResourceModule({
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::ImmBorrowGlobal, .operands = {0}},
      {.opcode = movescape::Opcode::StLoc, .operands = {1}},
      {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
      {.opcode = movescape::Opcode::MoveFrom, .operands = {0}},
      {.opcode = movescape::Opcode::Pop},
      {.opcode = movescape::Opcode::Ret},
  });
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidBorrowState);
}

TEST(borrow_safety_rejects_returning_a_reference_to_a_value_local) {
  const auto module = borrowModule({type(movescape::TypeKind::U64)}, {reference(movescape::TypeKind::Reference, type(movescape::TypeKind::U64))}, {},
                                   {
                                       {.opcode = movescape::Opcode::ImmBorrowLoc, .operands = {0}},
                                       {.opcode = movescape::Opcode::Ret},
                                   });
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidBorrowState);
}

TEST(borrow_safety_allows_returning_a_copy_of_a_reference_parameter) {
  const auto immutable_u64 = reference(movescape::TypeKind::Reference, type(movescape::TypeKind::U64));
  const auto module = borrowModule({immutable_u64}, {immutable_u64}, {},
                                   {
                                       {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
                                       {.opcode = movescape::Opcode::Ret},
                                   });
  movescape::validateModule(module);
}

TEST(borrow_safety_joins_reference_loans_across_a_diamond) {
  const auto module = borrowModule({type(movescape::TypeKind::Bool), type(movescape::TypeKind::U64)}, {},
                                   {reference(movescape::TypeKind::Reference, type(movescape::TypeKind::U64))},
                                   {
                                       {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
                                       {.opcode = movescape::Opcode::BrFalse, .operands = {5}},
                                       {.opcode = movescape::Opcode::ImmBorrowLoc, .operands = {1}},
                                       {.opcode = movescape::Opcode::StLoc, .operands = {2}},
                                       {.opcode = movescape::Opcode::Branch, .operands = {7}},
                                       {.opcode = movescape::Opcode::ImmBorrowLoc, .operands = {1}},
                                       {.opcode = movescape::Opcode::StLoc, .operands = {2}},
                                       {.opcode = movescape::Opcode::MoveLoc, .operands = {2}},
                                       {.opcode = movescape::Opcode::ReadRef},
                                       {.opcode = movescape::Opcode::Pop},
                                       {.opcode = movescape::Opcode::Ret},
                                   });
  movescape::validateModule(module);
}

TEST(borrow_safety_rejects_passing_an_aliased_mutable_reference_to_a_call) {
  const auto mutable_u64 = reference(movescape::TypeKind::MutableReference, type(movescape::TypeKind::U64));
  auto module = borrowModule({mutable_u64}, {}, {mutable_u64},
                             {
                                 {.opcode = movescape::Opcode::CopyLoc, .operands = {0}},
                                 {.opcode = movescape::Opcode::StLoc, .operands = {1}},
                                 {.opcode = movescape::Opcode::MoveLoc, .operands = {0}},
                                 {.opcode = movescape::Opcode::Call, .operands = {1}},
                                 {.opcode = movescape::Opcode::Ret},
                             });
  module.identifiers.insert(module.identifiers.end(), {"Dependency", "g"});
  movescape::Address dependency{};
  dependency.back() = 1;
  module.addresses.push_back(dependency);
  module.module_handles.push_back({.address = 1, .name = 2});
  module.function_handles.push_back({
      .module = 1,
      .name = 3,
      .parameters = module.function_handles[0].parameters,
      .returns = module.function_handles[0].returns,
  });
  REQUIRE_ERROR(movescape::validateModule(module), movescape::ErrorCode::InvalidBorrowState);
}
