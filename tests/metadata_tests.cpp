#include "test.hpp"

#include "movescape/metadata.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes bytes(std::string text) { return Bytes(text.begin(), text.end()); }

void appendUleb(Bytes &result, std::uint64_t value) {
  do {
    auto byte = static_cast<std::uint8_t>(value & 0x7fU);
    value >>= 7U;
    if (value != 0) {
      byte = static_cast<std::uint8_t>(byte | 0x80U);
    }
    result.push_back(byte);
  } while (value != 0);
}

void appendU64(Bytes &result, std::uint64_t value) {
  for (unsigned index = 0; index < 8; ++index) {
    result.push_back(static_cast<std::uint8_t>(value & 0xffU));
    value >>= 8U;
  }
}

void appendString(Bytes &result, std::string_view value) {
  appendUleb(result, value.size());
  result.insert(result.end(), value.begin(), value.end());
}

} // namespace

TEST(metadata_raw_only_never_interprets_known_keys) {
  const std::vector<movescape::Metadata> metadata{{
      .key = bytes("compilation_metadata"),
      .value = Bytes{0x00, 0x03, '2', '.', '0', 0x03, '2', '.', '4'},
  }};
  const auto decoded = movescape::decodeMetadata(metadata, movescape::MetadataDecoderVersion::RawOnly);
  REQUIRE_EQ(decoded.size(), 1U);
  REQUIRE(!decoded[0].recognized());
  REQUIRE_EQ(decoded[0].raw.value, metadata[0].value);
}

TEST(decode_aptos_compilation_metadata_v1) {
  const std::vector<movescape::Metadata> metadata{{
      .key = bytes("compilation_metadata"),
      .value = Bytes{0x00, 0x03, '2', '.', '0', 0x03, '2', '.', '4'},
  }};
  const auto decoded = movescape::decodeMetadata(metadata, movescape::MetadataDecoderVersion::AptosV1);
  REQUIRE(decoded[0].decoded());
  REQUIRE(decoded[0].compilation.has_value());
  REQUIRE(!decoded[0].compilation->unstable);
  REQUIRE_EQ(decoded[0].compilation->compiler_version, "2.0");
  REQUIRE_EQ(decoded[0].compilation->language_version, "2.4");
}

TEST(decode_aptos_runtime_metadata_v1) {
  Bytes value;
  appendUleb(value, 1); // error map
  appendU64(value, 7);
  appendString(value, "ESEVEN");
  appendString(value, "seven failed");
  appendUleb(value, 1); // struct attribute map
  appendString(value, "Group");
  appendUleb(value, 1);
  value.push_back(2); // resource group
  appendUleb(value, 1);
  appendString(value, "global");
  appendUleb(value, 1); // function attribute map
  appendString(value, "read");
  appendUleb(value, 1);
  value.push_back(1); // view
  appendUleb(value, 0);

  const std::vector<movescape::Metadata> metadata{{
      .key = bytes("aptos::metadata_v1"),
      .value = value,
  }};
  const auto decoded = movescape::decodeMetadata(metadata, movescape::MetadataDecoderVersion::AptosV1);
  REQUIRE(decoded[0].decoded());
  REQUIRE(decoded[0].runtime.has_value());
  REQUIRE_EQ(decoded[0].runtime->version, 1U);
  REQUIRE_EQ(decoded[0].runtime->errors.size(), 1U);
  REQUIRE_EQ(decoded[0].runtime->errors[0].code, 7U);
  REQUIRE_EQ(decoded[0].runtime->struct_attributes[0].attributes[0].kind, 2U);
  REQUIRE_EQ(decoded[0].runtime->function_attributes[0].attributes[0].kind, 1U);
  REQUIRE_EQ(decoded[0].raw.value, value);
}

