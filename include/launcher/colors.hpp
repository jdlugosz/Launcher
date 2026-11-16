#pragma once

#include "launcher/config_error.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

class FileLookup;

using rgb_t = std::uint32_t;

struct ColorStop {
    float at;
    rgb_t rgb;
    bool operator==(const ColorStop&) const = default;
};

struct SurfaceColors {
    std::vector<ColorStop> gradient;
    rgb_t hover = 0;
};

struct ColorScheme {
    SurfaceColors tab;
    SurfaceColors page;
    rgb_t outline = 0;
    rgb_t text = 0x131313;
};

using ColorLibrary = std::map<std::string, ColorScheme, std::less<>>;
extern ColorLibrary color_library;

// Definitions and references are normalized when read, before map lookup.
std::string normalize_color_name(std::string_view name);
ColorScheme adhoc_color(rgb_t rgb);
const ColorScheme& find_color(std::string_view normalized_name);

// Each document is validated completely before publishing its definitions.
void parse_colors(std::string_view source, std::string_view source_name = {});
// All disk locations and bundled fallback follow the shared file search policy.
std::string load_colors(std::string_view filename, const FileLookup& files);
// Compatibility entry point for callers with an explicit configuration directory.
std::string load_colors(std::string_view filename, const std::filesystem::path& configuration_directory, const std::filesystem::path& user_directory);
std::string color_library_report(std::string_view missing_name, std::span<const std::string> sources = {});

} // namespace launcher
