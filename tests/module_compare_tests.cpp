#include "test.hpp"

#include "movescape/module_compare.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace {

movescape::Module interfaceModule(bool permuted) {
  movescape::Module module;
  module.version = 10;
  module.identifiers = permuted ? std::vector<std::string>{"g", "T", "M", "x", "f", "S"} : std::vector<std::string>{"M", "S", "x", "T", "f", "g"};
  const auto identifier = [&](std::string_view name) {
    const auto found = std::find(module.identifiers.begin(), module.identifiers.end(), name);
    return static_cast<movescape::TableIndex>(std::distance(module.identifiers.begin(), found));
  };

  movescape::Address address{};
  address.back() = 1;
  module.addresses.push_back(address);
  module.module_handles.push_back({.address = 0, .name = identifier("M")});
  module.self_module_handle = 0;

  const movescape::Signature empty;
  const movescape::Signature u8{movescape::Type{.kind = movescape::TypeKind::U8}};
  const movescape::Signature u64{movescape::Type{.kind = movescape::TypeKind::U64}};
  module.signatures = permuted ? std::vector<movescape::Signature>{u64, empty, u8} : std::vector<movescape::Signature>{empty, u8, u64};
  const auto signatureIndex = [&](movescape::TypeKind kind) {
    for (std::size_t index = 0; index < module.signatures.size(); ++index) {
      if (kind == movescape::TypeKind::Bool && module.signatures[index].empty()) {
        return static_cast<movescape::TableIndex>(index);
      }
      if (module.signatures[index].size() == 1 && module.signatures[index][0].kind == kind) {
        return static_cast<movescape::TableIndex>(index);
      }
    }
    return movescape::TableIndex{0};
  };
  const auto empty_signature = signatureIndex(movescape::TypeKind::Bool);
  const auto u8_signature = signatureIndex(movescape::TypeKind::U8);
  const auto u64_signature = signatureIndex(movescape::TypeKind::U64);

  const auto makeStruct = [&](std::string_view name) {
    return movescape::StructHandle{
        .module = 0,
        .name = identifier(name),
        .abilities = name == "S" ? movescape::AbilitySet{movescape::AbilitySet::Key} : movescape::AbilitySet{},
    };
  };
  module.struct_handles =
      permuted ? std::vector<movescape::StructHandle>{makeStruct("T"), makeStruct("S")} : std::vector<movescape::StructHandle>{makeStruct("S"), makeStruct("T")};
  const auto structHandle = [&](std::string_view name) {
    for (std::size_t index = 0; index < module.struct_handles.size(); ++index) {
      if (module.struct_handles[index].name == identifier(name)) {
        return static_cast<movescape::TableIndex>(index);
      }
    }
    return movescape::TableIndex{0};
  };
  const movescape::StructDefinition structure_s{
      .handle = structHandle("S"),
      .field_kind = movescape::StructFieldKind::Declared,
      .fields = {{.name = identifier("x"), .type = movescape::Type{.kind = movescape::TypeKind::U8}}},
  };
  const movescape::StructDefinition structure_t{
      .handle = structHandle("T"),
      .field_kind = movescape::StructFieldKind::Native,
  };
  module.struct_definitions =
      permuted ? std::vector<movescape::StructDefinition>{structure_t, structure_s} : std::vector<movescape::StructDefinition>{structure_s, structure_t};
  const auto structDefinition = [&](std::string_view name) {
    const auto handle = structHandle(name);
    for (std::size_t index = 0; index < module.struct_definitions.size(); ++index) {
      if (module.struct_definitions[index].handle == handle) {
        return static_cast<movescape::TableIndex>(index);
      }
    }
    return movescape::TableIndex{0};
  };

  const auto makeFunction = [&](std::string_view name) {
    if (name == "f") {
      movescape::FunctionHandle handle{
          .module = 0,
          .name = identifier(name),
          .parameters = u8_signature,
          .returns = empty_signature,
      };
      handle.attributes.push_back({.kind = movescape::FunctionAttributeKind::Persistent});
      return handle;
    }
    return movescape::FunctionHandle{
        .module = 0,
        .name = identifier(name),
        .parameters = empty_signature,
        .returns = u64_signature,
    };
  };
  module.function_handles = permuted ? std::vector<movescape::FunctionHandle>{makeFunction("g"), makeFunction("f")}
                                     : std::vector<movescape::FunctionHandle>{makeFunction("f"), makeFunction("g")};
  const auto functionHandle = [&](std::string_view name) {
    for (std::size_t index = 0; index < module.function_handles.size(); ++index) {
      if (module.function_handles[index].name == identifier(name)) {
        return static_cast<movescape::TableIndex>(index);
      }
    }
    return movescape::TableIndex{0};
  };
  movescape::FunctionDefinition function_f{
      .handle = functionHandle("f"),
      .visibility = movescape::Visibility::Public,
      .is_entry = true,
      .acquires = {structDefinition("S")},
  };
  movescape::FunctionDefinition function_g{
      .handle = functionHandle("g"),
      .visibility = movescape::Visibility::Private,
  };
  module.function_definitions =
      permuted ? std::vector<movescape::FunctionDefinition>{function_g, function_f} : std::vector<movescape::FunctionDefinition>{function_f, function_g};
  return module;
}

