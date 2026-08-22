#pragma once

#include "movescape/cfg.hpp"
#include "movescape/module.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace movescape {

using DefinitionId = std::size_t;

struct LocalDefinition {
  DefinitionId id = 0;
  LocalIndex local = 0;
  std::optional<std::size_t> instruction;
  bool parameter = false;
};

struct LocalUse {
  std::size_t instruction = 0;
  LocalIndex local = 0;
  Opcode opcode = Opcode::Nop;
  std::vector<DefinitionId> reaching_definitions;
};

enum class Availability {
  Unavailable,
  MaybeAvailable,
  Available,
};

struct LocalAnalysis {
  std::vector<LocalDefinition> definitions;
  std::vector<LocalUse> uses;
  std::vector<std::vector<std::size_t>> definition_uses;

  // Indexed [block][local].
  std::vector<std::vector<std::vector<DefinitionId>>> reaching_in;
  std::vector<std::vector<std::vector<DefinitionId>>> reaching_out;
  std::vector<std::vector<bool>> live_in;
  std::vector<std::vector<bool>> live_out;
  std::vector<std::vector<Availability>> availability_in;
  std::vector<std::vector<Availability>> availability_out;

  std::size_t fixed_point_iterations = 0;
};

[[nodiscard]] LocalAnalysis analyzeLocals(const Module &module, const FunctionDefinition &function, const ControlFlowGraph &graph);
[[nodiscard]] std::string formatLocalAnalysis(const Module &module, const FunctionDefinition &function, const LocalAnalysis &analysis);

} // namespace movescape
