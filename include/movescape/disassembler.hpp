#pragma once

#include "movescape/module.hpp"

#include <string>

namespace movescape {

[[nodiscard]] std::string renderAddress(const Address &address);
[[nodiscard]] std::string renderIdentifier(const Module &module, TableIndex index);
[[nodiscard]] std::string renderModuleName(const Module &module, TableIndex index);
[[nodiscard]] std::string renderStructName(const Module &module, TableIndex index);
[[nodiscard]] std::string renderFunctionName(const Module &module, TableIndex index);
[[nodiscard]] std::string renderSourceFunctionIdentifier(const Module &module, TableIndex identifier_index);
[[nodiscard]] std::string renderSourceFunctionName(const Module &module, TableIndex function_handle_index);
[[nodiscard]] std::string renderType(const Module &module, const Type &type);
[[nodiscard]] std::string renderInstruction(const Module &module, const Instruction &instruction, std::size_t instruction_index);
[[nodiscard]] std::string disassembleModule(const Module &module);
[[nodiscard]] std::string disassembleScript(const Script &script);

} // namespace movescape
