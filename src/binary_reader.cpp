#include "movescape/binary_reader.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <sstream>
#include <string>

namespace movescape {

namespace {

[[nodiscard]] std::string eofMessage(std::string_view field, std::size_t need, std::size_t remaining) {
  std::ostringstream out;
  out << "unexpected end of input while reading " << field << ": need " << need << " byte(s), have " << remaining;
  return out.str();
}

template <typename Integer> [[nodiscard]] Integer readWideInteger(BinaryReader &reader, std::string_view field) {
  Integer result;
  const auto bytes = reader.readBytes(result.little_endian_bytes.size(), field);
  std::copy(bytes.begin(), bytes.end(), result.little_endian_bytes.begin());
  return result;
}

} // namespace

std::string_view errorCodeName(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::Io:
    return "io";
  case ErrorCode::UnexpectedEof:
    return "unexpected-eof";
  case ErrorCode::InvalidMagic:
    return "invalid-magic";
  case ErrorCode::UnsupportedVersion:
    return "unsupported-version";
  case ErrorCode::InvalidUleb128:
    return "invalid-uleb128";
  case ErrorCode::ValueOutOfRange:
    return "value-out-of-range";
  case ErrorCode::UnknownTableKind:
    return "unknown-table-kind";
  case ErrorCode::UnknownSerializedType:
    return "unknown-serialized-type";
  case ErrorCode::UnknownOpcode:
    return "unknown-opcode";
  case ErrorCode::DuplicateTable:
    return "duplicate-table";
  case ErrorCode::InvalidTableLayout:
    return "invalid-table-layout";
  case ErrorCode::Malformed:
    return "malformed";
  case ErrorCode::InvalidIndex:
    return "invalid-index";
  case ErrorCode::UnsupportedFeature:
    return "unsupported-feature";
  case ErrorCode::ResourceLimit:
    return "resource-limit";
  case ErrorCode::IntegerOverflow:
    return "integer-overflow";
  case ErrorCode::InvalidArgument:
    return "invalid-argument";
  case ErrorCode::TypeMismatch:
    return "type-mismatch";
  case ErrorCode::InvalidLocalState:
    return "invalid-local-state";
  case ErrorCode::InvalidBorrowState:
    return "invalid-borrow-state";
  }
  return "unknown";
}

void BinaryReader::require(std::size_t count, std::string_view field) const {
  if (count > remaining()) {
    throw Error(ErrorCode::UnexpectedEof, absolutePosition(), eofMessage(field, count, remaining()));
  }
}

std::span<const std::uint8_t> BinaryReader::readBytes(std::size_t count, std::string_view field) {
  require(count, field);
  const auto result = bytes_.subspan(position_, count);
  position_ += count;
  return result;
}

std::uint8_t BinaryReader::readU8(std::string_view field) { return readBytes(1, field)[0]; }

std::uint16_t BinaryReader::readU16(std::string_view field) {
  const auto data = readBytes(2, field);
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8U));
}

std::uint32_t BinaryReader::readU32(std::string_view field) {
  const auto data = readBytes(4, field);
  std::uint32_t result = 0;
  for (std::size_t index = 0; index < data.size(); ++index) {
    result |= static_cast<std::uint32_t>(data[index]) << static_cast<unsigned>(index * 8U);
  }
  return result;
}

std::uint64_t BinaryReader::readU64(std::string_view field) {
  const auto data = readBytes(8, field);
  std::uint64_t result = 0;
  for (std::size_t index = 0; index < data.size(); ++index) {
    result |= static_cast<std::uint64_t>(data[index]) << static_cast<unsigned>(index * 8U);
  }
  return result;
}

UInt128 BinaryReader::readU128(std::string_view field) { return readWideInteger<UInt128>(*this, field); }
UInt256 BinaryReader::readU256(std::string_view field) { return readWideInteger<UInt256>(*this, field); }
std::int8_t BinaryReader::readI8(std::string_view field) { return std::bit_cast<std::int8_t>(readU8(field)); }
std::int16_t BinaryReader::readI16(std::string_view field) { return std::bit_cast<std::int16_t>(readU16(field)); }
std::int32_t BinaryReader::readI32(std::string_view field) { return std::bit_cast<std::int32_t>(readU32(field)); }
std::int64_t BinaryReader::readI64(std::string_view field) { return std::bit_cast<std::int64_t>(readU64(field)); }
Int128 BinaryReader::readI128(std::string_view field) { return readWideInteger<Int128>(*this, field); }
Int256 BinaryReader::readI256(std::string_view field) { return readWideInteger<Int256>(*this, field); }

std::uint64_t BinaryReader::readUleb128(std::uint64_t maximum, std::string_view field) {
  const auto start = absolutePosition();
  std::uint64_t value = 0;
  unsigned shift = 0;

  for (unsigned byte_index = 0; byte_index < 10; ++byte_index) {
    const auto byte = readU8(field);
    const auto payload = static_cast<std::uint64_t>(byte & 0x7fU);

    if (shift == 63U && payload > 1U) {
      throw Error(ErrorCode::InvalidUleb128, start, "ULEB128 value overflows 64 bits");
    }
    if (shift >= 64U && payload != 0U) {
      throw Error(ErrorCode::InvalidUleb128, start, "ULEB128 value overflows 64 bits");
    }

    if (shift < 64U) {
      value |= payload << shift;
    }

    if ((byte & 0x80U) == 0U) {
      if (byte_index > 0U && payload == 0U) {
        throw Error(ErrorCode::InvalidUleb128, start, "non-canonical ULEB128 encoding");
      }
      if (value > maximum) {
        std::ostringstream out;
        out << field << " value " << value << " exceeds maximum " << maximum;
        throw Error(ErrorCode::ValueOutOfRange, start, out.str());
      }
      return value;
    }
    shift += 7U;
  }

  throw Error(ErrorCode::InvalidUleb128, start, "ULEB128 encoding is longer than 10 bytes");
}

BinaryReader BinaryReader::subReader(std::size_t count, std::string_view field) {
  const auto start = absolutePosition();
  return BinaryReader(readBytes(count, field), start);
}

} // namespace movescape
