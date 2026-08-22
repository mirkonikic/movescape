#pragma once

#include "movescape/module.hpp"

#include <optional>
#include <string>
#include <vector>

namespace movescape {

struct NormalizedDeclaration {
  std::string identity;
  std::string value;

  friend bool operator==(const NormalizedDeclaration &, const NormalizedDeclaration &) = default;
};

struct NormalizedModuleInterface {
  std::string module_name;
  std::vector<NormalizedDeclaration> declarations;

  friend bool operator==(const NormalizedModuleInterface &, const NormalizedModuleInterface &) = default;
};

struct InterfaceDifference {
  std::string identity;
  std::optional<std::string> reference;
  std::optional<std::string> candidate;
};

struct ModuleInterfaceComparison {
  std::vector<InterfaceDifference> differences;

  [[nodiscard]] bool equivalent() const noexcept { return differences.empty(); }
};

// Produces a deterministic declaration interface independent of serialized
// table order and indexes. Function bodies, constants, metadata, and source
// names are intentionally outside this comparison level.
[[nodiscard]] NormalizedModuleInterface normalizeModuleInterface(const Module &module);

[[nodiscard]] ModuleInterfaceComparison compareModuleInterfaces(const Module &reference, const Module &candidate);

[[nodiscard]] std::string formatModuleInterface(const NormalizedModuleInterface &interface);
[[nodiscard]] std::string formatModuleInterfaceComparison(const ModuleInterfaceComparison &comparison);

} // namespace movescape
