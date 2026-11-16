#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/colors.hpp"
#include "launcher/config_text.hpp"
#include "launcher/file_lookup.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <set>
#include <utility>

namespace launcher {

ColorLibrary color_library;

namespace {

[[noreturn]] void invalid(std::string_view location, std::string_view message) {
    throw ConfigError(std::string(location) + ": " + std::string(message));
}

char lower(char character) noexcept {
    return character >= 'A' && character <= 'Z'
        ? static_cast<char>(character - 'A' + 'a') : character;
}

void check_keys(const toml::table& table, std::initializer_list<std::string_view> allowed, std::string_view location) {
    for (const auto& [key, value] : table) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end()) {
            invalid(location, "unknown setting '" + std::string(key.str()) + "'");
        }
    }
}

std::optional<rgb_t> hex_rgb(std::string_view text) noexcept {
    if (text.size() != 7 || text.front() != '#') { return std::nullopt; }
    rgb_t result = 0;
    for (const char character : text.substr(1)) {
        unsigned digit = 0;
        if (character >= '0' && character <= '9') {
            digit = static_cast<unsigned>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            digit = static_cast<unsigned>(character - 'a') + 10U;
        } else {
            return std::nullopt;
        }
        result = (result << 4) | digit;
    }
    return result;
}

rgb_t read_rgb(const toml::node& node, std::string_view location) {
    const auto text = node.value<std::string>();
    if (!text) { invalid(location, "color must use #RRGGBB hexadecimal syntax"); }
    std::string normalized(*text);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), lower);
    const auto rgb = hex_rgb(normalized);
    if (!rgb) { invalid(location, "color must use #RRGGBB hexadecimal syntax with six hexadecimal digits"); }
    return *rgb;
}

void validate_gradient(const std::vector<ColorStop>& stops, std::string_view location) {
    if (stops.size() < 2 || stops.size() > 16) {
        invalid(location, "gradient needs 2 through 16 stops");
    }
    float previous = -1.0F;
    for (const auto& stop : stops) {
        if (!std::isfinite(stop.at) || stop.at < 0.0F || stop.at > 1.0F || stop.at <= previous) {
            invalid(location, "stop positions must be finite, strictly increasing, and within 0 through 1");
        }
        if (stop.rgb > 0xffffffU) { invalid(location, "stop color must fit #RRGGBB"); }
        previous = stop.at;
    }
    if (stops.front().at != 0.0F || stops.back().at != 1.0F) {
        invalid(location, "gradient must start at 0 and end at 1");
    }
}

void validate_scheme(const ColorScheme& scheme, std::string_view location) {
    validate_gradient(scheme.tab.gradient, std::string(location) + ".tab.gradient");
    validate_gradient(scheme.page.gradient, std::string(location) + ".page.gradient");
    if (scheme.tab.hover > 0xffffffU || scheme.page.hover > 0xffffffU) {
        invalid(location, "hover colors must fit #RRGGBB");
    }
    if (scheme.outline > 0xffffffU) { invalid(location, "outline must fit #RRGGBB"); }
    if (scheme.text > 0xffffffU) { invalid(location, "text must fit #RRGGBB"); }
}

std::vector<ColorStop> read_gradient(const toml::node& node, std::string_view location) {
    const auto* array = node.as_array();
    if (!array || array->size() < 2 || array->size() > 16) {
        invalid(location, "gradient must be an array of 2 through 16 stop tables");
    }
    std::vector<ColorStop> result;
    result.reserve(array->size());
    for (const auto& entry : *array) {
        const auto* table = entry.as_table();
        if (!table) { invalid(location, "gradient stops must be tables"); }
        check_keys(*table, {"at", "color"}, location);
        const auto* at = table->get("at");
        const auto* color = table->get("color");
        const auto value = at ? at->value<double>() : std::nullopt;
        if (!at || (!at->is_integer() && !at->is_floating_point()) || !value ||
                !std::isfinite(*value) || *value < 0.0 || *value > 1.0 || !color) {
            invalid(location, "each stop requires finite 'at' from 0 through 1 and a 'color'");
        }
        if ((result.empty() && *value != 0.0) ||
                (result.size() + 1 == array->size() && *value != 1.0)) {
            invalid(location, "gradient must start at 0 and end at 1");
        }
        result.push_back({static_cast<float>(*value), read_rgb(*color, location)});
    }
    validate_gradient(result, location);
    return result;
}

