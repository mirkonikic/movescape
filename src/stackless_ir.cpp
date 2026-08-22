#include "movescape/stackless_ir.hpp"

#include "movescape/disassembler.hpp"
#include "movescape/error.hpp"
#include "movescape/opcode.hpp"
#include "movescape/type_analysis.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <sstream>
#include <string>

namespace movescape {

namespace {

[[noreturn]] void invalidStack(std::string message) { throw Error(ErrorCode::Malformed, Error::UnknownOffset, std::move(message)); }

[[nodiscard]] std::size_t firstOperand(const Instruction &instruction) {
  if (instruction.operands.empty()) {
    invalidStack("stack effect requires a missing instruction operand");
  }
  return static_cast<std::size_t>(instruction.operands.front());
}

[[nodiscard]] const StructDefinition &structDefinitionFor(const Module &module, const Instruction &instruction, bool generic) {
  if (!generic) {
    return module.struct_definitions.at(firstOperand(instruction));
  }
  const auto &instantiation = module.struct_definition_instantiations.at(firstOperand(instruction));
  return module.struct_definitions.at(instantiation.definition);
}

[[nodiscard]] const StructVariantHandle &variantHandleFor(const Module &module, const Instruction &instruction, bool generic) {
  if (!generic) {
    return module.struct_variant_handles.at(firstOperand(instruction));
  }
  const auto &instantiation = module.struct_variant_instantiations.at(firstOperand(instruction));
  return module.struct_variant_handles.at(instantiation.handle);
}

[[nodiscard]] const FunctionHandle &functionHandleFor(const Module &module, const Instruction &instruction, bool generic) {
  if (!generic) {
    return module.function_handles.at(firstOperand(instruction));
  }
  const auto &instantiation = module.function_instantiations.at(firstOperand(instruction));
  return module.function_handles.at(instantiation.handle);
}

[[nodiscard]] std::size_t variantFieldCount(const Module &module, const Instruction &instruction, bool generic) {
  const auto &handle = variantHandleFor(module, instruction, generic);
  const auto &definition = module.struct_definitions.at(handle.definition);
  return definition.variants.at(handle.variant).fields.size();
}

[[nodiscard]] StackEffect unary() { return {.pops = 1, .pushes = 1}; }
[[nodiscard]] StackEffect binary() { return {.pops = 2, .pushes = 1}; }

} // namespace

StackEffect instructionStackEffect(const Module &module, const FunctionDefinition &function, const Instruction &instruction) {
  switch (instruction.opcode) {
  case Opcode::Pop:
  case Opcode::BrTrue:
  case Opcode::BrFalse:
  case Opcode::StLoc:
  case Opcode::Abort:
    return {.pops = 1, .pushes = 0};

  case Opcode::LdU8:
  case Opcode::LdU16:
  case Opcode::LdU32:
  case Opcode::LdU64:
  case Opcode::LdU128:
  case Opcode::LdU256:
  case Opcode::LdI8:
  case Opcode::LdI16:
  case Opcode::LdI32:
  case Opcode::LdI64:
  case Opcode::LdI128:
  case Opcode::LdI256:
  case Opcode::LdTrue:
  case Opcode::LdFalse:
  case Opcode::LdConst:
  case Opcode::CopyLoc:
  case Opcode::MoveLoc:
  case Opcode::MutBorrowLoc:
  case Opcode::ImmBorrowLoc:
    return {.pops = 0, .pushes = 1};

  case Opcode::Not:
  case Opcode::Negate:
  case Opcode::FreezeRef:
  case Opcode::ReadRef:
  case Opcode::Exists:
  case Opcode::ExistsGeneric:
  case Opcode::MutBorrowGlobal:
  case Opcode::MutBorrowGlobalGeneric:
  case Opcode::ImmBorrowGlobal:
  case Opcode::ImmBorrowGlobalGeneric:
  case Opcode::MutBorrowField:
  case Opcode::MutBorrowFieldGeneric:
  case Opcode::ImmBorrowField:
  case Opcode::ImmBorrowFieldGeneric:
  case Opcode::MutBorrowVariantField:
  case Opcode::MutBorrowVariantFieldGeneric:
  case Opcode::ImmBorrowVariantField:
  case Opcode::ImmBorrowVariantFieldGeneric:
  case Opcode::TestVariant:
  case Opcode::TestVariantGeneric:
  case Opcode::MoveFrom:
  case Opcode::MoveFromGeneric:
  case Opcode::CastU8:
  case Opcode::CastU16:
  case Opcode::CastU32:
  case Opcode::CastU64:
  case Opcode::CastU128:
  case Opcode::CastU256:
  case Opcode::CastI8:
  case Opcode::CastI16:
  case Opcode::CastI32:
  case Opcode::CastI64:
  case Opcode::CastI128:
  case Opcode::CastI256:
  case Opcode::VecLen:
  case Opcode::VecPopBack:
    return unary();

  case Opcode::Add:
  case Opcode::Sub:
  case Opcode::Mul:
  case Opcode::Mod:
  case Opcode::Div:
  case Opcode::BitOr:
  case Opcode::BitAnd:
  case Opcode::Xor:
  case Opcode::Shl:
  case Opcode::Shr:
  case Opcode::Or:
  case Opcode::And:
  case Opcode::Eq:
  case Opcode::Neq:
  case Opcode::Lt:
  case Opcode::Gt:
  case Opcode::Le:
  case Opcode::Ge:
  case Opcode::VecImmBorrow:
  case Opcode::VecMutBorrow:
    return binary();

  case Opcode::MoveTo:
  case Opcode::MoveToGeneric:
  case Opcode::WriteRef:
  case Opcode::VecPushBack:
  case Opcode::AbortMsg:
    return {.pops = 2, .pushes = 0};

  case Opcode::VecSwap:
    return {.pops = 3, .pushes = 0};

  case Opcode::Branch:
  case Opcode::Nop:
    return {.pops = 0, .pushes = 0};

  case Opcode::Ret: {
    const auto &handle = module.function_handles.at(function.handle);
    return {.pops = module.signatures.at(handle.returns).size(), .pushes = 0};
  }

  case Opcode::Call:
  case Opcode::CallGeneric: {
    const auto &handle = functionHandleFor(module, instruction, instruction.opcode == Opcode::CallGeneric);
    return {
        .pops = module.signatures.at(handle.parameters).size(),
        .pushes = module.signatures.at(handle.returns).size(),
    };
  }

  case Opcode::CallClosure: {
    const auto &signature = module.signatures.at(firstOperand(instruction));
    if (signature.size() != 1 || signature.front().kind != TypeKind::Function) {
      invalidStack("CallClosure signature is not one function type");
    }
    return {
        .pops = signature.front().arguments.size() + 1,
        .pushes = signature.front().results.size(),
    };
  }

  case Opcode::PackClosure:
  case Opcode::PackClosureGeneric:
    if (instruction.operands.size() < 2) {
      invalidStack("PackClosure has no capture mask");
    }
    return {
        .pops = static_cast<std::size_t>(std::popcount(instruction.operands[1])),
        .pushes = 1,
    };

  case Opcode::Pack:
  case Opcode::PackGeneric: {
    const auto &definition = structDefinitionFor(module, instruction, instruction.opcode == Opcode::PackGeneric);
    return {.pops = definition.fields.size(), .pushes = 1};
  }

  case Opcode::Unpack:
  case Opcode::UnpackGeneric: {
    const auto &definition = structDefinitionFor(module, instruction, instruction.opcode == Opcode::UnpackGeneric);
    return {.pops = 1, .pushes = definition.fields.size()};
  }

  case Opcode::PackVariant:
  case Opcode::PackVariantGeneric:
    return {
        .pops = variantFieldCount(module, instruction, instruction.opcode == Opcode::PackVariantGeneric),
        .pushes = 1,
    };

  case Opcode::UnpackVariant:
  case Opcode::UnpackVariantGeneric:
    return {
        .pops = 1,
        .pushes = variantFieldCount(module, instruction, instruction.opcode == Opcode::UnpackVariantGeneric),
    };

  case Opcode::VecPack:
    if (instruction.operands.size() < 2 || instruction.operands[1] > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      invalidStack("VecPack count is missing or too large");
    }
    return {.pops = static_cast<std::size_t>(instruction.operands[1]), .pushes = 1};

  case Opcode::VecUnpack:
    if (instruction.operands.size() < 2 || instruction.operands[1] > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      invalidStack("VecUnpack count is missing or too large");
    }
    return {.pops = 1, .pushes = static_cast<std::size_t>(instruction.operands[1])};
  }
  invalidStack("opcode has no stack-effect definition");
}

void validateStackUsage(const Module &module, const FunctionDefinition &function, const ControlFlowGraph &graph) {
  if (!function.code.has_value()) {
    return;
  }
  const auto &unit = *function.code;
  for (const auto &block : graph.blocks) {
    std::size_t height = 0;
    for (std::size_t index = block.begin; index < block.end; ++index) {
      const auto effect = instructionStackEffect(module, function, unit.code[index]);
      if (effect.pops > height) {
        std::ostringstream out;
        out << "operand stack underflow in bb" << block.id << " at instruction " << index;
        invalidStack(out.str());
      }
      height -= effect.pops;
      if (effect.pushes > std::numeric_limits<std::size_t>::max() - height) {
        invalidStack("operand stack height overflow");
      }
      height += effect.pushes;
    }
    if (height != 0) {
      std::ostringstream out;
      out << "operand stack height is " << height << " at the end of bb" << block.id << ", expected zero";
      invalidStack(out.str());
    }
  }
}

StacklessFunction liftToStackless(const Module &module, const FunctionDefinition &function, const ControlFlowGraph &graph) {
  validateStackUsage(module, function, graph);
  if (!function.code.has_value()) { return {}; }
  
  // current function we are lifting
  const auto &unit = *function.code;

  // prepping the output
  StacklessFunction result;
  std::uint64_t next_value = 0;
  result.blocks.reserve(graph.blocks.size());

  for (const auto &block : graph.blocks) {
    StacklessBlock lifted{.id = block.id, .instructions = {}};
    std::vector<ValueId> stack;
    // go instruction by instruction and translate to stackless IR
    for (std::size_t index = block.begin; index < block.end; ++index) {
      const auto &instruction = unit.code[index];
      const auto effect = instructionStackEffect(module, function, instruction);
      StacklessInstruction operation{
          .bytecode_index = index,
          .opcode = instruction.opcode,
          .inputs = {},
          .outputs = {},
          .immediate_operands = instruction.operands,
          .wide_immediate = instruction.wide_operand,
      };
      operation.inputs.insert(operation.inputs.end(), stack.end() - static_cast<std::ptrdiff_t>(effect.pops), stack.end());
      stack.resize(stack.size() - effect.pops);
      for (std::size_t output = 0; output < effect.pushes; ++output) {
        if (next_value > static_cast<std::uint64_t>(std::numeric_limits<ValueId>::max())) {
          invalidStack("stackless value identifier overflow");
        }
        const auto value = static_cast<ValueId>(next_value++);
        operation.outputs.push_back(value);
        stack.push_back(value);
      }
      result.maximum_stack_height = std::max(result.maximum_stack_height, stack.size());
      lifted.instructions.push_back(std::move(operation));
    }
    result.blocks.push_back(std::move(lifted));
  }
  result.value_count = static_cast<std::size_t>(next_value);
  inferAndValidateStacklessTypes(module, function, result);
  return result;
}

std::string formatStacklessFunction(const Module &module, const StacklessFunction &function) {
  std::ostringstream out;
  out << "values: " << function.value_count << "\nmaximum-stack-height: " << function.maximum_stack_height << '\n';
  for (const auto &block : function.blocks) {
    out << "\nbb" << block.id << ":\n";
    for (const auto &instruction : block.instructions) {
      out << "  ";
      for (std::size_t index = 0; index < instruction.outputs.size(); ++index) {
        if (index != 0) {
          out << ", ";
        }
        const auto value = instruction.outputs[index];
        out << 'v' << value << ": " << renderType(module, function.value_types.at(value));
      }
      if (!instruction.outputs.empty()) {
        out << " = ";
      }
      out << opcodeInfo(instruction.opcode).name;
      if (!instruction.inputs.empty()) {
        out << " (";
        for (std::size_t index = 0; index < instruction.inputs.size(); ++index) {
          if (index != 0) {
            out << ", ";
          }
          out << 'v' << instruction.inputs[index];
        }
        out << ')';
      }
      if (!instruction.immediate_operands.empty() || !instruction.wide_immediate.empty()) {
        Instruction original{
            .opcode = instruction.opcode,
            .operands = instruction.immediate_operands,
            .wide_operand = instruction.wide_immediate,
        };
        const auto rendered = renderInstruction(module, original, instruction.bytecode_index);
        const auto colon = rendered.find(": ");
        const auto operation = colon == std::string::npos ? rendered : rendered.substr(colon + 2);
        const auto space = operation.find(' ');
        if (space != std::string::npos) {
          out << " [" << operation.substr(space + 1) << ']';
        }
      }
      out << "  ; @" << instruction.bytecode_index << '\n';
    }
  }
  return out.str();
}

} // namespace movescape
