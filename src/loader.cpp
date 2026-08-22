#include "movescape/loader.hpp"

#include "movescape/binary_reader.hpp"
#include "movescape/error.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <string>

namespace movescape {

namespace {

[[nodiscard]] std::string versionMessage(std::uint32_t version) {
  std::ostringstream out;
  out << "Move bytecode version " << version << " is unsupported; expected " << format::kMinimumSupportedVersion << ".." << format::kMaximumSupportedVersion;
  return out.str();
}

void requireWithinLimit(std::size_t value, std::size_t maximum, std::size_t offset, std::string_view subject) {
  if (value <= maximum) {
    return;
  }
  std::ostringstream out;
  out << subject << " " << value << " exceeds configured limit " << maximum;
  throw Error(ErrorCode::ResourceLimit, offset, out.str());
}

void validateTableAvailability(const std::vector<TableHeader> &tables, std::uint32_t version, BinaryKind kind) {
  for (const auto &table : tables) {
    const auto minimum = format::tableMinimumVersion(table.kind);
    if (version < minimum) {
      std::ostringstream out;
      out << format::tableKindName(table.kind) << " table requires bytecode version " << minimum << ", binary uses version " << version;
      throw Error(ErrorCode::UnsupportedFeature, table.header_offset, out.str());
    }
    if (kind == BinaryKind::Script && format::isModuleOnlyTable(table.kind)) {
      throw Error(ErrorCode::InvalidTableLayout, table.header_offset, "module-only table in script: " + std::string(format::tableKindName(table.kind)));
    }
  }
}

void validateTables(const std::vector<TableHeader> &tables, std::size_t content_offset, std::size_t file_size, std::size_t &content_size) {
  std::set<format::TableKind> kinds;
  for (const auto &table : tables) {
    // checking that there is no duplicate tables
    if (!kinds.insert(table.kind).second) {
      throw Error(ErrorCode::DuplicateTable, table.header_offset, "duplicate table kind: " + std::string(format::tableKindName(table.kind)));
    }
    // all tables need to have size
    if (table.size == 0U) {
      throw Error(ErrorCode::InvalidTableLayout, table.header_offset, "declared table has zero size: " + std::string(format::tableKindName(table.kind)));
    }
  }

  auto sorted = tables;
  // sorting tables
  std::sort(sorted.begin(), sorted.end(), [](const TableHeader &left, const TableHeader &right) { return left.offset < right.offset; });

  // checking that the offsets of tables are continuous
  std::uint64_t expected_offset = 0;
  for (const auto &table : sorted) {
    if (static_cast<std::uint64_t>(table.offset) != expected_offset) {
      std::ostringstream out;
      out << "table " << format::tableKindName(table.kind) << " begins at relative offset " << table.offset << ", expected contiguous offset "
          << expected_offset;
      throw Error(ErrorCode::InvalidTableLayout, table.header_offset, out.str());
    }
    if (expected_offset > std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(table.size)) {
      throw Error(ErrorCode::IntegerOverflow, table.header_offset, "table content range overflows");
    }
    expected_offset += static_cast<std::uint64_t>(table.size);
  }

  if (expected_offset > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    throw Error(ErrorCode::IntegerOverflow, content_offset, "table content does not fit host address space");
  }

  content_size = static_cast<std::size_t>(expected_offset);
  if (content_size > file_size - content_offset) {
    throw Error(ErrorCode::UnexpectedEof, content_offset, "declared table content extends past end of input");
  }
}

} // namespace

BinaryEnvelope parseEnvelope(std::span<const std::uint8_t> bytes, const ParserLimits &limits) { return parseEnvelope(bytes, BinaryKind::Module, limits); }

BinaryEnvelope parseEnvelope(std::span<const std::uint8_t> bytes, BinaryKind binary_kind, const ParserLimits &limits) {
  requireWithinLimit(bytes.size(), limits.max_file_bytes, 0, "input size");
  BinaryReader reader(bytes);
  const auto magic = reader.readBytes(format::kMagic.size(), "Move magic");
  if (!std::equal(magic.begin(), magic.end(), format::kMagic.begin())) {
    throw Error(ErrorCode::InvalidMagic, 0, "input does not begin with the Move bytecode magic");
  }

  const auto raw_version = reader.readU32("raw bytecode version");
  const auto version = raw_version & ~format::kAptosVersionMask;
  if (version < format::kMinimumSupportedVersion || version > format::kMaximumSupportedVersion) {
    throw Error(ErrorCode::UnsupportedVersion, format::kMagic.size(), versionMessage(version));
  }

  const auto table_count_value = reader.readUleb128(format::kMaximumTableCount, "table count");
  const auto table_count = static_cast<std::size_t>(table_count_value);
  requireWithinLimit(table_count, limits.max_table_count, format::kMagic.size() + sizeof(std::uint32_t), "table count");

  std::vector<TableHeader> tables;
  tables.reserve(table_count);
  for (std::size_t index = 0; index < table_count; ++index) {
    const auto header_offset = reader.absolutePosition();
    const auto kind_byte = reader.readU8("table kind");
    const auto kind = format::tableKindFromByte(kind_byte);
    if (!kind.has_value()) {
      std::ostringstream out;
      out << "unknown Move table kind 0x" << std::hex << static_cast<unsigned>(kind_byte);
      throw Error(ErrorCode::UnknownTableKind, header_offset, out.str());
    }

    const auto offset_value = reader.readUleb128(format::kMaximumTableOffset, "table offset");
    const auto size_value = reader.readUleb128(format::kMaximumTableSize, "table size");
    tables.push_back(TableHeader{
        .kind = *kind,
        .offset = static_cast<std::uint32_t>(offset_value),
        .size = static_cast<std::uint32_t>(size_value),
        .header_offset = header_offset,
    });
  }

  validateTableAvailability(tables, version, binary_kind);
  const auto table_content_offset = reader.absolutePosition();
  std::size_t table_content_size = 0;
  validateTables(tables, table_content_offset, bytes.size(), table_content_size);
  requireWithinLimit(table_content_size, limits.max_table_content_bytes, table_content_offset, "table content size");

  return BinaryEnvelope{
      .kind = binary_kind,
      .raw_version = raw_version,
      .version = version,
      .tables = std::move(tables),
      .table_content_offset = table_content_offset,
      .table_content_size = table_content_size,
      .footer_offset = table_content_offset + table_content_size,
  };
}

std::span<const std::uint8_t> BinaryEnvelope::tableBytes(std::span<const std::uint8_t> file, const TableHeader &table) const {
  const auto start = table_content_offset + static_cast<std::size_t>(table.offset);
  const auto count = static_cast<std::size_t>(table.size);
  if (start > file.size() || count > file.size() - start) {
    throw Error(ErrorCode::InvalidTableLayout, table.header_offset, "table byte range is outside the supplied file");
  }
  return file.subspan(start, count);
}

std::vector<std::uint8_t> readBinaryFile(const std::filesystem::path &path, const ParserLimits &limits) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw Error(ErrorCode::Io, 0, "unable to open input file: " + path.string());
  }

  // checking the file length
  input.seekg(0, std::ios::end);
  const auto end = input.tellg();
  if (end < 0) {
    throw Error(ErrorCode::Io, 0, "unable to determine input size: " + path.string());
  }
  input.seekg(0, std::ios::beg);

  // checking if the length of the file is too large
  const auto unsigned_size = static_cast<std::uintmax_t>(end);
  if (unsigned_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
    throw Error(ErrorCode::Io, 0, "input file is too large for this host: " + path.string());
  }
  if (unsigned_size > static_cast<std::uintmax_t>(limits.max_file_bytes)) {
    std::ostringstream out;
    out << "input size " << unsigned_size << " exceeds configured limit " << limits.max_file_bytes << ": " << path.string();
    throw Error(ErrorCode::ResourceLimit, 0, out.str());
  }

  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(unsigned_size));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
      throw Error(ErrorCode::Io, 0, "unable to read complete input file: " + path.string());
    }
  }
  return bytes;
}

} // namespace movescape