TEST(decode_aptos_runtime_metadata_v0) {
  Bytes value;
  appendUleb(value, 1);
  appendU64(value, 42);
  appendString(value, "EANSWER");
  appendString(value, "answer");
  const std::vector<movescape::Metadata> metadata{{
      .key = bytes("aptos::metadata_v0"),
      .value = value,
  }};
  const auto decoded = movescape::decodeMetadata(metadata, movescape::MetadataDecoderVersion::AptosV1);
  REQUIRE(decoded[0].decoded());
  REQUIRE_EQ(decoded[0].runtime->version, 0U);
  REQUIRE(decoded[0].runtime->struct_attributes.empty());
  REQUIRE(decoded[0].runtime->function_attributes.empty());
}

TEST(malformed_known_metadata_retains_raw_value_and_reports_failure) {
  const std::vector<movescape::Metadata> metadata{{
      .key = bytes("compilation_metadata"),
      .value = Bytes{0x02},
  }};
  const auto decoded = movescape::decodeMetadata(metadata, movescape::MetadataDecoderVersion::AptosV1);
  REQUIRE(decoded[0].recognized());
  REQUIRE(!decoded[0].decoded());
  REQUIRE(decoded[0].error.has_value());
  REQUIRE_EQ(decoded[0].raw.value, metadata[0].value);
  const auto formatted = movescape::formatMetadata(decoded, movescape::MetadataDecoderVersion::AptosV1);
  REQUIRE(formatted.find("value-hex: 02") != std::string::npos);
  REQUIRE(formatted.find("decode-status: malformed") != std::string::npos);
}

TEST(metadata_decoder_enforces_string_and_node_limits) {
  const std::vector<movescape::Metadata> metadata{{
      .key = bytes("compilation_metadata"),
      .value = Bytes{0x00, 0x03, '2', '.', '0', 0x03, '2', '.', '4'},
  }};
  auto limits = movescape::MetadataDecodeLimits{};
  limits.max_string_bytes = 2;
  const auto decoded = movescape::decodeMetadata(metadata, movescape::MetadataDecoderVersion::AptosV1, limits);
  REQUIRE(decoded[0].error.has_value());

  limits = movescape::MetadataDecodeLimits{};
  limits.max_total_nodes = 1;
  const auto node_limited = movescape::decodeMetadata(metadata, movescape::MetadataDecoderVersion::AptosV1, limits);
  REQUIRE(node_limited[0].error.has_value());
}

TEST(unknown_metadata_is_never_claimed_by_aptos_decoder) {
  const std::vector<movescape::Metadata> metadata{{
      .key = Bytes{0xff, 0x00},
      .value = Bytes{0x01, 0x02},
  }};
  const auto decoded = movescape::decodeMetadata(metadata, movescape::MetadataDecoderVersion::AptosV1);
  REQUIRE(!decoded[0].recognized());
  const auto formatted = movescape::formatMetadata(decoded, movescape::MetadataDecoderVersion::AptosV1);
  REQUIRE(formatted.find("key-text: <non-UTF-8>") != std::string::npos);
  REQUIRE(formatted.find("key-hex: ff00") != std::string::npos);
  REQUIRE(formatted.find("value-hex: 0102") != std::string::npos);
}

TEST(aptos_decoder_reports_duplicate_compilation_and_runtime_keys) {
  const Bytes compilation{0x00, 0x03, '2', '.', '0', 0x03, '2', '.', '4'};
  const std::vector<movescape::Metadata> metadata{
      {.key = bytes("compilation_metadata"), .value = compilation},
      {.key = bytes("compilation_metadata"), .value = compilation},
      {.key = bytes("aptos::metadata_v0"), .value = Bytes{0x00}},
      {.key = bytes("aptos::metadata_v1"), .value = Bytes{0x00, 0x00, 0x00}},
  };
  const auto decoded = movescape::decodeMetadata(metadata, movescape::MetadataDecoderVersion::AptosV1);
  REQUIRE(decoded[0].decoded());
  REQUIRE(decoded[1].error.has_value());
  REQUIRE(decoded[2].decoded());
  REQUIRE(decoded[3].error.has_value());
  REQUIRE(decoded[1].error->find("duplicate") != std::string::npos);
  REQUIRE(decoded[3].error->find("duplicate") != std::string::npos);
}
