#pragma once

#include "movescape/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace movescape {

template <std::size_t BitCount, bool Signed> struct FixedWidthInteger {
  static_assert(BitCount == 128 || BitCount == 256);
  static constexpr std::size_t bit_count = BitCount;
  static constexpr std::size_t byte_count = BitCount / 8;
  static constexpr bool is_signed = Signed;

  // Move serializes fixed width integers LSB first
  std::array<std::uint8_t, byte_count> little_endian_bytes{};

  [[nodiscard]] constexpr bool negative() const noexcept
    requires(Signed)
  {
    return (little_endian_bytes.back() & 0x80U) != 0;
  }
  friend bool operator==(const FixedWidthInteger &, const FixedWidthInteger &) = default;
};

using UInt128 = FixedWidthInteger<128, false>;
using UInt256 = FixedWidthInteger<256, false>;
using Int128 = FixedWidthInteger<128, true>;
using Int256 = FixedWidthInteger<256, true>;

class BinaryReader {
public:
  explicit BinaryReader(std::span<const std::uint8_t> bytes, std::size_t absolute_base = 0) noexcept : bytes_(bytes), absolute_base_(absolute_base) {}
  [[nodiscard]] std::size_t position() const noexcept { return position_; }
  [[nodiscard]] std::size_t absolutePosition() const noexcept { return absolute_base_ + position_; }
  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - position_; }
  [[nodiscard]] bool empty() const noexcept { return remaining() == 0; }

  std::uint8_t readU8(std::string_view field = "u8");
  std::uint16_t readU16(std::string_view field = "u16");
  std::uint32_t readU32(std::string_view field = "u32");
  std::uint64_t readU64(std::string_view field = "u64");
  UInt128 readU128(std::string_view field = "u128");
  UInt256 readU256(std::string_view field = "u256");

  std::int8_t readI8(std::string_view field = "i8");
  std::int16_t readI16(std::string_view field = "i16");
  std::int32_t readI32(std::string_view field = "i32");
  std::int64_t readI64(std::string_view field = "i64");
  Int128 readI128(std::string_view field = "i128");
  Int256 readI256(std::string_view field = "i256");

  std::span<const std::uint8_t> readBytes(std::size_t count, std::string_view field = "bytes");
  std::uint64_t readUleb128(std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max(), std::string_view field = "ULEB128");
  BinaryReader subReader(std::size_t count, std::string_view field = "sub-reader");

private:
  void require(std::size_t count, std::string_view field) const;
  std::span<const std::uint8_t> bytes_;
  std::size_t position_ = 0;
  std::size_t absolute_base_ = 0;
};

} // namespace movescape
