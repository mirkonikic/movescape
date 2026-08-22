#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace movescape {

[[nodiscard]] bool isMoveSourceIdentifier(std::string_view value) noexcept;

// Converts a bytecode identifier into deterministic Move-source syntax. The
// category and index are used only when the original has no usable characters
// or is a reserved source keyword.
[[nodiscard]] std::string makeMoveSourceIdentifier(std::string_view value, std::string_view category, std::size_t index);

} // namespace movescape
