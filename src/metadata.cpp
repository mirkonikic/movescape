#include "movescape/metadata.hpp"

#include "movescape/binary_reader.hpp"
#include "movescape/error.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace movescape {

namespace {

constexpr std::string_view kCompilationKey = "compilation_metadata";
constexpr std::string_view kAptosRuntimeV0Key = "aptos::metadata_v0";
constexpr std::string_view kAptosRuntimeV1Key = "aptos::metadata_v1";

bool equalsKey(const std::vector<std::uint8_t> &key, std::string_view expected) {
  return key.size() == expected.size() && std::equal(key.begin(), key.end(), expected.begin(), expected.end(),
                                                     [](std::uint8_t left, char right) { return left == static_cast<std::uint8_t>(right); });
}

bool validUtf8(std::span<const std::uint8_t> bytes) {
  std::size_t index = 0;
  while (index < bytes.size()) {
    const auto first = bytes[index];
    std::size_t continuation_count = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if (first <= 0x7fU) {
      ++index;
      continue;
    }
    if (first >= 0xc2U && first <= 0xdfU) {
      continuation_count = 1;
      value = first & 0x1fU;
      minimum = 0x80U;
    } else if (first >= 0xe0U && first <= 0xefU) {
      continuation_count = 2;
      value = first & 0x0fU;
      minimum = 0x800U;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      continuation_count = 3;
      value = first & 0x07U;
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (continuation_count > bytes.size() - index - 1U) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
      const auto next = bytes[index + offset];
      if ((next & 0xc0U) != 0x80U) {
        return false;
      }
      value = (value << 6U) | (next & 0x3fU);
    }
    if (value < minimum || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)) {
      return false;
    }
    index += continuation_count + 1U;
  }
  return true;
}

class BcsMetadataReader {
public:
  BcsMetadataReader(std::span<const std::uint8_t> bytes, const MetadataDecodeLimits &limits) : reader_(bytes), limits_(limits) {}

  bool readBool(std::string_view field) {
    consumeNode(field);
    const auto value = reader_.readU8(field);
    if (value > 1U) {
      throw Error(ErrorCode::Malformed, reader_.position() - 1U, std::string(field) + " is not a canonical BCS Boolean");
    }
    return value != 0U;
  }

  std::uint8_t readU8(std::string_view field) {
    consumeNode(field);
    return reader_.readU8(field);
  }

  std::uint64_t readU64(std::string_view field) {
    consumeNode(field);
    return reader_.readU64(field);
  }

  std::size_t readLength(std::string_view field) {
    consumeNode(field);
    const auto value = reader_.readUleb128(static_cast<std::uint64_t>(limits_.max_container_entries), field);
    return static_cast<std::size_t>(value);
  }

  std::string readString(std::string_view field) {
    consumeNode(field);
    const auto length = reader_.readUleb128(static_cast<std::uint64_t>(limits_.max_string_bytes), field);
    const auto bytes = reader_.readBytes(static_cast<std::size_t>(length), field);
    if (!validUtf8(bytes)) {
      throw Error(ErrorCode::Malformed, reader_.position() - static_cast<std::size_t>(length), std::string(field) + " is not valid UTF-8");
    }
    if (bytes.empty()) {
      return {};
    }
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  }

  void requireEnd() const {
    if (!reader_.empty()) {
      throw Error(ErrorCode::Malformed, reader_.position(), "trailing bytes after decoded metadata value");
    }
  }

private:
  void consumeNode(std::string_view field) {
    if (nodes_ >= limits_.max_total_nodes) {
      throw Error(ErrorCode::ResourceLimit, reader_.position(), std::string("metadata node limit reached while reading ") + std::string(field));
    }
    ++nodes_;
  }

  BinaryReader reader_;
  const MetadataDecodeLimits &limits_;
  std::size_t nodes_ = 0;
};

std::vector<AptosErrorDescription> readErrorMap(BcsMetadataReader &reader) {
  const auto count = reader.readLength("Aptos error-map length");
  std::vector<AptosErrorDescription> result;
  result.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    result.push_back(AptosErrorDescription{
        .code = reader.readU64("Aptos error code"),
        .name = reader.readString("Aptos error name"),
        .description = reader.readString("Aptos error description"),
    });
  }
  return result;
}

