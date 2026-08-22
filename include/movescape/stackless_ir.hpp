#pragma once

#include "movescape/cfg.hpp"
#include "movescape/module.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace movescape {

struct StackEffect {
  std::size_t pops = 0;
  std::size_t pushes = 0;

  friend bool operator==(const StackEffect &, const StackEffect &) = default;
};

using ValueId = std::uint32_t;

struct StacklessInstruction {
  std::size_t bytecode_index = 0;
  Opcode opcode = Opcode::Nop;
  std::vector<ValueId> inputs;
  std::vector<ValueId> outputs;
  std::vector<std::uint64_t> immediate_operands;
  std::vector<std::uint8_t> wide_immediate;
};

struct StacklessBlock {
  BlockId id = 0;
  std::vector<StacklessInstruction> instructions;
};

struct StacklessFunction {
  std::vector<StacklessBlock> blocks;
  std::vector<Type> value_types;
  std::size_t value_count = 0;
  std::size_t maximum_stack_height = 0;
};

[[nodiscard]] StackEffect instructionStackEffect(const Module &module, const FunctionDefinition &function, const Instruction &instruction);
void validateStackUsage(const Module &module, const FunctionDefinition &function, const ControlFlowGraph &graph);
[[nodiscard]] StacklessFunction liftToStackless(const Module &module, const FunctionDefinition &function, const ControlFlowGraph &graph);
[[nodiscard]] std::string formatStacklessFunction(const Module &module, const StacklessFunction &function);

} // namespace movescape
