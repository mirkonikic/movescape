#include "movescape/disassembler.hpp"

#include "movescape/error.hpp"
#include "movescape/opcode.hpp"
#include "movescape/source_names.hpp"
#include "movescape/validator.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <iomanip>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace movescape {

namespace {

[[nodiscard]] std::string identifier(const Module &module, TableIndex index) {
  if (index >= module.identifiers.size()) {
    return "<bad-ident:" + std::to_string(index) + ">";
  }
  return module.identifiers[index];
}

[[nodiscard]] std::string moduleName(const Module &module, TableIndex index) {
  if (index >= module.module_handles.size()) {
    return "<bad-module:" + std::to_string(index) + ">";
  }
  const auto &handle = module.module_handles[index];
  if (handle.address >= module.addresses.size()) {
    return "<bad-address>::" + identifier(module, handle.name);
  }
  return renderAddress(module.addresses[handle.address]) + "::" + identifier(module, handle.name);
}

[[nodiscard]] std::string structName(const Module &module, TableIndex index) {
  if (index >= module.struct_handles.size()) {
    return "<bad-struct:" + std::to_string(index) + ">";
  }
  const auto &handle = module.struct_handles[index];
  return moduleName(module, handle.module) + "::" + identifier(module, handle.name);
}

[[nodiscard]] std::string functionName(const Module &module, TableIndex index) {
  if (index >= module.function_handles.size()) {
    return "<bad-function:" + std::to_string(index) + ">";
  }
  const auto &handle = module.function_handles[index];
  return moduleName(module, handle.module) + "::" + identifier(module, handle.name);
}

[[nodiscard]] std::string join(const std::vector<std::string> &items, std::string_view separator) {
  std::ostringstream out;
  for (std::size_t index = 0; index < items.size(); ++index) {
    if (index != 0) {
      out << separator;
    }
    out << items[index];
  }
  return out.str();
}

[[nodiscard]] std::string renderTypes(const Module &module, const Signature &signature) {
  std::vector<std::string> parts;
  parts.reserve(signature.size());
  for (const auto &type : signature) {
    parts.push_back(renderType(module, type));
  }
  return join(parts, ", ");
}

[[nodiscard]] std::string abilities(AbilitySet set) {
  std::vector<std::string> result;
  if (set.has(AbilitySet::Copy)) {
    result.emplace_back("copy");
  }
  if (set.has(AbilitySet::Drop)) {
    result.emplace_back("drop");
  }
  if (set.has(AbilitySet::Store)) {
    result.emplace_back("store");
  }
  if (set.has(AbilitySet::Key)) {
    result.emplace_back("key");
  }
  return join(result, " + ");
}

[[nodiscard]] std::string wideHex(std::span<const std::uint8_t> bytes) {
  std::ostringstream out;
  out << "0x";
  bool started = false;
  for (auto iterator = bytes.rbegin(); iterator != bytes.rend(); ++iterator) {
    if (!started && *iterator == 0 && iterator + 1 != bytes.rend()) {
      continue;
    }
    if (!started) {
      out << std::hex << static_cast<unsigned>(*iterator);
      started = true;
    } else {
      out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(*iterator);
    }
  }
  if (!started) {
    out << '0';
  }
  return out.str();
}

[[nodiscard]] std::string visibility(Visibility value) {
  switch (value) {
  case Visibility::Private:
    return "private";
  case Visibility::Public:
    return "public";
  case Visibility::Friend:
    return "friend";
  }
  return "<visibility>";
}

[[nodiscard]] std::string instructionOperand(const Module &module, const Instruction &instruction) {
  if (instruction.operands.empty() && instruction.wide_operand.empty()) {
    return {};
  }
  const auto first = [&]() { return instruction.operands.empty() ? 0U : instruction.operands[0]; };
  const auto raw = [&]() { return std::to_string(first()); };

  switch (instruction.opcode) {
  case Opcode::BrTrue:
  case Opcode::BrFalse:
  case Opcode::Branch:
    return "@" + raw();
  case Opcode::CopyLoc:
  case Opcode::MoveLoc:
  case Opcode::StLoc:
  case Opcode::MutBorrowLoc:
  case Opcode::ImmBorrowLoc:
    return "local#" + raw();
  case Opcode::Call:
    return functionName(module, static_cast<TableIndex>(first()));
  case Opcode::PackClosure:
    return functionName(module, static_cast<TableIndex>(first())) + ", mask=" + std::to_string(instruction.operands.at(1));
  case Opcode::CallGeneric:
    if (first() < module.function_instantiations.size()) {
      return functionName(module, module.function_instantiations[first()].handle);
    }
    return "function-inst#" + raw();
  case Opcode::PackClosureGeneric:
    if (first() < module.function_instantiations.size()) {
      return functionName(module, module.function_instantiations[first()].handle) + ", mask=" + std::to_string(instruction.operands.at(1));
    }
    return "function-inst#" + raw() + ", mask=" + std::to_string(instruction.operands.at(1));
  case Opcode::Pack:
  case Opcode::Unpack:
  case Opcode::Exists:
  case Opcode::MutBorrowGlobal:
  case Opcode::ImmBorrowGlobal:
  case Opcode::MoveFrom:
  case Opcode::MoveTo:
    if (first() < module.struct_definitions.size()) {
      return structName(module, module.struct_definitions[first()].handle);
    }
    return "struct-def#" + raw();
  case Opcode::LdU128:
  case Opcode::LdU256:
  case Opcode::LdI128:
  case Opcode::LdI256:
    return wideHex(instruction.wide_operand);
  case Opcode::LdI8:
  case Opcode::LdI16:
  case Opcode::LdI32:
  case Opcode::LdI64:
    return std::to_string(std::bit_cast<std::int64_t>(first()));
  case Opcode::VecPack:
  case Opcode::VecUnpack:
    return "signature#" + raw() + ", count=" + std::to_string(instruction.operands.at(1));
  default:
    break;
  }

  std::vector<std::string> values;
  for (const auto value : instruction.operands) {
    values.push_back(std::to_string(value));
  }
  if (!instruction.wide_operand.empty()) {
    values.push_back(wideHex(instruction.wide_operand));
  }
  return join(values, ", ");
}

} // namespace

