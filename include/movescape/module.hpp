#pragma once

#include "movescape/opcode.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace movescape {

using TableIndex = std::uint16_t;
using LocalIndex = std::uint8_t;
using CodeOffset = std::uint16_t;

struct AbilitySet {
  std::uint8_t bits = 0;

  static constexpr std::uint8_t Copy = 0x01;
  static constexpr std::uint8_t Drop = 0x02;
  static constexpr std::uint8_t Store = 0x04;
  static constexpr std::uint8_t Key = 0x08;
  static constexpr std::uint8_t All = Copy | Drop | Store | Key;

  [[nodiscard]] bool has(std::uint8_t ability) const noexcept { return (bits & ability) != 0; }

  friend bool operator==(const AbilitySet &, const AbilitySet &) = default;
};

enum class TypeKind {
  Bool,
  U8,
  U16,
  U32,
  U64,
  U128,
  U256,
  I8,
  I16,
  I32,
  I64,
  I128,
  I256,
  Address,
  Signer,
  Vector,
  Reference,
  MutableReference,
  Struct,
  StructInstantiation,
  TypeParameter,
  Function,
};

struct Type {
  TypeKind kind = TypeKind::Bool;
  TableIndex index = 0;
  AbilitySet abilities{};
  std::vector<Type> arguments;
  std::vector<Type> results;

  friend bool operator==(const Type &, const Type &) = default;
};

using Signature = std::vector<Type>;
using Address = std::array<std::uint8_t, 32>;

struct SourceLocation {
  std::array<std::uint8_t, 32> file_hash{};
  std::uint32_t start = 0;
  std::uint32_t end = 0;

  friend bool operator==(const SourceLocation &, const SourceLocation &) = default;
};

struct ModuleHandle {
  TableIndex address = 0;
  TableIndex name = 0;
};

struct StructTypeParameter {
  AbilitySet constraints;
  bool is_phantom = false;
};

struct StructHandle {
  TableIndex module = 0;
  TableIndex name = 0;
  AbilitySet abilities;
  std::vector<StructTypeParameter> type_parameters;
};

enum class FunctionAttributeKind {
  Persistent,
  ModuleLock,
  Pack,
  PackVariant,
  Unpack,
  UnpackVariant,
  TestVariant,
  BorrowFieldImmutable,
  BorrowFieldMutable,
};

struct FunctionAttribute {
  FunctionAttributeKind kind;
  std::optional<std::uint16_t> value;
};

enum class AccessKind { Reads, Writes };
enum class ResourceSpecifierKind {
  Any,
  DeclaredAtAddress,
  DeclaredInModule,
  Resource,
  ResourceInstantiation,
};
enum class AddressSpecifierKind { Any, Literal, Parameter };

struct ResourceSpecifier {
  ResourceSpecifierKind kind = ResourceSpecifierKind::Any;
  TableIndex primary = 0;
  TableIndex signature = 0;
};

struct AddressSpecifier {
  AddressSpecifierKind kind = AddressSpecifierKind::Any;
  TableIndex value = 0;
  std::optional<TableIndex> function_instantiation;
};

struct AccessSpecifier {
  AccessKind kind = AccessKind::Reads;
  bool negated = false;
  ResourceSpecifier resource;
  AddressSpecifier address;
};

struct FunctionHandle {
  TableIndex module = 0;
  TableIndex name = 0;
  TableIndex parameters = 0;
  TableIndex returns = 0;
  std::vector<AbilitySet> type_parameters;
  std::optional<std::vector<AccessSpecifier>> access_specifiers;
  std::vector<FunctionAttribute> attributes;
};

struct FunctionInstantiation {
  TableIndex handle = 0;
  TableIndex type_parameters = 0;
};

struct Constant {
  Type type;
  std::vector<std::uint8_t> data;
};

struct Metadata {
  std::vector<std::uint8_t> key;
  std::vector<std::uint8_t> value;
};

struct StructDefinitionInstantiation {
  TableIndex definition = 0;
  TableIndex type_parameters = 0;
};

