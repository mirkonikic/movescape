#pragma once

#include "movescape/module.hpp"

#include <optional>
#include <string>
#include <vector>

namespace movescape {

struct NormalizedBasicBlock {
  std::string label;
  std::vector<std::string> instructions;
  std::vector<std::string> successors;
  std::string exit;

  friend bool operator==(const NormalizedBasicBlock &, const NormalizedBasicBlock &) = default;
};

struct NormalizedFunctionBody {
  std::string identity;
  bool native = false;
  std::vector<NormalizedBasicBlock> blocks;

  friend bool operator==(const NormalizedFunctionBody &, const NormalizedFunctionBody &) = default;
};

struct NormalizedModuleBodies {
  std::string module_name;
  std::vector<NormalizedFunctionBody> functions;

  friend bool operator==(const NormalizedModuleBodies &, const NormalizedModuleBodies &) = default;
};

struct FunctionBodyDifference {
  std::string identity;
  std::optional<std::string> reference;
  std::optional<std::string> candidate;
};

struct ModuleBodyComparison {
  std::vector<FunctionBodyDifference> differences;

  [[nodiscard]] bool equivalent() const noexcept { return differences.empty(); }
};

// Resolves table indexes to qualified identities and type arguments, renames
// branch targets by canonical CFG traversal, alpha-renames non-parameter
// locals by first use, and removes layout-only branch instructions and Nops.
// Equality remains intentionally conservative: different normalized opcode
// sequences are reported even when they may be behaviorally equivalent.
[[nodiscard]] NormalizedModuleBodies normalizeModuleBodies(const Module &module);

[[nodiscard]] ModuleBodyComparison compareModuleBodies(const Module &reference, const Module &candidate);

[[nodiscard]] std::string formatNormalizedModuleBodies(const NormalizedModuleBodies &bodies);
[[nodiscard]] std::string formatModuleBodyComparison(const ModuleBodyComparison &comparison);

} // namespace movescape
