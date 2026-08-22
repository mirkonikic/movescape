#include "test.hpp"

#include "movescape/binary_reader.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using movescape::BinaryReader;
using movescape::ErrorCode;

TEST(read_fixed_width_little_endian_values) {
  const std::array<std::uint8_t, 15> bytes{
      0x7f, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01,
  };
  BinaryReader reader(bytes);
  REQUIRE_EQ(reader.readU8(), 0x7fU);
  REQUIRE_EQ(reader.readU16(), 0x1234U);
  REQUIRE_EQ(reader.readU32(), 0x12345678U);
  REQUIRE_EQ(reader.readU64(), 0x0123456789abcdefULL);
  REQUIRE(reader.empty());
}

TEST(read_signed_fixed_width_little_endian_values) {
  const std::array<std::uint8_t, 23> bytes{
      0xff,                                           // -1i8
      0x00, 0x80,                                     // minimum i16
      0xff, 0xff, 0xff, 0x7f,                         // maximum i32
      0x00, 0x00, 0x00, 0x00,                         // minimum i64
      0x00, 0x00, 0x00, 0x80, 0xff, 0xff, 0xff, 0xff, // maximum i64
      0xff, 0xff, 0xff, 0x7f,
  };
  BinaryReader reader(bytes);
  REQUIRE_EQ(reader.readI8(), -1);
  REQUIRE_EQ(reader.readI16(), std::numeric_limits<std::int16_t>::min());
  REQUIRE_EQ(reader.readI32(), std::numeric_limits<std::int32_t>::max());
  REQUIRE_EQ(reader.readI64(), std::numeric_limits<std::int64_t>::min());
  REQUIRE_EQ(reader.readI64(), std::numeric_limits<std::int64_t>::max());
  REQUIRE(reader.empty());
}

TEST(read_wide_fixed_width_integer_containers) {
  std::array<std::uint8_t, 96> bytes{};
  bytes[0] = 0x12;
  bytes[15] = 0x34;
  bytes[16] = 0x56;
  bytes[47] = 0x78;
  bytes[63] = 0x80;
  bytes[95] = 0x80;

  BinaryReader reader(bytes);
  const auto u128 = reader.readU128();
  const auto u256 = reader.readU256();
  const auto i128 = reader.readI128();
  const auto i256 = reader.readI256();

  REQUIRE_EQ(u128.little_endian_bytes.front(), 0x12U);
  REQUIRE_EQ(u128.little_endian_bytes.back(), 0x34U);
  REQUIRE_EQ(u256.little_endian_bytes.front(), 0x56U);
  REQUIRE_EQ(u256.little_endian_bytes.back(), 0x78U);
  REQUIRE(i128.negative());
  REQUIRE(i256.negative());
  REQUIRE(reader.empty());
}

TEST(wide_fixed_width_read_is_atomic_on_truncation) {
  const std::array<std::uint8_t, 15> bytes{};
  BinaryReader reader(bytes, 50);
  try {
    (void)reader.readU128("wide value");
    REQUIRE(false);
  } catch (const movescape::Error &error) {
    REQUIRE_EQ(error.code(), ErrorCode::UnexpectedEof);
    REQUIRE_EQ(error.offset(), 50U);
    REQUIRE_EQ(reader.position(), 0U);
  }
}

TEST(fixed_width_read_reports_absolute_subreader_offset) {
  const std::array<std::uint8_t, 3> bytes{0xaa, 0xbb, 0xcc};
  BinaryReader reader(bytes, 100);
  (void)reader.readU8();
  try {
    (void)reader.readU32();
    REQUIRE(false);
  } catch (const movescape::Error &error) {
    REQUIRE_EQ(error.code(), ErrorCode::UnexpectedEof);
    REQUIRE_EQ(error.offset(), 101U);
  }
}

TEST(read_canonical_uleb128_values) {
  const std::array<std::uint8_t, 7> bytes{
      0x00, 0x01, 0x7f, 0x80, 0x01, 0xff, 0x01,
  };
  BinaryReader reader(bytes);
  REQUIRE_EQ(reader.readUleb128(), 0U);
  REQUIRE_EQ(reader.readUleb128(), 1U);
  REQUIRE_EQ(reader.readUleb128(), 127U);
  REQUIRE_EQ(reader.readUleb128(), 128U);
  REQUIRE_EQ(reader.readUleb128(), 255U);
}

TEST(reject_noncanonical_uleb128) {
  const std::array<std::uint8_t, 2> bytes{0x80, 0x00};
  BinaryReader reader(bytes);
  REQUIRE_ERROR(reader.readUleb128(), ErrorCode::InvalidUleb128);
}

TEST(reject_overflowing_uleb128) {
  const std::array<std::uint8_t, 10> bytes{
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02,
  };
  BinaryReader reader(bytes);
  REQUIRE_ERROR(reader.readUleb128(), ErrorCode::InvalidUleb128);
}

TEST(reject_too_long_uleb128) {
  const std::array<std::uint8_t, 10> bytes{
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  };
  BinaryReader reader(bytes);
  REQUIRE_ERROR(reader.readUleb128(), ErrorCode::InvalidUleb128);
}

TEST(reject_uleb128_above_field_maximum) {
  const std::array<std::uint8_t, 2> bytes{0x80, 0x01};
  BinaryReader reader(bytes);
  REQUIRE_ERROR(reader.readUleb128(127), ErrorCode::ValueOutOfRange);
}

TEST(subreader_is_bounded_and_tracks_absolute_offsets) {
  const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
  BinaryReader reader(bytes);
  (void)reader.readU8();
  auto sub = reader.subReader(2);
  REQUIRE_EQ(sub.readU8(), 2U);
  REQUIRE_EQ(sub.readU8(), 3U);
  try {
    (void)sub.readU8();
    REQUIRE(false);
  } catch (const movescape::Error &error) {
    REQUIRE_EQ(error.offset(), 3U);
  }
  REQUIRE_EQ(reader.readU8(), 4U);
}