movescape::Module richInterfaceModule() {
  auto module = interfaceModule(false);
  module.identifiers.insert(module.identifiers.end(), {"A", "y"});

  module.struct_handles[0].type_parameters.push_back({});
  module.struct_definitions[0].fields[0].type = movescape::Type{
      .kind = movescape::TypeKind::TypeParameter,
      .index = 0,
  };
  movescape::Type instantiated_s{
      .kind = movescape::TypeKind::StructInstantiation,
      .index = 0,
      .arguments = {movescape::Type{.kind = movescape::TypeKind::U8}},
  };
  movescape::Type vector_s{
      .kind = movescape::TypeKind::Vector,
      .arguments = {instantiated_s},
  };
  module.struct_definitions[1].field_kind = movescape::StructFieldKind::Variants;
  module.struct_definitions[1].variants = {
      {.name = 6, .fields = {{.name = 7, .type = vector_s}}},
  };

  module.signatures.push_back({instantiated_s});
  module.function_handles[0].parameters = static_cast<movescape::TableIndex>(module.signatures.size() - 1);
  module.function_handles[0].access_specifiers = {
      movescape::AccessSpecifier{
          .kind = movescape::AccessKind::Reads,
          .resource =
              movescape::ResourceSpecifier{
                  .kind = movescape::ResourceSpecifierKind::ResourceInstantiation,
                  .primary = 0,
                  .signature = 1,
              },
          .address =
              movescape::AddressSpecifier{
                  .kind = movescape::AddressSpecifierKind::Literal,
                  .value = 0,
              },
      },
  };
  return module;
}

} // namespace

TEST(normalized_interfaces_ignore_serialized_table_and_definition_order) {
  const auto reference = interfaceModule(false);
  const auto candidate = interfaceModule(true);

  const auto normalized_reference = movescape::normalizeModuleInterface(reference);
  const auto normalized_candidate = movescape::normalizeModuleInterface(candidate);
  REQUIRE_EQ(normalized_reference, normalized_candidate);
  REQUIRE(movescape::compareModuleInterfaces(reference, candidate).equivalent());
}

TEST(interface_comparison_reports_a_changed_function_signature) {
  const auto reference = interfaceModule(false);
  auto candidate = interfaceModule(true);
  const auto f = candidate.function_definitions[1].handle;
  candidate.function_handles[f].returns = candidate.function_handles[f].parameters;

  const auto comparison = movescape::compareModuleInterfaces(reference, candidate);
  REQUIRE(!comparison.equivalent());
  REQUIRE_EQ(comparison.differences.size(), 1U);
  REQUIRE_EQ(comparison.differences[0].identity, std::string("function 0x1::M::f"));
  REQUIRE(comparison.differences[0].reference.has_value());
  REQUIRE(comparison.differences[0].candidate.has_value());
  REQUIRE(comparison.differences[0].reference->find("returns=()") != std::string::npos);
  REQUIRE(comparison.differences[0].candidate->find("returns=(u8)") != std::string::npos);
}

TEST(interface_comparison_reports_missing_declarations_deterministically) {
  const auto reference = interfaceModule(false);
  auto candidate = interfaceModule(true);
  candidate.function_definitions.erase(candidate.function_definitions.begin());
  candidate.function_handles.erase(candidate.function_handles.begin());
  candidate.function_definitions.front().handle = 0;

  const auto comparison = movescape::compareModuleInterfaces(reference, candidate);
  REQUIRE_EQ(comparison.differences.size(), 1U);
  REQUIRE_EQ(comparison.differences[0].identity, std::string("function 0x1::M::g"));
  REQUIRE(comparison.differences[0].reference.has_value());
  REQUIRE(!comparison.differences[0].candidate.has_value());

  const auto formatted = movescape::formatModuleInterfaceComparison(comparison);
  REQUIRE(formatted.starts_with("module interfaces differ (1)\n"));
  REQUIRE(formatted.find("candidate: <missing>") != std::string::npos);
}

TEST(normalized_interface_format_is_stable) {
  const auto interface = movescape::normalizeModuleInterface(interfaceModule(true));
  const auto first = movescape::formatModuleInterface(interface);
  const auto second = movescape::formatModuleInterface(interface);
  REQUIRE_EQ(first, second);
  REQUIRE(first.starts_with("module 0x1::M\n"));
  REQUIRE(first.find("struct 0x1::M::S = abilities=[key]") != std::string::npos);
  REQUIRE(first.find("function 0x1::M::f = visibility=public") != std::string::npos);
}

TEST(normalized_interface_covers_generics_enums_attributes_and_accesses) {
  const auto module = richInterfaceModule();
  const auto normalized = movescape::normalizeModuleInterface(module);
  const auto formatted = movescape::formatModuleInterface(normalized);

  REQUIRE(formatted.find("type-parameters=[[]] fields=[x:T0]") != std::string::npos);
  REQUIRE(formatted.find("variants=[A{y:vector<0x1::M::S<u8>>}]") != std::string::npos);
  REQUIRE(formatted.find("parameters=(0x1::M::S<u8>)") != std::string::npos);
  REQUIRE(formatted.find("attributes=[persistent]") != std::string::npos);
  REQUIRE(formatted.find("access=[reads resource(0x1::M::S(u8)) at literal(0x1)]") != std::string::npos);

  auto changed = module;
  changed.function_handles[0].access_specifiers->front().kind = movescape::AccessKind::Writes;
  const auto comparison = movescape::compareModuleInterfaces(module, changed);
  REQUIRE_EQ(comparison.differences.size(), 1U);
  REQUIRE_EQ(comparison.differences[0].identity, std::string("function 0x1::M::f"));
}