struct FieldDefinition {
  TableIndex name = 0;
  Type type;
};

struct VariantDefinition {
  TableIndex name = 0;
  std::vector<FieldDefinition> fields;
};

enum class StructFieldKind { Native, Declared, Variants };

struct StructDefinition {
  TableIndex handle = 0;
  StructFieldKind field_kind = StructFieldKind::Native;
  std::vector<FieldDefinition> fields;
  std::vector<VariantDefinition> variants;
  std::vector<std::string> source_type_parameter_names;
  std::optional<SourceLocation> source_definition_location;
  std::vector<std::vector<SourceLocation>> source_field_locations;
};

struct FieldHandle {
  TableIndex owner = 0;
  std::uint16_t field = 0;
};

struct FieldInstantiation {
  TableIndex handle = 0;
  TableIndex type_parameters = 0;
};

struct VariantFieldHandle {
  TableIndex owner = 0;
  std::vector<std::uint16_t> variants;
  std::uint16_t field = 0;
};

struct VariantFieldInstantiation {
  TableIndex handle = 0;
  TableIndex type_parameters = 0;
};

struct StructVariantHandle {
  TableIndex definition = 0;
  std::uint16_t variant = 0;
};

struct StructVariantInstantiation {
  TableIndex handle = 0;
  TableIndex type_parameters = 0;
};

struct Instruction {
  Opcode opcode = Opcode::Nop;
  std::vector<std::uint64_t> operands;
  std::vector<std::uint8_t> wide_operand;
  std::size_t serialized_offset = 0;
};

struct CodeUnit {
  TableIndex locals = 0;
  std::vector<Instruction> code;
};

enum class Visibility { Private, Public, Friend };

struct FunctionDefinition {
  TableIndex handle = 0;
  Visibility visibility = Visibility::Private;
  bool is_entry = false;
  std::vector<TableIndex> acquires;
  std::optional<CodeUnit> code;
  std::vector<std::string> source_local_names;
  std::vector<std::string> source_type_parameter_names;
  std::optional<SourceLocation> source_definition_location;
  std::vector<std::pair<CodeOffset, SourceLocation>> source_code_locations;
};

struct Module {
  std::uint32_t raw_version = 0;
  std::uint32_t version = 0;
  TableIndex self_module_handle = 0;

  std::vector<ModuleHandle> module_handles;
  std::vector<StructHandle> struct_handles;
  std::vector<FunctionHandle> function_handles;
  std::vector<FunctionInstantiation> function_instantiations;
  std::vector<Signature> signatures;
  std::vector<Constant> constants;
  std::vector<std::string> identifiers;
  std::vector<Address> addresses;
  std::vector<Metadata> metadata;

  std::optional<SourceLocation> source_module_location;
  std::vector<std::optional<std::string>> source_constant_names;

  std::vector<StructDefinition> struct_definitions;
  std::vector<StructDefinitionInstantiation> struct_definition_instantiations;
  std::vector<FunctionDefinition> function_definitions;
  std::vector<FieldHandle> field_handles;
  std::vector<FieldInstantiation> field_instantiations;
  std::vector<ModuleHandle> friends;
  std::vector<VariantFieldHandle> variant_field_handles;
  std::vector<VariantFieldInstantiation> variant_field_instantiations;
  std::vector<StructVariantHandle> struct_variant_handles;
  std::vector<StructVariantInstantiation> struct_variant_instantiations;

  std::vector<std::string> source_type_parameter_names;
};

struct Script {
  Module common;
  std::vector<AbilitySet> type_parameters;
  TableIndex parameters = 0;
  std::optional<std::vector<AccessSpecifier>> access_specifiers;
  CodeUnit code;
  std::vector<std::string> source_local_names;
  std::vector<std::string> source_type_parameter_names;
  std::optional<SourceLocation> source_definition_location;
  std::vector<std::pair<CodeOffset, SourceLocation>> source_code_locations;
};

} // namespace movescape