std::vector<AptosNamedAttributes> readAttributeMap(BcsMetadataReader &reader, std::string_view map_name) {
  const auto count = reader.readLength(map_name);
  std::vector<AptosNamedAttributes> result;
  result.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    AptosNamedAttributes named;
    named.name = reader.readString("Aptos attribute owner");
    const auto attribute_count = reader.readLength("Aptos attribute count");
    named.attributes.reserve(attribute_count);
    for (std::size_t attribute_index = 0; attribute_index < attribute_count; ++attribute_index) {
      AptosKnownAttribute attribute;
      attribute.kind = reader.readU8("Aptos attribute kind");
      const auto argument_count = reader.readLength("Aptos attribute argument count");
      attribute.arguments.reserve(argument_count);
      for (std::size_t argument_index = 0; argument_index < argument_count; ++argument_index) {
        attribute.arguments.push_back(reader.readString("Aptos attribute argument"));
      }
      named.attributes.push_back(std::move(attribute));
    }
    result.push_back(std::move(named));
  }
  return result;
}

AptosCompilationMetadataV1 decodeCompilation(std::span<const std::uint8_t> bytes, const MetadataDecodeLimits &limits) {
  BcsMetadataReader reader(bytes, limits);
  AptosCompilationMetadataV1 result{
      .unstable = reader.readBool("compilation unstable flag"),
      .compiler_version = reader.readString("compiler version"),
      .language_version = reader.readString("language version"),
  };
  reader.requireEnd();
  return result;
}

AptosRuntimeMetadata decodeRuntime(std::span<const std::uint8_t> bytes, std::uint32_t version, const MetadataDecodeLimits &limits) {
  BcsMetadataReader reader(bytes, limits);
  AptosRuntimeMetadata result;
  result.version = version;
  result.errors = readErrorMap(reader);
  if (version == 1U) {
    result.struct_attributes = readAttributeMap(reader, "Aptos struct-attribute map length");
    result.function_attributes = readAttributeMap(reader, "Aptos function-attribute map length");
  }
  reader.requireEnd();
  return result;
}

std::string hex(std::span<const std::uint8_t> bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2U);
  for (const auto byte : bytes) {
    result.push_back(digits[byte >> 4U]);
    result.push_back(digits[byte & 0x0fU]);
  }
  return result;
}

std::string quoted(std::string_view text) {
  std::ostringstream out;
  out << '"';
  for (const auto character : text) {
    const auto byte = static_cast<unsigned char>(character);
    if (character == '\\' || character == '"') {
      out << '\\' << character;
    } else if (byte >= 0x20U && byte != 0x7fU) {
      out << character;
    } else {
      out << "\\x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte) << std::dec;
    }
  }
  out << '"';
  return out.str();
}

std::string quotedBytes(std::span<const std::uint8_t> bytes) {
  if (!validUtf8(bytes)) {
    return "<non-UTF-8>";
  }
  if (bytes.empty()) {
    return "\"\"";
  }
  return quoted(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
}

} // namespace

std::vector<DecodedMetadata> decodeMetadata(std::span<const Metadata> metadata, MetadataDecoderVersion version, const MetadataDecodeLimits &limits) {
  std::vector<DecodedMetadata> result;
  result.reserve(metadata.size());
  bool compilation_seen = false;
  bool runtime_seen = false;
  for (const auto &raw : metadata) {
    DecodedMetadata decoded;
    decoded.raw = raw;
    if (version == MetadataDecoderVersion::RawOnly) {
      result.push_back(std::move(decoded));
      continue;
    }

    if (equalsKey(raw.key, kCompilationKey)) {
      decoded.kind = DecodedMetadataKind::AptosCompilationV1;
    } else if (equalsKey(raw.key, kAptosRuntimeV0Key)) {
      decoded.kind = DecodedMetadataKind::AptosRuntimeV0;
    } else if (equalsKey(raw.key, kAptosRuntimeV1Key)) {
      decoded.kind = DecodedMetadataKind::AptosRuntimeV1;
    } else {
      result.push_back(std::move(decoded));
      continue;
    }

    if (decoded.kind == DecodedMetadataKind::AptosCompilationV1) {
      if (compilation_seen) {
        decoded.error = "malformed: duplicate compilation_metadata key";
        result.push_back(std::move(decoded));
        continue;
      }
      compilation_seen = true;
    } else {
      if (runtime_seen) {
        decoded.error = "malformed: duplicate Aptos runtime metadata key";
        result.push_back(std::move(decoded));
        continue;
      }
      runtime_seen = true;
    }

    try {
      if (decoded.kind == DecodedMetadataKind::AptosCompilationV1) {
        decoded.compilation = decodeCompilation(raw.value, limits);
      } else {
        decoded.runtime = decodeRuntime(raw.value, decoded.kind == DecodedMetadataKind::AptosRuntimeV1 ? 1U : 0U, limits);
      }
    } catch (const Error &error) {
      decoded.error = std::string(errorCodeName(error.code())) + ": " + error.what();
    }
    result.push_back(std::move(decoded));
  }
  return result;
}

