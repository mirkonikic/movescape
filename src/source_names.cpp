#include "movescape/source_names.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace movescape {

namespace {

constexpr std::array<std::string_view, 49> SourceKeywords{
    "abort", "acquires", "address", "as",  "bool",   "break",   "const",  "continue", "copy",   "else",   "entry",     "enum",
    "false", "friend",   "fun",     "has", "if",     "i8",      "i16",    "i32",      "i64",    "i128",   "i256",      "let",
    "loop",  "module",   "move",    "mut", "native", "phantom", "public", "return",   "script", "signer", "struct",    "true",
    "u8",    "u16",      "u32",     "u64", "u128",   "u256",    "use",    "vector",   "while",  "spec",   "invariant", "global",
};

[[nodiscard]] constexpr bool asciiLetter(char value) noexcept { return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z'); }

[[nodiscard]] constexpr bool sourceStart(char value) noexcept { return asciiLetter(value) || value == '_'; }

[[nodiscard]] constexpr bool sourceContinuation(char value) noexcept { return sourceStart(value) || (value >= '0' && value <= '9'); }

[[nodiscard]] bool keyword(std::string_view value) noexcept { return std::find(SourceKeywords.begin(), SourceKeywords.end(), value) != SourceKeywords.end(); }

} // namespace

bool isMoveSourceIdentifier(std::string_view value) noexcept {
  if (value.empty() || !sourceStart(value.front()) || keyword(value)) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), sourceContinuation);
}

std::string makeMoveSourceIdentifier(std::string_view value, std::string_view category, std::size_t index) {
  if (isMoveSourceIdentifier(value)) {
    return std::string(value);
  }

  std::string result(value);
  for (auto &character : result) {
    if (!sourceContinuation(character)) {
      character = '_';
    }
  }
  if (result.empty() || result.find_first_not_of('_') == std::string::npos) {
    result = std::string(category) + '_' + std::to_string(index);
  } else if (!sourceStart(result.front())) {
    result = std::string(category) + '_' + result;
  }
  if (keyword(result)) {
    result = std::string(category) + '_' + result;
  }
  return result;
}

} // namespace movescape
