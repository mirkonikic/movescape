#pragma once

#include "movescape/loader.hpp"
#include "movescape/module.hpp"

#include <cstdint>
#include <span>

namespace movescape {

[[nodiscard]] Module loadModule(std::span<const std::uint8_t> bytes, const ParserLimits &limits = {});
[[nodiscard]] Script loadScript(std::span<const std::uint8_t> bytes, const ParserLimits &limits = {});

} // namespace movescape
