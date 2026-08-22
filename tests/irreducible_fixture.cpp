#include "movescape/error.hpp"
#include "movescape/move_emitter.hpp"

#include <iostream>

int main() {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"IrreducibleFixture", "run"};
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

  try {
    std::cout << movescape::emitMoveModule(module).source;
  } catch (const movescape::Error &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
