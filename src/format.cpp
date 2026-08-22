#include "movescape/format.hpp"

namespace movescape::format {

std::optional<TableKind> tableKindFromByte(std::uint8_t value) noexcept {
  switch (value) {
  case 0x01:
    return TableKind::ModuleHandles;
  case 0x02:
    return TableKind::StructHandles;
  case 0x03:
    return TableKind::FunctionHandles;
  case 0x04:
    return TableKind::FunctionInstantiations;
  case 0x05:
    return TableKind::Signatures;
  case 0x06:
    return TableKind::Constants;
  case 0x07:
    return TableKind::Identifiers;
  case 0x08:
    return TableKind::AddressIdentifiers;
  case 0x0a:
    return TableKind::StructDefinitions;
  case 0x0b:
    return TableKind::StructDefinitionInstantiations;
  case 0x0c:
    return TableKind::FunctionDefinitions;
  case 0x0d:
    return TableKind::FieldHandles;
  case 0x0e:
    return TableKind::FieldInstantiations;
  case 0x0f:
    return TableKind::FriendDeclarations;
  case 0x10:
    return TableKind::Metadata;
  case 0x11:
    return TableKind::VariantFieldHandles;
  case 0x12:
    return TableKind::VariantFieldInstantiations;
  case 0x13:
    return TableKind::StructVariantHandles;
  case 0x14:
    return TableKind::StructVariantInstantiations;
  default:
    return std::nullopt;
  }
}

std::string_view tableKindName(TableKind kind) noexcept {
  switch (kind) {
  case TableKind::ModuleHandles:
    return "module-handles";
  case TableKind::StructHandles:
    return "struct-handles";
  case TableKind::FunctionHandles:
    return "function-handles";
  case TableKind::FunctionInstantiations:
    return "function-instantiations";
  case TableKind::Signatures:
    return "signatures";
  case TableKind::Constants:
    return "constants";
  case TableKind::Identifiers:
    return "identifiers";
  case TableKind::AddressIdentifiers:
    return "address-identifiers";
  case TableKind::StructDefinitions:
    return "struct-definitions";
  case TableKind::StructDefinitionInstantiations:
    return "struct-definition-instantiations";
  case TableKind::FunctionDefinitions:
    return "function-definitions";
  case TableKind::FieldHandles:
    return "field-handles";
  case TableKind::FieldInstantiations:
    return "field-instantiations";
  case TableKind::FriendDeclarations:
    return "friend-declarations";
  case TableKind::Metadata:
    return "metadata";
  case TableKind::VariantFieldHandles:
    return "variant-field-handles";
  case TableKind::VariantFieldInstantiations:
    return "variant-field-instantiations";
  case TableKind::StructVariantHandles:
    return "struct-variant-handles";
  case TableKind::StructVariantInstantiations:
    return "struct-variant-instantiations";
  }
  return "unknown";
}

std::uint32_t tableMinimumVersion(TableKind kind) noexcept {
  switch (kind) {
  case TableKind::VariantFieldHandles:
  case TableKind::VariantFieldInstantiations:
  case TableKind::StructVariantHandles:
  case TableKind::StructVariantInstantiations:
    return 7;
  default:
    return kMinimumSupportedVersion;
  }
}

bool isModuleOnlyTable(TableKind kind) noexcept {
  switch (kind) {
  case TableKind::StructDefinitions:
  case TableKind::StructDefinitionInstantiations:
  case TableKind::FunctionDefinitions:
  case TableKind::FieldHandles:
  case TableKind::FieldInstantiations:
  case TableKind::FriendDeclarations:
  case TableKind::VariantFieldHandles:
  case TableKind::VariantFieldInstantiations:
  case TableKind::StructVariantHandles:
  case TableKind::StructVariantInstantiations:
    return true;
  default:
    return false;
  }
}

} // namespace movescape::format
