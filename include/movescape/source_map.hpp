#pragma once

#include "movescape/module.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace movescape {

struct SourceMapLimits {
  std::size_t max_input_bytes = 16U * 1024U * 1024U;
  std::size_t max_entries = 1U << 20U;
  std::size_t max_name_bytes = 1024U;
};

struct FunctionSourceNames {
  SourceLocation definition_location;
  std::vector<std::string> type_parameters;
  std::vector<std::string> parameters;
  std::vector<std::string> locals;
  std::vector<std::pair<std::string, CodeOffset>> nop_offsets;
  std::vector<std::pair<CodeOffset, SourceLocation>> code_locations;
  bool is_native = false;
};

struct StructSourceNames {
  SourceLocation definition_location;
  std::vector<std::string> type_parameters;
  std::vector<std::vector<SourceLocation>> field_locations;
};

struct MoveSourceMap {
  SourceLocation module_location;
  std::optional<std::pair<Address, std::string>> module;
  std::vector<std::optional<StructSourceNames>> structs;
  std::vector<std::optional<FunctionSourceNames>> functions;
  std::vector<std::pair<std::string, TableIndex>> constants;
};

// Decodes the current Aptos/Diem .mvsm BCS schema, retaining definition and
// per-instruction locations as well as source-level names.
[[nodiscard]] MoveSourceMap loadSourceMap(std::span<const std::uint8_t> bytes, const SourceMapLimits &limits = {});

// Verifies module identity and definition/local arities before attaching
// source names. The returned module is an independent annotated copy.
[[nodiscard]] Module withSourceMapNames(const Module &module, const MoveSourceMap &source_map);
[[nodiscard]] Script withSourceMapNames(const Script &script, const MoveSourceMap &source_map);

// Returns the closest source span whose code-map offset is at or before the
// requested bytecode instruction offset. Empty maps and offsets preceding the
// first entry have no location.
[[nodiscard]] std::optional<SourceLocation> sourceLocationAt(const FunctionSourceNames &function, CodeOffset offset);

} // namespace movescape