std::string renderIdentifier(const Module &module, TableIndex index) { return identifier(module, index); }

std::string renderModuleName(const Module &module, TableIndex index) { return moduleName(module, index); }

std::string renderStructName(const Module &module, TableIndex index) { return structName(module, index); }

std::string renderFunctionName(const Module &module, TableIndex index) { return functionName(module, index); }

std::string renderSourceFunctionIdentifier(const Module &module, TableIndex identifier_index) {
  return makeMoveSourceIdentifier(identifier(module, identifier_index), "function", identifier_index);
}

std::string renderSourceFunctionName(const Module &module, TableIndex function_handle_index) {
  if (function_handle_index >= module.function_handles.size()) {
    return "<bad-function:" + std::to_string(function_handle_index) + ">";
  }
  const auto &handle = module.function_handles[function_handle_index];
  return moduleName(module, handle.module) + "::" + renderSourceFunctionIdentifier(module, handle.name);
}

std::string renderAddress(const Address &address) {
  std::ostringstream out;
  out << "0x";
  bool started = false;
  for (const auto byte : address) {
    if (!started && byte == 0) {
      continue;
    }
    if (!started) {
      out << std::hex << static_cast<unsigned>(byte);
      started = true;
    } else {
      out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    }
  }
  if (!started) {
    out << '0';
  }
  return out.str();
}

std::string renderType(const Module &module, const Type &type) {
  switch (type.kind) {
  case TypeKind::Bool:
    return "bool";
  case TypeKind::U8:
    return "u8";
  case TypeKind::U16:
    return "u16";
  case TypeKind::U32:
    return "u32";
  case TypeKind::U64:
    return "u64";
  case TypeKind::U128:
    return "u128";
  case TypeKind::U256:
    return "u256";
  case TypeKind::I8:
    return "i8";
  case TypeKind::I16:
    return "i16";
  case TypeKind::I32:
    return "i32";
  case TypeKind::I64:
    return "i64";
  case TypeKind::I128:
    return "i128";
  case TypeKind::I256:
    return "i256";
  case TypeKind::Address:
    return "address";
  case TypeKind::Signer:
    return "signer";
  case TypeKind::Vector:
    return "vector<" + renderType(module, type.arguments.at(0)) + ">";
  case TypeKind::Reference:
    return "&" + renderType(module, type.arguments.at(0));
  case TypeKind::MutableReference:
    return "&mut " + renderType(module, type.arguments.at(0));
  case TypeKind::Struct:
    return structName(module, type.index);
  case TypeKind::StructInstantiation: {
    std::vector<std::string> arguments;
    for (const auto &argument : type.arguments) {
      arguments.push_back(renderType(module, argument));
    }
    return structName(module, type.index) + "<" + join(arguments, ", ") + ">";
  }
  case TypeKind::TypeParameter:
    if (type.index < module.source_type_parameter_names.size()) {
      return module.source_type_parameter_names[type.index];
    }
    return "T" + std::to_string(type.index);
  case TypeKind::Function: {
    std::vector<std::string> arguments;
    std::vector<std::string> results;
    for (const auto &argument : type.arguments) {
      arguments.push_back(renderType(module, argument));
    }
    for (const auto &result : type.results) {
      results.push_back(renderType(module, result));
    }
    auto rendered = "|" + join(arguments, ", ") + "|(" + join(results, ", ") + ")";
    if (type.abilities.bits != 0) {
      rendered += " has " + abilities(type.abilities);
    }
    return rendered;
  }
  }
  return "<type>";
}

