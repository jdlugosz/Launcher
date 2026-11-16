#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/color_resources.hpp"
#include "launcher/config_error.hpp"
#include "launcher/resource_module.hpp"
#include "launcher_resource.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace launcher {
std::string_view color_resource_text(std::string_view filename) {
    if (filename.empty() || !std::ranges::all_of(filename, [](unsigned char character) { return character >= 32 && character < 127; })) {
        throw ConfigError("Colors resource filename must be nonempty and contain only printable ASCII characters");
    }
    ResourceModule module(nullptr);
    return module.FetchText(std::string(LAUNCHER_TOML_RESOURCE), std::string(filename));
}

} // namespace launcher
