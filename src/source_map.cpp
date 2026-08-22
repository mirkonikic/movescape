#include "movescape/source_map.hpp"

#include "movescape/binary_reader.hpp"
#include "movescape/error.hpp"
#include "movescape/source_names.hpp"
#include "movescape/validator.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>

namespace movescape {

namespace {

class SourceMapReader {
public:
  SourceMapReader(std::span<const std::uint8_t> bytes, const SourceMapLimits &limits)
      : reader_(bytes), limits_(limits), remaining_entries_(limits.max_entries) {
    if (bytes.size() > limits.max_input_bytes) {
      fail(ErrorCode::ResourceLimit, 0, "source map exceeds the configured input limit");
    }
  }

  [[nodiscard]] MoveSourceMap read() {
    MoveSourceMap result;
    result.module_location = location("module definition location");
    const auto module_tag = reader_.readU8("source-map module option");
    if (module_tag > 1U) {
      fail(ErrorCode::Malformed, reader_.absolutePosition() - 1U, "source-map module option is not a BCS option tag");
    }
    if (module_tag == 1U) {
      Address address{};
      const auto bytes = reader_.readBytes(address.size(), "module address");
      std::copy(bytes.begin(), bytes.end(), address.begin());
      result.module = std::pair{address, string("module name")};
    }

    readIndexedMap<StructSourceNames>(result.structs, "struct source map", [&]() { return structNames(); });
    readIndexedMap<FunctionSourceNames>(result.functions, "function source map", [&]() { return functionNames(); });

    const auto constants = count("constant source map");
    std::optional<std::string> previous_constant;
    for (std::size_t index = 0; index < constants; ++index) {
      auto name = string("constant name");
      if (previous_constant.has_value() && name <= *previous_constant) {
        fail(ErrorCode::Malformed, reader_.absolutePosition(), "constant source-map keys are not strictly ordered");
      }
      previous_constant = name;
      result.constants.emplace_back(std::move(name), reader_.readU16("constant pool index"));
    }
    if (!reader_.empty()) {
      fail(ErrorCode::Malformed, reader_.absolutePosition(), "source map has trailing bytes");
    }
    return result;
  }

private:
  [[noreturn]] static void fail(ErrorCode code, std::size_t offset, std::string message) { throw Error(code, offset, std::move(message)); }

  [[nodiscard]] std::size_t count(std::string_view field) {
    const auto value = reader_.readUleb128(static_cast<std::uint64_t>(limits_.max_entries), field);
    const auto converted = static_cast<std::size_t>(value);
    if (converted > remaining_entries_) {
      fail(ErrorCode::ResourceLimit, reader_.absolutePosition(), "source-map aggregate entry limit exceeded");
    }
    remaining_entries_ -= converted;
    return converted;
  }

  [[nodiscard]] std::string string(std::string_view field) {
    const auto length = reader_.readUleb128(static_cast<std::uint64_t>(limits_.max_name_bytes), field);
    const auto bytes = reader_.readBytes(static_cast<std::size_t>(length), field);
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
  }

  [[nodiscard]] SourceLocation location(std::string_view field) {
    SourceLocation result;
    const auto hash = reader_.readBytes(result.file_hash.size(), field);
    std::copy(hash.begin(), hash.end(), result.file_hash.begin());
    result.start = reader_.readU32(field);
    result.end = reader_.readU32(field);
    if (result.start > result.end) {
      fail(ErrorCode::Malformed, reader_.absolutePosition() - 4U, "source-map location starts after it ends");
    }
    return result;
  }

  [[nodiscard]] std::vector<std::string> sourceNames(std::string_view field) {
    const auto size = count(field);
    std::vector<std::string> result;
    result.reserve(size);
    for (std::size_t index = 0; index < size; ++index) {
      result.push_back(string(field));
      (void)location(field);
    }
    return result;
  }

  [[nodiscard]] StructSourceNames structNames() {
    StructSourceNames result{
        .definition_location = location("struct definition location"), .type_parameters = sourceNames("struct type parameters"), .field_locations = {}};
    const auto variants = count("struct field variants");
    result.field_locations.reserve(variants);
    for (std::size_t variant = 0; variant < variants; ++variant) {
      const auto fields = count("struct field locations");
      std::vector<SourceLocation> locations;
      locations.reserve(fields);
      for (std::size_t field = 0; field < fields; ++field) {
        locations.push_back(location("struct field location"));
      }
      result.field_locations.push_back(std::move(locations));
    }
    return result;
  }

  template <typename KeyReader, typename ValueReader> void readMap(std::string_view field, KeyReader key_reader, ValueReader value_reader) {
    const auto size = count(field);
    for (std::size_t index = 0; index < size; ++index) {
      key_reader(index);
      value_reader();
    }
  }

