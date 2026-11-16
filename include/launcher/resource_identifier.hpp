#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace launcher {

// Numeric resource identifiers and literal UTF-8 names retain their identity.
using ResourceIdentifier = std::variant<std::uint16_t, std::string>;

} // namespace launcher
