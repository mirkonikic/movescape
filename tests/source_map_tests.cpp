#include "test.hpp"

#include "movescape/move_emitter.hpp"
#include "movescape/source_map.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace {

void pushUleb(std::vector<std::uint8_t> &bytes, std::size_t value) {
  do {
    auto byte = static_cast<std::uint8_t>(value & 0x7fU);
    value >>= 7U;
    if (value != 0U) {
      byte = static_cast<std::uint8_t>(byte | 0x80U);
    }
    bytes.push_back(byte);
  } while (value != 0U);
}

void pushU16(std::vector<std::uint8_t> &bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void pushU32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32U; shift += 8U) {
    bytes.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void pushString(std::vector<std::uint8_t> &bytes, std::string_view value) {
  pushUleb(bytes, value.size());
  bytes.insert(bytes.end(), value.begin(), value.end());
}

void pushLocation(std::vector<std::uint8_t> &bytes, std::uint32_t start = 0, std::uint32_t end = 1, std::uint8_t hash = 0) {
  bytes.insert(bytes.end(), 32U, hash);
  pushU32(bytes, start);
  pushU32(bytes, end);
}

void pushSourceName(std::vector<std::uint8_t> &bytes, std::string_view name) {
  pushString(bytes, name);
  pushLocation(bytes);
}

std::vector<std::uint8_t> sourceMapBytes(std::string_view parameter = "condition", std::string_view local = "result", std::string_view module_name = "M",
                                         bool with_constant = false, bool module_identity = true) {
  std::vector<std::uint8_t> bytes;
  pushLocation(bytes);
  bytes.push_back(module_identity ? 1 : 0);
  if (module_identity) {
    bytes.insert(bytes.end(), 31U, 0U);
    bytes.push_back(1);
    pushString(bytes, module_name);
  }
  pushUleb(bytes, 0); // struct map
  pushUleb(bytes, 1); // function map
  pushU16(bytes, 0);
  pushLocation(bytes);
  pushUleb(bytes, 0); // type parameters
  pushUleb(bytes, 1); // parameters
  pushSourceName(bytes, parameter);
  pushUleb(bytes, 1); // locals
  pushSourceName(bytes, local);
  pushUleb(bytes, 0); // nops
  pushUleb(bytes, 2); // code map
  pushU16(bytes, 0);
  pushLocation(bytes, 10, 20, 0xaa);
  pushU16(bytes, 2);
  pushLocation(bytes, 30, 40, 0xbb);
  bytes.push_back(0);                     // is_native
  pushUleb(bytes, with_constant ? 1 : 0); // constant map
  if (with_constant) {
    pushString(bytes, "MAGIC_BYTES");
    pushU16(bytes, 0);
  }
  return bytes;
}

movescape::Module namedModule() {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"M", "f"};
  module.addresses.push_back({});
  module.addresses[0].back() = 1;
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  const movescape::Type boolean{.kind = movescape::TypeKind::Bool};
  const movescape::Type u64{.kind = movescape::TypeKind::U64};
  module.signatures = {{}, {boolean}, {u64}};
  module.function_handles.push_back({
      .module = 0,
      .name = 1,
      .parameters = 1,
      .returns = 0,
  });
  movescape::CodeUnit unit;
  unit.locals = 2;
  unit.code = {
      {.opcode = movescape::Opcode::LdU64, .operands = {7}},
      {.opcode = movescape::Opcode::StLoc, .operands = {1}},
      {.opcode = movescape::Opcode::Ret},
  };
  module.function_definitions.push_back({.handle = 0, .code = unit});
  return module;
}

} // namespace

