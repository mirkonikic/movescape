#include "test.hpp"

#include "movescape/format.hpp"
#include "movescape/loader.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using movescape::ErrorCode;

namespace {

std::vector<std::uint8_t> header(std::uint32_t raw_version = 5) {
  return {
      0xa1,
      0x1c,
      0xeb,
      0x0b,
      static_cast<std::uint8_t>(raw_version & 0xffU),
      static_cast<std::uint8_t>((raw_version >> 8U) & 0xffU),
      static_cast<std::uint8_t>((raw_version >> 16U) & 0xffU),
      static_cast<std::uint8_t>((raw_version >> 24U) & 0xffU),
  };
}

std::vector<std::uint8_t> singleTableEnvelope(std::uint32_t version, movescape::format::TableKind kind, std::vector<std::uint8_t> content = {0x00}) {
  auto bytes = header(version);
  bytes.push_back(0x01); // table count
  bytes.push_back(static_cast<std::uint8_t>(kind));
  bytes.push_back(0x00); // relative offset
  bytes.push_back(static_cast<std::uint8_t>(content.size()));
  bytes.insert(bytes.end(), content.begin(), content.end());
  return bytes;
}

class TemporaryBinaryFile {
public:
  explicit TemporaryBinaryFile(const std::vector<std::uint8_t> &bytes)
      : path_(std::filesystem::temp_directory_path() /
              ("movescape-limit-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".mv")) {
    std::ofstream output(path_, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
      throw std::runtime_error("unable to create temporary binary fixture");
    }
  }

  ~TemporaryBinaryFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

} // namespace

TEST(parse_minimal_contiguous_envelope) {
  auto bytes = header();
  bytes.insert(bytes.end(), {
                                0x01, // table count
                                0x07, // identifiers
                                0x00, // offset
                                0x01, // size
                                0x00, // table content
                                0x00, // module footer
                            });

  const auto envelope = movescape::parseEnvelope(bytes);
  REQUIRE_EQ(envelope.version, 5U);
  REQUIRE_EQ(envelope.tables.size(), 1U);
  REQUIRE_EQ(envelope.tables[0].kind, movescape::format::TableKind::Identifiers);
  REQUIRE_EQ(envelope.tables[0].offset, 0U);
  REQUIRE_EQ(envelope.tables[0].size, 1U);
  REQUIRE_EQ(envelope.table_content_offset, 12U);
  REQUIRE_EQ(envelope.table_content_size, 1U);
  REQUIRE_EQ(envelope.footer_offset, 13U);
  REQUIRE_EQ(envelope.tableBytes(bytes, envelope.tables[0]).size(), 1U);
}

TEST(normalize_aptos_version_mask) {
  auto bytes = header(0x0a00000aU);
  bytes.insert(bytes.end(), {0x00, 0x00});
  const auto envelope = movescape::parseEnvelope(bytes);
  REQUIRE_EQ(envelope.raw_version, 0x0a00000aU);
  REQUIRE_EQ(envelope.version, 10U);
  REQUIRE_EQ(envelope.table_content_size, 0U);
  REQUIRE_EQ(envelope.footer_offset, 9U);
}

TEST(envelope_records_caller_selected_binary_kind) {
  auto bytes = header();
  bytes.push_back(0x00); // table count

  const auto module = movescape::parseEnvelope(bytes);
  const auto script = movescape::parseEnvelope(bytes, movescape::BinaryKind::Script);
  REQUIRE_EQ(module.kind, movescape::BinaryKind::Module);
  REQUIRE_EQ(script.kind, movescape::BinaryKind::Script);
  REQUIRE_EQ(module.footer_offset, script.footer_offset);
}

TEST(script_envelope_accepts_every_common_table_kind) {
  using movescape::format::TableKind;
  const std::vector<TableKind> common{
      TableKind::ModuleHandles, TableKind::StructHandles, TableKind::FunctionHandles, TableKind::FunctionInstantiations,
      TableKind::Signatures,    TableKind::Constants,     TableKind::Identifiers,     TableKind::AddressIdentifiers,
      TableKind::Metadata,
  };
  for (const auto kind : common) {
    const auto bytes = singleTableEnvelope(7, kind);
    const auto envelope = movescape::parseEnvelope(bytes, movescape::BinaryKind::Script);
    REQUIRE_EQ(envelope.tables.at(0).kind, kind);
  }
}

TEST(script_envelope_rejects_every_module_only_table_kind) {
  using movescape::format::TableKind;
  const std::vector<TableKind> module_only{
      TableKind::StructDefinitions,    TableKind::StructDefinitionInstantiations,
      TableKind::FunctionDefinitions,  TableKind::FieldHandles,
      TableKind::FieldInstantiations,  TableKind::FriendDeclarations,
      TableKind::VariantFieldHandles,  TableKind::VariantFieldInstantiations,
      TableKind::StructVariantHandles, TableKind::StructVariantInstantiations,
  };
  for (const auto kind : module_only) {
    const auto bytes = singleTableEnvelope(7, kind);
    REQUIRE_ERROR(movescape::parseEnvelope(bytes, movescape::BinaryKind::Script), ErrorCode::InvalidTableLayout);
  }
}

TEST(enum_tables_require_bytecode_version_seven) {
  using movescape::format::TableKind;
  const std::vector<TableKind> enum_tables{
      TableKind::VariantFieldHandles,
      TableKind::VariantFieldInstantiations,
      TableKind::StructVariantHandles,
      TableKind::StructVariantInstantiations,
  };
  for (const auto kind : enum_tables) {
    const auto old_bytes = singleTableEnvelope(6, kind);
    REQUIRE_ERROR(movescape::parseEnvelope(old_bytes), ErrorCode::UnsupportedFeature);

    const auto supported_bytes = singleTableEnvelope(7, kind);
    const auto envelope = movescape::parseEnvelope(supported_bytes);
    REQUIRE_EQ(envelope.tables.at(0).kind, kind);
  }
}

TEST(reject_bad_magic) {
  auto bytes = header();
  bytes[0] = 0;
  bytes.push_back(0);
  REQUIRE_ERROR(movescape::parseEnvelope(bytes), ErrorCode::InvalidMagic);
}

TEST(reject_unsupported_version) {
  auto bytes = header(11);
  bytes.push_back(0);
  REQUIRE_ERROR(movescape::parseEnvelope(bytes), ErrorCode::UnsupportedVersion);
}

TEST(reject_unknown_table_kind) {
  auto bytes = header();
  bytes.insert(bytes.end(), {0x01, 0x09, 0x00, 0x01, 0x00});
  REQUIRE_ERROR(movescape::parseEnvelope(bytes), ErrorCode::UnknownTableKind);
}

TEST(reject_duplicate_table_kind) {
  auto bytes = header();
  bytes.insert(bytes.end(), {
                                0x02,
                                0x07,
                                0x00,
                                0x01,
                                0x07,
                                0x01,
                                0x01,
                                0x00,
                                0x00,
                            });
  REQUIRE_ERROR(movescape::parseEnvelope(bytes), ErrorCode::DuplicateTable);
}

TEST(reject_zero_sized_table) {
  auto bytes = header();
  bytes.insert(bytes.end(), {0x01, 0x07, 0x00, 0x00});
  REQUIRE_ERROR(movescape::parseEnvelope(bytes), ErrorCode::InvalidTableLayout);
}

TEST(reject_gap_before_first_table) {
  auto bytes = header();
  bytes.insert(bytes.end(), {0x01, 0x07, 0x01, 0x01, 0x00, 0x00});
  REQUIRE_ERROR(movescape::parseEnvelope(bytes), ErrorCode::InvalidTableLayout);
}

TEST(reject_gap_between_tables) {
  auto bytes = header();
  bytes.insert(bytes.end(), {
                                0x02,
                                0x07,
                                0x00,
                                0x01,
                                0x08,
                                0x02,
                                0x01,
                                0x00,
                                0x00,
                                0x00,
                            });
  REQUIRE_ERROR(movescape::parseEnvelope(bytes), ErrorCode::InvalidTableLayout);
}

TEST(reject_truncated_table_content) {
  auto bytes = header();
  bytes.insert(bytes.end(), {
                                0x01,
                                0x07,
                                0x00,
                                0x03,
                                0x00,
                                0x00,
                            });
  REQUIRE_ERROR(movescape::parseEnvelope(bytes), ErrorCode::UnexpectedEof);
}

TEST(custom_limits_reject_oversized_input_before_parsing) {
  auto bytes = header();
  bytes.insert(bytes.end(), {0x00, 0x00});
  movescape::ParserLimits limits;
  limits.max_file_bytes = bytes.size() - 1;
  REQUIRE_ERROR(movescape::parseEnvelope(bytes, limits), ErrorCode::ResourceLimit);
}

TEST(binary_file_reader_enforces_limit_before_loading_contents) {
  const std::vector<std::uint8_t> contents{1, 2, 3, 4};
  const TemporaryBinaryFile file(contents);

  movescape::ParserLimits exact_limits;
  exact_limits.max_file_bytes = contents.size();
  REQUIRE_EQ(movescape::readBinaryFile(file.path(), exact_limits), contents);

  movescape::ParserLimits small_limits;
  small_limits.max_file_bytes = contents.size() - 1;
  REQUIRE_ERROR(movescape::readBinaryFile(file.path(), small_limits), ErrorCode::ResourceLimit);
}

TEST(custom_limits_reject_table_count) {
  auto bytes = header();
  bytes.insert(bytes.end(), {
                                0x01, // table count
                                0x07, // identifiers
                                0x00, // offset
                                0x02, // size
                                0x01, // identifier length
                                'A',
                                0x00, // module footer
                            });
  movescape::ParserLimits limits;
  limits.max_table_count = 0;
  REQUIRE_ERROR(movescape::parseEnvelope(bytes, limits), ErrorCode::ResourceLimit);
}

TEST(custom_limits_reject_total_table_content) {
  auto bytes = header();
  bytes.insert(bytes.end(), {
                                0x01, // table count
                                0x07, // identifiers
                                0x00, // offset
                                0x02, // size
                                0x01, // identifier length
                                'A',
                                0x00, // module footer
                            });
  movescape::ParserLimits limits;
  limits.max_table_content_bytes = 1;
  REQUIRE_ERROR(movescape::parseEnvelope(bytes, limits), ErrorCode::ResourceLimit);
}
