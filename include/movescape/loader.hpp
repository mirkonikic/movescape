#pragma once

#include "movescape/format.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace movescape {

enum class BinaryKind { Module, Script };

struct ParserLimits {
  std::size_t max_file_bytes = 64U * 1024U * 1024U;
  std::size_t max_table_count = 255;
  std::size_t max_table_content_bytes = 64U * 1024U * 1024U;
  std::size_t max_table_entries = 65536;
  std::size_t max_list_elements = 65535;
  std::size_t max_signature_length = 255;
  std::size_t max_type_nesting = 256;
  std::size_t max_total_type_nodes = 1'000'000;
  std::size_t max_identifier_bytes = 255;
  std::size_t max_constant_bytes = 65535;
  std::size_t max_metadata_key_bytes = 1023;
  std::size_t max_metadata_value_bytes = 65535;
  std::size_t max_instructions_per_function = 65535;
  std::size_t max_total_instructions = 1'000'000;
};

struct TableHeader {
  format::TableKind kind;
  std::uint32_t offset;
  std::uint32_t size;
  std::size_t header_offset;
};

struct BinaryEnvelope {
  BinaryKind kind;
  std::uint32_t raw_version;
  std::uint32_t version;
  std::vector<TableHeader> tables;
  std::size_t table_content_offset;
  std::size_t table_content_size;
  std::size_t footer_offset;

  [[nodiscard]] std::span<const std::uint8_t> tableBytes(std::span<const std::uint8_t> file, const TableHeader &table) const;
};

[[nodiscard]] BinaryEnvelope parseEnvelope(std::span<const std::uint8_t> bytes, const ParserLimits &limits = {});
[[nodiscard]] BinaryEnvelope parseEnvelope(std::span<const std::uint8_t> bytes, BinaryKind kind, const ParserLimits &limits = {});
[[nodiscard]] std::vector<std::uint8_t> readBinaryFile(const std::filesystem::path &path, const ParserLimits &limits = {});

} // namespace movescape
