#pragma once

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace movescape {

enum class ErrorCode {
  Io,
  UnexpectedEof,
  InvalidMagic,
  UnsupportedVersion,
  InvalidUleb128,
  ValueOutOfRange,
  UnknownTableKind,
  UnknownSerializedType,
  UnknownOpcode,
  DuplicateTable,
  InvalidTableLayout,
  Malformed,
  InvalidIndex,
  UnsupportedFeature,
  ResourceLimit,
  IntegerOverflow,
  InvalidArgument,
  TypeMismatch,
  InvalidLocalState,
  InvalidBorrowState,
};

class Error final : public std::runtime_error {
public:
  static constexpr std::size_t UnknownOffset = std::numeric_limits<std::size_t>::max();

  Error(ErrorCode code, std::size_t offset, std::string message) : std::runtime_error(std::move(message)), code_(code), offset_(offset) {}

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] bool hasOffset() const noexcept { return offset_ != UnknownOffset; }

private:
  ErrorCode code_;
  std::size_t offset_;
};

[[nodiscard]] std::string_view errorCodeName(ErrorCode code) noexcept;

} // namespace movescape