std::string renderInstruction(const Module &module, const Instruction &instruction, std::size_t instruction_index) {
  std::ostringstream out;
  out << std::setw(4) << std::setfill('0') << instruction_index << ": " << opcodeInfo(instruction.opcode).name;
  const auto operand = instructionOperand(module, instruction);
  if (!operand.empty()) {
    out << ' ' << operand;
  }
  return out.str();
}

std::string disassembleModule(const Module &module) {
  std::ostringstream out;
  out << "module " << moduleName(module, module.self_module_handle) << " bytecode-v" << module.version << "\n\n";

  for (std::size_t index = 0; index < module.struct_definitions.size(); ++index) {
    const auto &definition = module.struct_definitions[index];
    const auto &handle = module.struct_handles[definition.handle];
    out << "struct #" << index << ' ' << identifier(module, handle.name);
    if (handle.abilities.bits != 0) {
      out << " has " << abilities(handle.abilities);
    }
    if (definition.field_kind == StructFieldKind::Native) {
      out << " native\n\n";
      continue;
    }
    out << " {\n";
    if (definition.field_kind == StructFieldKind::Declared) {
      for (const auto &field : definition.fields) {
        out << "  " << identifier(module, field.name) << ": " << renderType(module, field.type) << "\n";
      }
    } else {
      for (const auto &variant : definition.variants) {
        out << "  variant " << identifier(module, variant.name) << " {\n";
        for (const auto &field : variant.fields) {
          out << "    " << identifier(module, field.name) << ": " << renderType(module, field.type) << "\n";
        }
        out << "  }\n";
      }
    }
    out << "}\n\n";
  }

  for (std::size_t index = 0; index < module.function_definitions.size(); ++index) {
    const auto &definition = module.function_definitions[index];
    const auto &handle = module.function_handles[definition.handle];
    out << "function #" << index << ' ' << visibility(definition.visibility) << (definition.is_entry ? " entry " : " ") << identifier(module, handle.name)
        << '(' << renderTypes(module, module.signatures[handle.parameters]) << ')';
    const auto &returns = module.signatures[handle.returns];
    if (!returns.empty()) {
      out << " -> (" << renderTypes(module, returns) << ')';
    }
    if (!definition.code.has_value()) {
      out << " native\n\n";
      continue;
    }
    out << " {\n";
    const auto &unit = *definition.code;
    out << "  locals: (" << renderTypes(module, module.signatures[unit.locals]) << ")\n";
    for (std::size_t pc = 0; pc < unit.code.size(); ++pc) {
      out << "  " << renderInstruction(module, unit.code[pc], pc) << '\n';
    }
    out << "}\n\n";
  }
  return out.str();
}

std::string disassembleScript(const Script &script) {
  validateScript(script);
  auto result = disassembleModule(scriptValidationModule(script));
  constexpr std::string_view synthetic_header = "module 0x0::movescape_script bytecode-v";
  const auto header = result.find(synthetic_header);
  if (header == std::string::npos) {
    throw Error(ErrorCode::Malformed, Error::UnknownOffset, "synthetic script disassembly header is missing");
  }
  result.replace(header, synthetic_header.size(), "script bytecode-v");
  constexpr std::string_view synthetic_main = "function #0 public entry main";
  const auto main = result.find(synthetic_main);
  if (main == std::string::npos) {
    throw Error(ErrorCode::Malformed, Error::UnknownOffset, "synthetic script disassembly main is missing");
  }
  result.replace(main, synthetic_main.size(), "main");
  return result;
}

} // namespace movescape