rgb_t mix(rgb_t rgb, float white, float scale = 1.0F) {
    rgb_t result = 0;
    for (unsigned shift : {16U, 8U, 0U}) {
        const float channel = static_cast<float>((rgb >> shift) & 0xffU);
        const auto value = static_cast<rgb_t>(std::lround((channel * (1.0F - white) + 255.0F * white) * scale));
        result |= value << shift;
    }
    return result;
}

ColorScheme palette(rgb_t top, rgb_t middle, rgb_t foot, rgb_t far, rgb_t outline) {
    // Spread the page transition over its full extent. A near-origin white
    // highlight becomes a sharp diagonal band in the oblique page brush.
    return {{{{0.0F, top}, {0.45F, middle}, {1.0F, foot}}, top},
        {{{0.0F, foot}, {1.0F, far}}, mix(outline, 0.0F, 0.65F)}, outline};
}

void read_surface(const toml::node& node, SurfaceColors& surface, std::string_view location) {
    const toml::node* gradient = &node;
    const toml::node* hover = nullptr;
    if (const auto* table = node.as_table()) {
        check_keys(*table, {"gradient", "hover"}, location);
        gradient = table->get("gradient");
        hover = table->get("hover");
    }
    if (gradient) {
        const bool default_hover = surface.gradient.empty();
        surface.gradient = read_gradient(*gradient, std::string(location) + ".gradient");
        // A new surface gets a contrasting default. Partial overrides preserve
        // every unspecified leaf, including hover when replacing a gradient.
        if (default_hover) {
            const auto rgb = surface.gradient.front().rgb;
            const auto brightness = ((rgb >> 16) & 255) * 0.2126F +
                ((rgb >> 8) & 255) * 0.7152F + (rgb & 255) * 0.0722F;
            surface.hover = brightness > 160.0F ? mix(rgb, 0.0F, 0.45F) : mix(rgb, 0.50F);
        }
    }
    if (surface.gradient.empty()) { invalid(location, "a new surface requires 'gradient'"); }
    if (hover) { surface.hover = read_rgb(*hover, std::string(location) + ".hover"); }
}

void read_colors(const toml::table& root, std::string_view source_name) {
    const std::string prefix = source_name.empty() ? std::string{} : std::string(source_name) + ": ";
    check_keys(root, {"colors"}, prefix + "colors file");
    const auto* node = root.get("colors");
    if (!node) { return; }
    const auto* table = node->as_table();
    if (!table) { invalid(prefix + "colors", "must be a table of named palettes"); }

    std::set<std::string, std::less<>> seen;
    std::vector<std::pair<std::string, ColorScheme>> pending;
    pending.reserve(table->size());
    for (const auto& [key, entry] : *table) {
        const std::string name = normalize_color_name(key.str());
        const std::string location = prefix + "colors." + name;
        if (!seen.insert(name).second) { invalid(location, "duplicate normalized color name"); }
        const auto* definition = entry.as_table();
        if (!definition) { invalid(location, "palette must be a table"); }
        check_keys(*definition, {"partial", "tab", "page", "outline", "text"}, location);

        bool partial = false;
        if (const auto* flag = definition->get("partial")) {
            const auto value = flag->value<bool>();
            if (!flag->is_boolean() || !value) { invalid(location + ".partial", "must be a boolean"); }
            partial = *value;
        }
        ColorScheme scheme;
        if (partial) {
            const auto existing = color_library.find(name);
            if (existing == color_library.end()) { invalid(location, "partial override requires an existing color name"); }
            scheme = existing->second;
        } else if (!definition->contains("tab") || !definition->contains("page") || !definition->contains("outline")) {
            invalid(location, "complete palettes require 'tab', 'page' and 'outline'; use partial = true to change selected fields");
        }
        if (const auto* tab = definition->get("tab")) { read_surface(*tab, scheme.tab, location + ".tab"); }
        if (const auto* page = definition->get("page")) { read_surface(*page, scheme.page, location + ".page"); }
        if (const auto* outline = definition->get("outline")) { scheme.outline = read_rgb(*outline, location + ".outline"); }
        if (const auto* text = definition->get("text")) { scheme.text = read_rgb(*text, location + ".text"); }
        validate_scheme(scheme, location);
        pending.emplace_back(name, std::move(scheme));
    }
    for (auto& [name, scheme] : pending) { color_library.insert_or_assign(std::move(name), std::move(scheme)); }
}

