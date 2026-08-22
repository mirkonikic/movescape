#pragma once

#include "movescape/module.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace movescape {

struct FunctionEmissionStatus {
  std::size_t definition = 0;
  bool native = false;
  bool control_flow_complete = false;
  bool source_semantics_complete = false;
};

struct SourceNameChange {
  std::string context;
  std::string bytecode_name;
  std::string source_name;
};

struct MoveSourcePolicy {
  std::uint32_t minimum_bytecode_version = 5;
  std::string minimum_language_version = "2.0";
  std::vector<std::string> reasons;
};

struct MoveEmission {
  std::string source;
  MoveSourcePolicy policy;
  std::vector<FunctionEmissionStatus> functions;
  std::vector<SourceNameChange> renamed_identifiers;

  [[nodiscard]] bool allControlFlowComplete() const noexcept;
  [[nodiscard]] bool allSourceSemanticsComplete() const noexcept;
};

[[nodiscard]] MoveSourcePolicy sourcePolicy(const Module &module);
[[nodiscard]] MoveSourcePolicy sourcePolicy(const Script &script);
[[nodiscard]] MoveEmission emitMoveModule(const Module &module);
[[nodiscard]] MoveEmission emitMoveScript(const Script &script);

} // namespace movescape
