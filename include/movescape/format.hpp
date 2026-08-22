#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace movescape::format {

inline constexpr std::array<std::uint8_t, 4> kMagic{0xa1, 0x1c, 0xeb, 0x0b};
inline constexpr std::uint32_t kAptosVersionMask = 0x0a000000U;
inline constexpr std::uint32_t kMinimumSupportedVersion = 5;
inline constexpr std::uint32_t kMaximumSupportedVersion = 10;
inline constexpr std::uint64_t kMaximumTableCount = 255;
inline constexpr std::uint64_t kMaximumTableOffset = 0xffffffffULL;
inline constexpr std::uint64_t kMaximumTableSize = 0xffffffffULL;

enum class TableKind : std::uint8_t {
  ModuleHandles = 0x01,
  StructHandles = 0x02,
  FunctionHandles = 0x03,
  FunctionInstantiations = 0x04,
  Signatures = 0x05,
  Constants = 0x06,
  Identifiers = 0x07,
  AddressIdentifiers = 0x08,
  StructDefinitions = 0x0a,
  StructDefinitionInstantiations = 0x0b,
  FunctionDefinitions = 0x0c,
  FieldHandles = 0x0d,
  FieldInstantiations = 0x0e,
  FriendDeclarations = 0x0f,
  Metadata = 0x10,
  VariantFieldHandles = 0x11,
  VariantFieldInstantiations = 0x12,
  StructVariantHandles = 0x13,
  StructVariantInstantiations = 0x14,
};

[[nodiscard]] std::optional<TableKind> tableKindFromByte(std::uint8_t value) noexcept;
[[nodiscard]] std::string_view tableKindName(TableKind kind) noexcept;
[[nodiscard]] std::uint32_t tableMinimumVersion(TableKind kind) noexcept;
[[nodiscard]] bool isModuleOnlyTable(TableKind kind) noexcept;

} // namespace movescape::format