  [[nodiscard]] FunctionSourceNames functionNames() {
    FunctionSourceNames result{
        .definition_location = location("function definition location"),
        .type_parameters = sourceNames("function type parameters"),
        .parameters = sourceNames("function parameters"),
        .locals = sourceNames("function locals"),
        .nop_offsets = {},
        .code_locations = {},
        .is_native = false,
    };

    std::optional<std::string> previous_nop;
    readMap(
        "function nop map",
        [&](std::size_t) {
          auto key = string("nop label");
          if (previous_nop.has_value() && key <= *previous_nop) {
            fail(ErrorCode::Malformed, reader_.absolutePosition(), "nop source-map keys are not strictly ordered");
          }
          previous_nop = std::move(key);
        },
        [&]() { result.nop_offsets.emplace_back(*previous_nop, reader_.readU16("nop code offset")); });

    std::optional<std::uint16_t> previous_offset;
    readMap(
        "function code map",
        [&](std::size_t) {
          const auto key = reader_.readU16("code-map offset");
          if (previous_offset.has_value() && key <= *previous_offset) {
            fail(ErrorCode::Malformed, reader_.absolutePosition() - 2U, "code source-map keys are not strictly ordered");
          }
          previous_offset = key;
        },
        [&]() { result.code_locations.emplace_back(*previous_offset, location("code location")); });

    const auto native = reader_.readU8("native function flag");
    if (native > 1U) {
      fail(ErrorCode::Malformed, reader_.absolutePosition() - 1U, "source-map native flag is not a BCS boolean");
    }
    result.is_native = native != 0;
    return result;
  }

  template <typename Value, typename ValueReader>
  void readIndexedMap(std::vector<std::optional<Value>> &output, std::string_view field, ValueReader value_reader) {
    const auto size = count(field);
    std::optional<std::uint16_t> previous;
    for (std::size_t index = 0; index < size; ++index) {
      const auto key = reader_.readU16(field);
      if (previous.has_value() && key <= *previous) {
        fail(ErrorCode::Malformed, reader_.absolutePosition() - 2U, std::string(field) + " keys are not strictly ordered");
      }
      previous = key;
      if (output.size() <= key) {
        output.resize(static_cast<std::size_t>(key) + 1U);
      }
      output[key] = value_reader();
    }
  }