std::string_view decodedMetadataKindName(DecodedMetadataKind kind) noexcept {
  switch (kind) {
  case DecodedMetadataKind::Unknown:
    return "unknown";
  case DecodedMetadataKind::AptosCompilationV1:
    return "aptos-compilation-v1";
  case DecodedMetadataKind::AptosRuntimeV0:
    return "aptos-runtime-v0";
  case DecodedMetadataKind::AptosRuntimeV1:
    return "aptos-runtime-v1";
  }
  return "unknown";
}

std::string_view aptosAttributeKindName(std::uint8_t kind) noexcept {
  switch (kind) {
  case 0:
    return "legacy-view-function";
  case 1:
    return "view-function";
  case 2:
    return "resource-group";
  case 3:
    return "resource-group-member";
  case 4:
    return "event";
  case 5:
    return "randomness";
  default:
    return "unknown";
  }
}

std::string formatMetadata(std::span<const DecodedMetadata> metadata, MetadataDecoderVersion version) {
  std::ostringstream out;
  out << "decoder: " << (version == MetadataDecoderVersion::AptosV1 ? "aptos-v1" : "raw-only") << '\n' << "metadata-entries: " << metadata.size() << '\n';
  for (std::size_t index = 0; index < metadata.size(); ++index) {
    const auto &entry = metadata[index];
    out << "metadata[" << index << "]:\n"
        << "  key-text: " << quotedBytes(entry.raw.key) << '\n'
        << "  key-hex: " << hex(entry.raw.key) << '\n'
        << "  value-hex: " << hex(entry.raw.value) << '\n';
    if (!entry.recognized()) {
      out << "  decoded-kind: " << (version == MetadataDecoderVersion::RawOnly ? "not-requested" : "unknown") << '\n';
      continue;
    }
    out << "  decoded-kind: " << decodedMetadataKindName(entry.kind) << '\n';
    if (entry.error.has_value()) {
      out << "  decode-status: malformed\n"
          << "  decode-error: " << quoted(*entry.error) << '\n';
      continue;
    }
    out << "  decode-status: decoded\n";
    if (entry.compilation.has_value()) {
      out << "  unstable: " << (entry.compilation->unstable ? "yes" : "no") << '\n'
          << "  compiler-version: " << quoted(entry.compilation->compiler_version) << '\n'
          << "  language-version: " << quoted(entry.compilation->language_version) << '\n';
    } else if (entry.runtime.has_value()) {
      out << "  runtime-version: " << entry.runtime->version << '\n' << "  errors: " << entry.runtime->errors.size() << '\n';
      for (const auto &error : entry.runtime->errors) {
        out << "    " << error.code << ": " << quoted(error.name) << " - " << quoted(error.description) << '\n';
      }
      out << "  struct-attribute-owners: " << entry.runtime->struct_attributes.size() << '\n';
      for (const auto &owner : entry.runtime->struct_attributes) {
        out << "    " << quoted(owner.name) << ":";
        for (const auto &attribute : owner.attributes) {
          out << ' ' << aptosAttributeKindName(attribute.kind) << '(';
          for (std::size_t argument = 0; argument < attribute.arguments.size(); ++argument) {
            if (argument != 0) {
              out << ", ";
            }
            out << quoted(attribute.arguments[argument]);
          }
          out << ')';
        }
        out << '\n';
      }
      out << "  function-attribute-owners: " << entry.runtime->function_attributes.size() << '\n';
      for (const auto &owner : entry.runtime->function_attributes) {
        out << "    " << quoted(owner.name) << ":";
        for (const auto &attribute : owner.attributes) {
          out << ' ' << aptosAttributeKindName(attribute.kind) << '(';
          for (std::size_t argument = 0; argument < attribute.arguments.size(); ++argument) {
            if (argument != 0) {
              out << ", ";
            }
            out << quoted(attribute.arguments[argument]);
          }
          out << ')';
        }
        out << '\n';
      }
    }
  }
  return out.str();
}

} // namespace movescape