TEST(decodes_and_attaches_official_bcs_source_map_local_names) {
  const auto source_map = movescape::loadSourceMap(sourceMapBytes());
  REQUIRE(source_map.module.has_value());
  REQUIRE_EQ(source_map.module->second, std::string("M"));
  REQUIRE_EQ(source_map.functions.size(), 1U);
  REQUIRE(source_map.functions[0].has_value());
  REQUIRE_EQ(source_map.functions[0]->parameters, (std::vector<std::string>{"condition"}));
  REQUIRE_EQ(source_map.functions[0]->locals, (std::vector<std::string>{"result"}));
  REQUIRE_EQ(source_map.functions[0]->code_locations.size(), 2U);
  REQUIRE_EQ(movescape::sourceLocationAt(*source_map.functions[0], 1)->start, 10U);
  REQUIRE_EQ(movescape::sourceLocationAt(*source_map.functions[0], 2)->start, 30U);

  const auto annotated = movescape::withSourceMapNames(namedModule(), source_map);
  const auto emission = movescape::emitMoveModule(annotated);
  REQUIRE(emission.allControlFlowComplete());
  REQUIRE(emission.allSourceSemanticsComplete());
  REQUIRE(emission.source.find("Source-map names were used where available") != std::string::npos);
  REQUIRE(emission.source.find("fun f(condition: bool)") != std::string::npos);
  REQUIRE(emission.source.find("let result: u64;") != std::string::npos);
  REQUIRE(emission.source.find("result = 7u64;") != std::string::npos);
  REQUIRE_EQ(annotated.function_definitions[0].source_code_locations.at(1).second.end, 40U);
}

TEST(source_map_names_are_made_safe_and_collision_free) {
  const auto source_map = movescape::loadSourceMap(sourceMapBytes("tmp0", "tmp0"));
  const auto annotated = movescape::withSourceMapNames(namedModule(), source_map);
  REQUIRE_EQ(annotated.function_definitions[0].source_local_names, (std::vector<std::string>{"local_tmp0", "local_tmp0_1"}));
}

TEST(source_map_constant_names_become_exact_source_declarations) {
  auto module = namedModule();
  module.constants.push_back({
      .type = movescape::Type{.kind = movescape::TypeKind::Vector, .arguments = {movescape::Type{.kind = movescape::TypeKind::U8}}},
      .data = {3, 0x4d, 0x56, 0x01},
  });
  const auto annotated = movescape::withSourceMapNames(module, movescape::loadSourceMap(sourceMapBytes("condition", "result", "M", true)));
  const auto emission = movescape::emitMoveModule(annotated);
  REQUIRE(emission.source.find("const MAGIC_BYTES: vector<u8> = x\"4d5601\";") != std::string::npos);
}

TEST(script_source_maps_restore_main_parameter_and_local_names) {
  const auto module = namedModule();
  movescape::Script script{
      .common = module,
      .type_parameters = {},
      .parameters = 1,
      .access_specifiers = std::nullopt,
      .code = *module.function_definitions[0].code,
  };
  script.common.function_definitions.clear();
  script.common.function_handles.clear();
  script.common.module_handles.clear();
  script.common.addresses.clear();
  script.common.identifiers.clear();
  const auto annotated = movescape::withSourceMapNames(script, movescape::loadSourceMap(sourceMapBytes("condition", "result", "", false, false)));
  REQUIRE_EQ(annotated.source_local_names, (std::vector<std::string>{"condition", "result"}));
  const auto emission = movescape::emitMoveScript(annotated);
  REQUIRE(emission.source.find("fun main(condition: bool)") != std::string::npos);
  REQUIRE(emission.source.find("let result: u64;") != std::string::npos);
}

TEST(rejects_source_maps_for_a_different_module_or_with_trailing_data) {
  REQUIRE_ERROR(movescape::withSourceMapNames(namedModule(), movescape::loadSourceMap(sourceMapBytes("x", "y", "N"))), movescape::ErrorCode::Malformed);

  auto trailing = sourceMapBytes();
  trailing.push_back(0);
  REQUIRE_ERROR(movescape::loadSourceMap(trailing), movescape::ErrorCode::Malformed);
}

TEST(source_map_decoder_enforces_input_and_name_limits) {
  const auto bytes = sourceMapBytes();
  REQUIRE_ERROR(movescape::loadSourceMap(bytes, movescape::SourceMapLimits{.max_input_bytes = bytes.size() - 1, .max_entries = 100, .max_name_bytes = 100}),
                movescape::ErrorCode::ResourceLimit);
  REQUIRE_ERROR(movescape::loadSourceMap(bytes, movescape::SourceMapLimits{.max_input_bytes = bytes.size(), .max_entries = 100, .max_name_bytes = 2}),
                movescape::ErrorCode::ValueOutOfRange);
}
