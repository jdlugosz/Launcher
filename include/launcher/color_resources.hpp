#pragma once

#include <string_view>

namespace launcher {

// Borrows the exact resource bytes from the running executable for its lifetime.
// The resource uses its visible TOML filename as its name.
std::string_view color_resource_text(std::string_view filename);

} // namespace launcher