std::string color_filename(std::string_view filename) {
    const auto name = normalize_color_name(filename);
    for (const char character : name) {
        if (std::string_view("<>:\"/\\|?*").find(character) != std::string_view::npos) {
            invalid("color set", "filename contains an unsafe character");
        }
    }
    if (name.size() <= 5 || !name.ends_with(".toml")) {
        invalid("color set", "filename must have a name followed by .toml");
    }
    return name;
}

} // namespace

std::string normalize_color_name(std::string_view name) {
    if (name.empty()) { invalid("color name", "must not be empty"); }
    std::string result;
    result.reserve(name.size());
    for (std::size_t index = 0; index < name.size(); ++index) {
        const auto byte = static_cast<unsigned char>(name[index]);
        if (byte > 127) {
            invalid("color name", "non-ASCII byte " + std::to_string(byte) + " at byte " + std::to_string(index + 1));
        }
        if (byte < 32 || byte == 127) {
            invalid("color name", "control byte " + std::to_string(byte) + " at byte " + std::to_string(index + 1));
        }
        result.push_back(lower(static_cast<char>(byte)));
    }
    return result;
}

ColorScheme adhoc_color(rgb_t rgb) {
    if (rgb > 0xffffffU) { invalid("color", "ad hoc color must fit #RRGGBB"); }
    return palette(mix(rgb, 0.4F), mix(rgb, 0.18F), rgb, mix(rgb, 0.42F), mix(rgb, 0.0F, 0.65F));
}

const ColorScheme& find_color(std::string_view normalized_name) {
    if (const auto found = color_library.find(normalized_name); found != color_library.end()) { return found->second; }
    if (const auto rgb = hex_rgb(normalized_name)) {
        return color_library.emplace(std::string(normalized_name), adhoc_color(*rgb)).first->second;
    }
    throw UnknownColorError(std::string(normalized_name));
}

void parse_colors(std::string_view source, std::string_view source_name) {
    read_colors(config_text_builder::from_text(source, source_name).parse(), source_name);
}

std::string load_colors(std::string_view filename_text, const FileLookup& files) {
    const auto filename = color_filename(filename_text);
    const bool bundled = filename == "basic_colors.toml" || filename == "extra_colors.toml" ||
        filename == "colors-light.toml" || filename == "colors-dark.toml";
    const auto loaded = files.read_text(filename, bundled ? FileFallback::embedded_resource : FileFallback::none);
    try { parse_colors(loaded.text, loaded.source); }
    catch (ConfigError& error) {
        error.source_file = loaded.file;
        throw;
    }
    return loaded.source;
}

std::string load_colors(std::string_view filename, const std::filesystem::path& configuration_directory, const std::filesystem::path& user_directory) {
    return load_colors(filename, FileLookup({}, user_directory, configuration_directory, true));
}

std::string color_library_report(std::string_view missing_name, std::span<const std::string> sources) {
    std::string result = "Unknown color: '" + std::string(missing_name) + "'\n\nDefined colors (" +
        std::to_string(color_library.size()) + "):\n";
    for (const auto& [name, scheme] : color_library) {
        (void)scheme;
        result += "  " + name + "\n";
    }
    if (std::ranges::any_of(sources, [](const auto& source) { return source.find("!resource:") != std::string::npos; })) {
        result += "\nThe embedded color definitions are also supplied as readable TOML files in the examples directory beside the running executable.\n";
    }
    return result;
}

} // namespace launcher
