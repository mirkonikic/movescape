#include "test.hpp"

#include "movescape/disassembler.hpp"

#include <string>

TEST(render_addresses_without_losing_internal_zeroes) {
  movescape::Address address{};
  address[29] = 0x01;
  address[30] = 0x00;
  address[31] = 0x02;
  REQUIRE_EQ(movescape::renderAddress(address), std::string("0x10002"));
}

TEST(render_nested_move_type) {
  movescape::Module module;
  movescape::Type u64;
  u64.kind = movescape::TypeKind::U64;
  movescape::Type reference;
  reference.kind = movescape::TypeKind::MutableReference;
  reference.arguments.push_back(u64);
  movescape::Type vector;
  vector.kind = movescape::TypeKind::Vector;
  vector.arguments.push_back(reference);
  REQUIRE_EQ(movescape::renderType(module, vector), std::string("vector<&mut u64>"));
}

TEST(render_function_type_with_move_syntax) {
  movescape::Module module;
  movescape::Type function;
  function.kind = movescape::TypeKind::Function;
  function.abilities.bits = movescape::AbilitySet::Copy | movescape::AbilitySet::Drop;
  function.arguments = {
      movescape::Type{.kind = movescape::TypeKind::U64},
      movescape::Type{.kind = movescape::TypeKind::Bool},
  };
  function.results = {
      movescape::Type{.kind = movescape::TypeKind::U8},
  };
  REQUIRE_EQ(movescape::renderType(module, function), std::string("|u64, bool|(u8) has copy + drop"));
}

TEST(render_internal_function_names_as_valid_source_names) {
  movescape::Module module;
  module.identifiers = {"M", "__lambda__1__f"};
  module.addresses.push_back({});
  module.module_handles.push_back({.address = 0, .name = 0});
  module.function_handles.push_back({
      .module = 0,
      .name = 1,
  });
  REQUIRE_EQ(movescape::renderSourceFunctionIdentifier(module, 1), std::string("__lambda__1__f"));
  REQUIRE_EQ(movescape::renderSourceFunctionName(module, 0), std::string("0x0::M::__lambda__1__f"));
}

TEST(render_branch_instruction) {
  movescape::Module module;
  movescape::Instruction branch;
  branch.opcode = movescape::Opcode::BrFalse;
  branch.operands.push_back(42);
  REQUIRE_EQ(movescape::renderInstruction(module, branch, 7), std::string("0007: BrFalse @42"));
}