  BinaryReader reader_;
  const SourceMapLimits &limits_;
  std::size_t remaining_entries_;
};

[[nodiscard]] bool generatedName(std::string_view value, std::string_view prefix) {
  if (!value.starts_with(prefix) || value.size() == prefix.size()) {
    return false;
  }
  return std::all_of(value.begin() + static_cast<std::ptrdiff_t>(prefix.size()), value.end(),
                     [](char character) { return character >= '0' && character <= '9'; });
}

[[nodiscard]] std::vector<std::string> safeLocalNames(const FunctionSourceNames &names) {
  std::vector<std::string> raw = names.parameters;
  raw.insert(raw.end(), names.locals.begin(), names.locals.end());
  std::vector<std::string> result;
  result.reserve(raw.size());
  std::set<std::string> used;
  for (std::size_t index = 0; index < raw.size(); ++index) {
    auto candidate = makeMoveSourceIdentifier(raw[index], "local", index);
    if ((!candidate.empty() && candidate.front() >= 'A' && candidate.front() <= 'Z') || generatedName(candidate, "tmp") ||
        generatedName(candidate, "closure_arg")) {
      candidate = "local_" + candidate;
    }
    const auto base = candidate;
    std::size_t collision = 0;
    while (used.contains(candidate)) {
      candidate = base + "_" + std::to_string(++collision);
    }
    used.insert(candidate);
    result.push_back(std::move(candidate));
  }
  return result;
}

[[nodiscard]] std::vector<std::string> safeTypeParameterNames(const std::vector<std::string> &names) {
  std::vector<std::string> result;
  result.reserve(names.size());
  std::set<std::string> used;
  for (std::size_t index = 0; index < names.size(); ++index) {
    auto candidate = makeMoveSourceIdentifier(names[index], "T", index);
    const auto base = candidate;
    std::size_t collision = 0;
    while (used.contains(candidate)) {
      candidate = base + "_" + std::to_string(++collision);
    }
    used.insert(candidate);
    result.push_back(std::move(candidate));
  }
  return result;
}

[[noreturn]] void mismatch(std::string message) { throw Error(ErrorCode::Malformed, Error::UnknownOffset, std::move(message)); }

} // namespace

MoveSourceMap loadSourceMap(std::span<const std::uint8_t> bytes, const SourceMapLimits &limits) { return SourceMapReader(bytes, limits).read(); }

std::optional<SourceLocation> sourceLocationAt(const FunctionSourceNames &function, CodeOffset offset) {
  const auto entry = std::upper_bound(function.code_locations.begin(), function.code_locations.end(), offset,
                                      [](CodeOffset requested, const auto &candidate) { return requested < candidate.first; });
  if (entry == function.code_locations.begin()) {
    return std::nullopt;
  }
  return std::prev(entry)->second;
}

Module withSourceMapNames(const Module &module, const MoveSourceMap &source_map) {
  Module result = module;
  result.source_module_location = source_map.module_location;
  if (source_map.module.has_value()) {
    const auto &self = module.module_handles.at(module.self_module_handle);
    if (module.addresses.at(self.address) != source_map.module->first || module.identifiers.at(self.name) != source_map.module->second) {
      mismatch("source map belongs to a different module");
    }
  }

  for (std::size_t index = 0; index < source_map.structs.size(); ++index) {
    if (!source_map.structs[index].has_value()) {
      continue;
    }
    if (index >= result.struct_definitions.size()) {
      mismatch("source map references an unknown struct definition");
    }
    const auto &handle = result.struct_handles.at(result.struct_definitions[index].handle);
    if (source_map.structs[index]->type_parameters.size() != handle.type_parameters.size()) {
      mismatch("source-map struct type-parameter arity disagrees with bytecode");
    }
    result.struct_definitions[index].source_type_parameter_names = safeTypeParameterNames(source_map.structs[index]->type_parameters);
    result.struct_definitions[index].source_definition_location = source_map.structs[index]->definition_location;
    result.struct_definitions[index].source_field_locations = source_map.structs[index]->field_locations;
  }

  for (std::size_t index = 0; index < source_map.functions.size(); ++index) {
    if (!source_map.functions[index].has_value()) {
      continue;
    }
    if (index >= result.function_definitions.size()) {
      mismatch("source map references an unknown function definition");
    }
    auto &definition = result.function_definitions[index];
    const auto &handle = result.function_handles.at(definition.handle);
    const auto parameter_count = result.signatures.at(handle.parameters).size();
    const auto local_count = definition.code.has_value() ? result.signatures.at(definition.code->locals).size() : 0U;
    const auto &names = *source_map.functions[index];
    if (names.parameters.size() != parameter_count || names.locals.size() != local_count) {
      mismatch("source-map parameter/local arity disagrees with bytecode");
    }
    if (names.type_parameters.size() != handle.type_parameters.size()) {
      mismatch("source-map function type-parameter arity disagrees with bytecode");
    }
    if (names.is_native != !definition.code.has_value()) {
      mismatch("source-map native flag disagrees with bytecode");
    }
    if (definition.code.has_value() &&
        std::ranges::any_of(names.code_locations, [&](const auto &entry) { return entry.first >= definition.code->code.size(); })) {
      mismatch("source-map code offset is outside the function body");
    }
    definition.source_local_names = safeLocalNames(names);
    definition.source_type_parameter_names = safeTypeParameterNames(names.type_parameters);
    definition.source_definition_location = names.definition_location;
    definition.source_code_locations = names.code_locations;
  }

  result.source_constant_names.resize(result.constants.size());
  for (const auto &[name, constant] : source_map.constants) {
    if (constant >= result.constants.size()) {
      mismatch("source map references an unknown constant-pool entry");
    }
    auto safe = makeMoveSourceIdentifier(name, "CONSTANT", constant);
    if (!safe.empty() && safe.front() >= 'a' && safe.front() <= 'z') {
      safe = "CONSTANT_" + safe;
    }
    auto &slot = result.source_constant_names[constant];
    if (!slot.has_value()) {
      slot = std::move(safe);
    }
  }
  return result;
}

Script withSourceMapNames(const Script &script, const MoveSourceMap &source_map) {
  if (source_map.module.has_value()) {
    mismatch("script source map unexpectedly declares a module identity");
  }
  const auto annotated = withSourceMapNames(scriptValidationModule(script), source_map);
  if (annotated.function_definitions.size() != 1U) {
    mismatch("script validation view has no unique main definition");
  }
  Script result = script;
  result.common.source_module_location = annotated.source_module_location;
  result.common.source_constant_names = annotated.source_constant_names;
  const auto &main = annotated.function_definitions.front();
  result.source_local_names = main.source_local_names;
  result.source_type_parameter_names = main.source_type_parameter_names;
  result.source_definition_location = main.source_definition_location;
  result.source_code_locations = main.source_code_locations;
  return result;
}

} // namespace movescape
