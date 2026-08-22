#pragma once

#include "movescape/module.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace movescape {

enum class MetadataDecoderVersion {
  RawOnly,
  AptosV1,
};

struct MetadataDecodeLimits {
  std::size_t max_container_entries = 4096;
  std::size_t max_string_bytes = 65535;
  std::size_t max_total_nodes = 65536;
};

struct AptosCompilationMetadataV1 {
  bool unstable = false;
  std::string compiler_version;
  std::string language_version;
};

struct AptosErrorDescription {
  std::uint64_t code = 0;
  std::string name;
  std::string description;
};

struct AptosKnownAttribute {
  std::uint8_t kind = 0;
  std::vector<std::string> arguments;
};

struct AptosNamedAttributes {
  std::string name;
  std::vector<AptosKnownAttribute> attributes;
};

struct AptosRuntimeMetadata {
  std::uint32_t version = 0;
  std::vector<AptosErrorDescription> errors;
  std::vector<AptosNamedAttributes> struct_attributes;
  std::vector<AptosNamedAttributes> function_attributes;
};

enum class DecodedMetadataKind {
  Unknown,
  AptosCompilationV1,
  AptosRuntimeV0,
  AptosRuntimeV1,
};

struct DecodedMetadata {
  Metadata raw;
  DecodedMetadataKind kind = DecodedMetadataKind::Unknown;
  std::optional<AptosCompilationMetadataV1> compilation;
  std::optional<AptosRuntimeMetadata> runtime;
  std::optional<std::string> error;

  [[nodiscard]] bool recognized() const noexcept { return kind != DecodedMetadataKind::Unknown; }
  [[nodiscard]] bool decoded() const noexcept { return recognized() && !error.has_value(); }
};

[[nodiscard]] std::vector<DecodedMetadata> decodeMetadata(std::span<const Metadata> metadata, MetadataDecoderVersion version,
                                                          const MetadataDecodeLimits &limits = {});

[[nodiscard]] std::string formatMetadata(std::span<const DecodedMetadata> metadata, MetadataDecoderVersion version);

[[nodiscard]] std::string_view decodedMetadataKindName(DecodedMetadataKind kind) noexcept;

[[nodiscard]] std::string_view aptosAttributeKindName(std::uint8_t kind) noexcept;

} // namespace movescape
