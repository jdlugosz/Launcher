#include <windows.h>
#include <shlobj.h>

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/config.hpp"
#include "launcher/config_text.hpp"
#include "launcher/environment_text.hpp"
#include "launcher/file_lookup.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace launcher {

using std::size_t;
using std::string;
using std::string_view;

namespace {

std::mutex color_configuration_mutex;

[[noreturn]] void invalid(string_view location, string_view message) {
    throw ConfigError(string(location) + ": " + string(message));
}

void check_keys(const toml::table& table,
                std::initializer_list<string_view> allowed,
                string_view location) {
    for (const auto& [key, _] : table) {
        if (std::none_of(allowed.begin(), allowed.end(), [&key](string_view name) { return name == key.str(); })) {
            invalid(location, "unknown setting '" + string(key.str()) + "'");
        }
    }
}

string read_string(const toml::table& table,
                        string_view key,
                        string_view location,
                        string fallback = {}) {
    const auto* node = table.get(key);
    if (!node) {
        return fallback;
    }
    if (!node->is_string()) {
        invalid(location, "'" + string(key) + "' must be a string");
    }
    return *node->value<string>();
}

string read_launch_string(const toml::table& table,
                               string_view key,
                               string_view location,
                               string fallback = {}) {
    auto value = read_string(table, key, location, std::move(fallback));
    if (value.find('\0') != string::npos) {
        invalid(location, "'" + string(key) + "' contains embedded NUL");
    }
    return value;
}

constexpr bool identifier_start(char character) noexcept {
    return (character >= 'A' && character <= 'Z') ||
        (character >= 'a' && character <= 'z') || character == '_';
}

constexpr bool identifier_character(char character) noexcept {
    return identifier_start(character) || (character >= '0' && character <= '9');
}

enum class TextUse { path, arguments, directory };

string decode_dollar_symbols(string_view value) {
    string result;
    result.reserve(value.size());
    for (size_t position = 0; position < value.size(); ++position) {
        if (value[position] == '$' && position + 1 < value.size()) {
            const auto symbol = value[position + 1];
            if (symbol == '$' || symbol == '/') {
                result += symbol == '$' ? '$' : '%';
                ++position;
                continue;
            }
        }
        result += value[position];
    }
    return result;
}

string read_path_string(const toml::table& table, string_view key,
    string_view location, const toml::table* paths, TextUse use = TextUse::path) {
    const auto value = read_launch_string(table, key, location);
    string expanded;
    size_t position = 0;
    const size_t prefix = use == TextUse::arguments && !value.empty() && value.front() == '"' ? 1 : 0;
    if (use != TextUse::directory && value.size() > prefix + 1 && value[prefix] == '$' && identifier_start(value[prefix + 1])) {
        size_t end = prefix + 2;
        while (end < value.size() && identifier_character(value[end])) { ++end; }
        const auto identifier = string_view(value).substr(prefix + 1, end - prefix - 1);
        const auto* replacement = paths ? paths->get(identifier) : nullptr;
        if (!replacement) {
            invalid(location, "'" + string(key) + "' refers to undefined path '" +
                string(identifier) + "'");
        }
        expanded = string_view(value).substr(0, prefix);
        expanded += *replacement->value<string>();
        position = end;
    }
    // Validate item syntax without decoding it. Inserted values are data, and
    // named macros are never recognized again after the leading substitution.
    for (size_t check = position; use != TextUse::directory && check < value.size(); ++check) {
        if (value[check] != '$') { continue; }
        if (check + 1 < value.size() && (value[check + 1] == '$' || value[check + 1] == '/')) {
            ++check;
        } else {
            invalid(location, "'" + string(key) + "' uses a reserved '$'; "
                "use '$$' for a literal dollar sign or '$/' for a literal percent");
        }
    }
    expanded += string_view(value).substr(position);
    // Keep $/ and $$ intact for the complete Windows environment pass. Decode
    // symbols only afterward, once; neither replacement is rescanned.
    if (use != TextUse::arguments) { expanded = expand_environment_text(expanded); }
    return decode_dollar_symbols(expanded);
}

IconSource read_icon_source(const toml::table& item, string_view location, const toml::table* paths)
{
    const auto* node = item.get("icon");
    if (!node) { return StockIcon::application; }
    const auto icon_location = string(location) + ".icon";
    const auto file_path = [&](const toml::table& table, string_view key) {
        const auto field_location = key == "icon" ? location : string_view(icon_location);
        const auto text = read_path_string(table, key, field_location, paths);
        if (text.empty()) { invalid(icon_location, "'" + string(key) + "' must not be empty"); }
        // Construct from UTF-8 explicitly, independently of Windows' ANSI code page.
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    };
    if (node->is_string()) {
        return FileIcon{file_path(item, "icon")};
    }
    const auto* table = node->as_table();
    if (!table) { invalid(icon_location, "must be a filename string or a table"); }
    check_keys(*table, {"stock", "file", "index", "id", "type", "name", "language"}, icon_location);
    if (table->contains("stock")) {
        if (table->size() != 1) { invalid(icon_location, "'stock' cannot be combined with file or icon selectors"); }
        const auto name = read_launch_string(*table, "stock", icon_location);
        const auto stock = stock_icon_named(name);
        if (!stock) { invalid(icon_location, "unknown stock icon '" + name + "'"); }
        return *stock;
    }
    if (!table->contains("file")) { invalid(icon_location, "requires 'stock' or 'file'"); }
    auto path = file_path(*table, "file");
    if (table->contains("type")) {
        const auto numeric = [&](string_view key) {
            const auto* value = table->get(key);
            const auto number = value->value<std::int64_t>();
            if (!value->is_integer() || !number || *number < 0 || *number > 65535) {
                invalid(icon_location, "'" + string(key) + "' must be an integer from 0 through 65535");
            }
            return static_cast<std::uint16_t>(*number);
        };
        const auto named = [&](string_view key) {
            auto text = read_launch_string(*table, key, icon_location);
            if (text.empty() || text.front() == '#') {
                invalid(icon_location, "'" + string(key) + "' must be a nonempty literal name without a leading '#'; use integers for numeric IDs");
            }
            return text;
        };
        if (table->contains("index") || table->contains("name") == table->contains("id")) {
            invalid(icon_location, "a resource 'type' requires exactly one of 'name' or 'id', and cannot use 'index'");
        }
        ResourceIdentifier type = table->get("type")->is_string() ? ResourceIdentifier{named("type")} : ResourceIdentifier{numeric("type")};
        ResourceIdentifier name = table->contains("name") ? ResourceIdentifier{named("name")} : ResourceIdentifier{numeric("id")};
        const auto language = table->contains("language") ? std::optional{numeric("language")} : std::nullopt;
        return PeResourceIcon{std::move(path), std::move(type), std::move(name), language};
    }
    if (table->contains("name") || table->contains("language")) {
        invalid(icon_location, "'name' and 'language' require a resource 'type'");
    }
    const bool has_index = table->contains("index");
    const bool has_id = table->contains("id");
    if (has_index && has_id) { invalid(icon_location, "specify either 'index' or 'id', not both"); }
    if (!has_index && !has_id) { return FileIcon{std::move(path)}; }
    const auto key = has_index ? "index" : "id";
    const auto* selector = table->get(key);
    const auto maximum = has_index ? std::numeric_limits<int>::max() : 65535;
    const auto number = selector->value<std::int64_t>();
    if (!selector->is_integer() || !number || *number < 0 || *number > maximum) {
        invalid(icon_location, "'" + string(key) + "' must be an integer from 0 through " + std::to_string(maximum));
    }
    if (has_index) { return IconGroup{std::move(path), IconIndex{static_cast<int>(*number)}}; }
    return IconGroup{std::move(path), IconResourceId{static_cast<std::uint16_t>(*number)}};
}

bool read_boolean(const toml::table& table,
                  string_view key,
                  string_view location,
                  bool fallback) {
    const auto* node = table.get(key);
    if (!node) {
        return fallback;
    }
    if (!node->is_boolean()) {
        invalid(location, "'" + string(key) + "' must be a Boolean");
    }
    return *node->value<bool>();
}

double read_number(const toml::table& table, string_view key,
                   string_view location, double fallback,
                   double minimum, double maximum, bool positive = false) {
    const auto* node = table.get(key);
    if (!node) {
        return fallback;
    }
    const auto value = node->value<double>();
    if ((!node->is_integer() && !node->is_floating_point()) || !value ||
        !std::isfinite(*value) || *value < minimum || *value > maximum ||
        (positive && (*value <= 0 || static_cast<float>(*value) <= 0))) {
        std::ostringstream description;
        description << "'" << key << "' must be finite "
                    << (positive ? "and positive, at most " : "and in the range ")
                    << (positive ? maximum : minimum);
        if (!positive) {
            description << " through " << maximum;
        }
        invalid(location, description.str());
    }
    return *value;
}

std::uint32_t read_delay(const toml::table& table,
                         string_view key,
                         string_view location,
                         std::uint32_t fallback,
                         std::uint32_t maximum) {
    const auto* node = table.get(key);
    if (!node) {
        return fallback;
    }
    if (node->is_integer()) {
        const auto value = *node->value<std::int64_t>();
        if (value >= 0 && value <= static_cast<std::int64_t>(maximum)) {
            return static_cast<std::uint32_t>(value);
        }
    }
    invalid(location, "'" + string(key) + "' must be an integer from 0 to " +
                        std::to_string(maximum) + " milliseconds");
}

std::uint32_t read_item_flags(const toml::table& table, string_view location) {
    const auto* node = table.get("flags");
    if (!node) { return 0; }
    const auto value = node->value<std::int64_t>();
    if (!node->is_integer() || !value || *value < 0 || *value > 0xffffffffLL) {
        invalid(location, "'flags' must be an integer from 0 through 4294967295");
    }
    return static_cast<std::uint32_t>(*value);
}

std::uint32_t read_spacing_pixels(const toml::table& table) {
    const auto* node = table.get("icon_spacing_px");
    const auto value = node ? node->value<std::int64_t>() : std::nullopt;
    if (!node || !node->is_integer() || !value || *value < 0 ||
        *value > static_cast<std::int64_t>(max_icon_spacing_px)) {
        invalid("grid", "'icon_spacing_px' must be an integer from 0 through " +
                            std::to_string(max_icon_spacing_px));
    }
    return static_cast<std::uint32_t>(*value);
}

std::optional<size_t> read_dimension(const toml::table& table,
                                          string_view key,
                                          std::optional<size_t> fallback) {
    const auto* node = table.get(key);
    if (!node) {
        return fallback;
    }
    if (node->is_string() && node->value<string>() == "auto") {
        return std::nullopt;
    }
    if (node->is_integer()) {
        const auto value = *node->value<std::int64_t>();
        if (value > 0 && static_cast<std::uint64_t>(value) <= max_grid_dimension) {
            return static_cast<size_t>(value);
        }
    }
    invalid("grid", "'" + string(key) + "' must be an integer from 1 to " +
                        std::to_string(max_grid_dimension) + " or 'auto'");
}

size_t cell_count(const Tab& tab) {
    return static_cast<size_t>(std::count_if(
        tab.items.begin(), tab.items.end(),
        [](const Item& item) { return item.kind != ItemKind::line_break; }));
}

struct LayoutAttempt {
    TabLayout layout;
    bool overflow = false;
};

LayoutAttempt place_tab(const Tab& tab,
                        Flow flow,
                        size_t run_capacity,
                        std::optional<size_t> cross_capacity) {
    LayoutAttempt result;
    result.layout.placements.reserve(cell_count(tab));
    size_t run_position = 0;
    size_t cross_position = 0;

    for (size_t index = 0; index < tab.items.size(); ++index) {
        if (tab.items[index].kind == ItemKind::line_break) {
            if (run_position != 0) {
                run_position = 0;
                ++cross_position;
            }
            continue;
        }
        if (cross_capacity && cross_position >= *cross_capacity) {
            result.overflow = true;
            return result;
        }

        const auto column = flow == Flow::row_major ? run_position : cross_position;
        const auto row = flow == Flow::row_major ? cross_position : run_position;
        result.layout.placements.push_back({index, column, row});
        result.layout.used_columns = std::max(result.layout.used_columns, column + 1);
        result.layout.used_rows = std::max(result.layout.used_rows, row + 1);

        ++run_position;
        if (run_position == run_capacity) {
            run_position = 0;
            ++cross_position;
        }
    }
    return result;
}

bool all_tabs_fit(const Configuration& configuration,
                  size_t run_capacity,
                  size_t cross_capacity) {
    return std::all_of(configuration.tabs.begin(), configuration.tabs.end(),
                       [&](const Tab& tab) {
                           return !place_tab(tab, configuration.grid.flow,
                                             run_capacity, cross_capacity).overflow;
                       });
}

// Section and item readers for parse_configuration.
std::filesystem::path process_executable_path() {
    std::wstring filename(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, filename.data(), static_cast<DWORD>(filename.size()));
    if (!length || length >= filename.size()) {
        throw ConfigError("Cannot locate the running executable for '$EXE' and '$SELF'");
    }
    filename.resize(length);
    return std::filesystem::path(filename);
}

std::filesystem::path process_user_data_directory() {
    PWSTR name = nullptr;
    const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &name);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owner(name, CoTaskMemFree);
    if (FAILED(result) || !name) {
        throw ConfigError("Cannot locate the Launcher profile for '$USERDATA': " + std::system_category().message(result));
    }
    return std::filesystem::path(name) / L"Długosz" / L"Launcher";
}

string literal_path_prefix(const std::filesystem::path& path) {
    string result;
    // Actual executable and profile paths are data. Protect their symbols from the
    // environment expansion and final dollar decoding used by authored prefixes.
    for (const auto character : path.u8string()) {
        if (character == u8'$') { result += "$$"; }
        else if (character == u8'%') { result += "$/"; }
        else { result += static_cast<char>(character); }
    }
    return result;
}

toml::table read_paths(const toml::table& document, const FileLookup& files) {
    const auto* paths = document["paths"].as_table();
    if (document.contains("paths") && !paths) {
        invalid("configuration", "'paths' must be a table");
    }
    toml::table result = paths ? *paths : toml::table{};
    for (const auto& [key, _] : result) {
        const auto identifier = key.str();
        if (identifier == "EXE" || identifier == "SELF" || identifier == "USERDATA") {
            invalid("paths", "'" + string(identifier) + "' is built in and cannot be redefined");
        }
        if (identifier.empty() || !identifier_start(identifier.front()) ||
            !std::all_of(identifier.begin() + 1, identifier.end(), identifier_character)) {
            invalid("paths", "'" + string(identifier) + "' must be an identifier "
                "starting with a letter or underscore, followed by letters, digits or underscores");
        }
        (void)read_launch_string(result, identifier, "paths");
    }
    const auto executable = files.executable().empty() ? process_executable_path() :
        std::filesystem::absolute(files.executable()).lexically_normal();
    const auto user_directory = files.user_directory().empty() ? process_user_data_directory() :
        std::filesystem::absolute(files.user_directory()).lexically_normal();
    result.insert("EXE", literal_path_prefix(executable.parent_path()));
    result.insert("SELF", literal_path_prefix(executable));
    result.insert("USERDATA", literal_path_prefix(user_directory));
    return result;
}

void read_dock(const toml::table& document, DockOptions& options) {
    const auto* dock_node = document.get("dock");
    if (!dock_node) { return; }
    const auto* dock = dock_node->as_table();
    if (!dock) {
        invalid("configuration", "'dock' must be a table");
    }
    check_keys(*dock, {"edge", "display", "auto_hide", "hover_delay_ms",
                       "hide_delay_ms", "reveal_duration_ms", "hide_duration_ms",
                       "suppress_fullscreen", "position"}, "dock");
    const auto edge = read_string(*dock, "edge", "dock", "right");
    if (edge == "top") {
        options.edge = Edge::top;
    } else if (edge == "bottom") {
        options.edge = Edge::bottom;
    } else if (edge == "left") {
        options.edge = Edge::left;
    } else if (edge == "right") {
        options.edge = Edge::right;
    } else {
        invalid("dock", "'edge' must be 'top', 'bottom', 'left', or 'right'");
    }
    options.display = read_string(*dock, "display", "dock", "primary");
    if (options.display.empty()) {
        invalid("dock", "'display' must be 'primary' or a nonempty display name");
    }
    options.auto_hide = read_boolean(*dock, "auto_hide", "dock", options.auto_hide);
    options.hover_delay_ms = read_delay(*dock, "hover_delay_ms", "dock", options.hover_delay_ms, max_dock_delay_ms);
    options.hide_delay_ms = read_delay(*dock, "hide_delay_ms", "dock", options.hide_delay_ms, max_dock_delay_ms);
    options.reveal_duration_ms = read_delay(*dock, "reveal_duration_ms", "dock", options.reveal_duration_ms, max_dock_delay_ms);
    options.hide_duration_ms = read_delay(*dock, "hide_duration_ms", "dock", options.reveal_duration_ms, max_dock_delay_ms);
    options.suppress_fullscreen = read_boolean(*dock, "suppress_fullscreen", "dock", options.suppress_fullscreen);
    options.position = read_number(*dock, "position", "dock", 0.5, 0, 1);
}

void read_grid(const toml::table& document, GridOptions& options) {
    const auto* grid_node = document.get("grid");
    if (!grid_node) { return; }
    const auto* grid = grid_node->as_table();
    if (!grid) {
        invalid("configuration", "'grid' must be a table");
    }
    check_keys(*grid, {"columns", "rows", "flow", "icon_size_dip", "icon_padding_dip",
                      "icon_spacing_px"}, "grid");
    options.columns = read_dimension(*grid, "columns", options.columns);
    options.rows = read_dimension(*grid, "rows", options.rows);
    const auto flow = read_string(*grid, "flow", "grid", "row-major");
    if (flow == "row-major") {
        options.flow = Flow::row_major;
    } else if (flow == "column-major") {
        options.flow = Flow::column_major;
    } else {
        invalid("grid", "'flow' must be 'row-major' or 'column-major'");
    }
    if (const auto* icon_size = grid->get("icon_size_dip")) {
        if (icon_size->is_string() && icon_size->value<string>() == "desktop") {
            options.icon_size_dip = std::nullopt;
        } else {
            options.icon_size_dip = static_cast<float>(read_number(*grid, "icon_size_dip", "grid", 0, 0, max_icon_size_dip, true));
        }
    }
    options.icon_padding_dip = static_cast<float>(read_number(*grid, "icon_padding_dip", "grid", 4, 0, max_padding_dip));
    if (grid->contains("icon_spacing_px")) {
        options.icon_spacing_px = read_spacing_pixels(*grid);
    }
}

void read_appearance(const toml::table& document, AppearanceOptions& options) {
    const auto* appearance_node = document.get("appearance");
    if (!appearance_node) { return; }
    const auto* appearance = appearance_node->as_table();
    if (!appearance) {
        invalid("configuration", "'appearance' must be a table");
    }
    check_keys(*appearance, {"font_family", "font_weight", "text_size_pt", "text_size_dip",
                            "panel_padding_dip", "tooltip_delay_ms", "thickness",
                            "reverse_thickness", "tab_text_direction"}, "appearance");
    options.font_family = read_string(*appearance, "font_family", "appearance", options.font_family);
    if (options.font_family.empty()) {
        invalid("appearance", "'font_family' must not be empty");
    }
    if (const auto* weight = appearance->get("font_weight")) {
        const auto value = weight->value<std::int64_t>();
        if (!weight->is_integer() || !value || *value < 1 || *value > 999) {
            invalid("appearance", "'font_weight' must be an integer from 1 through 999");
        }
        options.font_weight = static_cast<std::uint32_t>(*value);
    }
    if (appearance->contains("text_size_pt") && appearance->contains("text_size_dip")) {
        invalid("appearance", "use only 'text_size_pt' or legacy 'text_size_dip', not both");
    }
    if (appearance->contains("text_size_dip")) {
        const auto legacy_dip = read_number(*appearance, "text_size_dip", "appearance", 16, 0, max_text_size_dip, true);
        options.text_size_pt = static_cast<float>(legacy_dip * 72.0 / 96.0);
        if (options.text_size_pt <= 0) {
            invalid("appearance", "legacy 'text_size_dip' must remain positive after conversion to points");
        }
    } else {
        options.text_size_pt = static_cast<float>(read_number(*appearance, "text_size_pt", "appearance", 9, 0, max_text_size_pt, true));
    }
    options.panel_padding_dip = static_cast<float>(read_number(*appearance, "panel_padding_dip", "appearance", 8, 0, max_padding_dip));
    options.thickness_dip = read_number(*appearance, "thickness", "appearance", 2, 0, max_thickness_dip);
    options.reverse_thickness = read_boolean(*appearance, "reverse_thickness", "appearance", false);
    const auto direction = read_string(*appearance, "tab_text_direction", "appearance", "auto");
    if (direction == "auto") {
        options.tab_text_direction = TabTextDirection::automatic;
    } else if (direction == "top-to-bottom") {
        options.tab_text_direction = TabTextDirection::top_to_bottom;
    } else if (direction == "bottom-to-top") {
        options.tab_text_direction = TabTextDirection::bottom_to_top;
    } else {
        invalid("appearance", "'tab_text_direction' must be 'auto', 'top-to-bottom', or 'bottom-to-top'");
    }
    options.tooltip_delay_ms = read_delay(*appearance, "tooltip_delay_ms", "appearance", options.tooltip_delay_ms, max_tooltip_delay_ms);
}

void read_adjustments(const toml::table& document, AdjustmentFactors& options) {
    const auto* adjustment_node = document.get("adjustments");
    if (!adjustment_node) { return; }
    const auto* adjustments = adjustment_node->as_table();
    if (!adjustments) {
        invalid("configuration", "'adjustments' must be a table");
    }
    check_keys(*adjustments, {"text_scale", "icon_scale", "spacing_scale"}, "adjustments");
    options.text_scale = read_number(*adjustments, "text_scale", "adjustments", 1, min_size_scale, max_adjustment_scale);
    options.icon_scale = read_number(*adjustments, "icon_scale", "adjustments", 1, min_size_scale, max_adjustment_scale);
    options.spacing_scale = read_number(*adjustments, "spacing_scale", "adjustments", 1, 0, max_adjustment_scale);
}

void read_logging(const toml::table& document, LoggingOptions& options) {
    const auto* node = document.get("logging");
    if (!node) { return; }
    const auto* logging = node->as_table();
    if (!logging) { invalid("configuration", "'logging' must be a table"); }
    check_keys(*logging, {"file"}, "logging");
    if (!logging->contains("file")) { return; }
    const auto text = read_launch_string(*logging, "file", "logging");
    if (text.empty()) { invalid("logging", "'file' must not be empty"); }
    const std::filesystem::path file(std::u8string(text.begin(), text.end()));
    if (file.has_root_path() && !file.is_absolute()) {
        invalid("logging", "'file' must be absolute or relative without a drive or root");
    }
    options.file = file;
}

void read_configuration_options(const toml::table& document, Configuration& configuration) {
    read_dock(document, configuration.dock);
    read_grid(document, configuration.grid);
    read_appearance(document, configuration.appearance);
    read_adjustments(document, configuration.adjustments);
    read_logging(document, configuration.logging);

    const auto* grid = document["grid"].as_table();
    const auto* adjustments = document["adjustments"].as_table();
    const bool pixel_spacing_explicit = grid && grid->contains("icon_spacing_px");
    const bool legacy_spacing_explicit = (grid && grid->contains("icon_padding_dip")) ||
        (adjustments && adjustments->contains("spacing_scale"));
    if (pixel_spacing_explicit && legacy_spacing_explicit) {
        invalid("configuration", "'grid.icon_spacing_px' cannot be combined with legacy "
                                 "'grid.icon_padding_dip' or 'adjustments.spacing_scale'");
    }
    if (legacy_spacing_explicit) {
        configuration.grid.icon_spacing_px.reset();
    }
}

LaunchAction read_launch_action(const toml::table& table, const string& location, const toml::table* paths, string target, bool adobe_cc) {
    LaunchAction action;
    action.target = std::move(target);
    action.handler = read_launch_string(table, "handler", location, adobe_cc ? "adobeCC" : action.handler);
    if (adobe_cc && action.handler != "adobeCC") {
        invalid(location, "Adobe CC entries cannot override handler 'adobeCC'");
    }
    if (table.contains("include_beta") && action.handler != "adobeCC") {
        invalid(location, "'include_beta' requires handler 'adobeCC'");
    }
    action.include_beta = read_boolean(table, "include_beta", location, action.handler == "adobeCC");
    if (const auto* roots = table.get("root_path")) {
        if (action.handler != "adobeCC") { invalid(location, "'root_path' requires handler 'adobeCC'"); }
        const auto append = [&](const toml::node& node) {
            if (!node.is_string()) { invalid(location, "'root_path' must be a string or an array of strings"); }
            const toml::table field{{"root_path", *node.value<string>()}};
            const auto text = read_path_string(field, "root_path", location, paths);
            if (text.empty()) { invalid(location, "'root_path' entries must not be empty"); }
            const std::filesystem::path path(std::u8string(text.begin(), text.end()));
            const auto& native = path.native();
            const auto start = native.starts_with(L"\\\\?\\") ? 4U : 0U;
            if (native.find_first_of(L"*?", start) != std::wstring::npos || (!path.is_absolute() && (path.has_root_name() || path.has_root_directory()))) {
                invalid(location, "'root_path' entries must be directories without wildcards, absolute or relative without a drive or root");
            }
            action.root_paths.push_back(path);
        };
        if (const auto* list = roots->as_array()) { for (const auto& entry : *list) { append(entry); } }
        else { append(*roots); }
    }
    action.arguments = read_path_string(table, "arguments", location, paths, TextUse::arguments);
    action.working_directory = read_path_string(table, "working_directory", location, paths, TextUse::directory);
    const auto show = read_launch_string(table, "show", location, "normal");
    if (action.target.empty() || action.handler.empty()) {
        invalid(location, "launch entries need a nonempty target and handler");
    }
    if (action.handler == "adobeCC") {
        const std::filesystem::path target_path(std::u8string(action.target.begin(), action.target.end()));
        const auto extension = target_path.extension().native();
        if (CompareStringOrdinal(extension.c_str(), static_cast<int>(extension.size()), L".exe", 4, TRUE) == CSTR_EQUAL &&
                (target_path.has_parent_path() || action.target.find(':') != string::npos)) {
            invalid(location, "Adobe executable targets must be filenames; put search directories in 'root_path'");
        }
    }
    if (show == "normal") {
        action.show = LaunchShow::normal;
    } else if (show == "hidden") {
        action.show = LaunchShow::hidden;
    } else if (show == "minimized") {
        action.show = LaunchShow::minimized;
    } else {
        invalid(location, "'show' must be 'normal', 'hidden', or 'minimized'");
    }
    return action;
}

void read_context_verbs(const toml::table& table, Item& item, const string& location) {
    const auto* verbs_node = table.get("context_verbs");
    if (!verbs_node) { return; }
    if (!item.launch || (item.launch->handler != "shell" && item.launch->handler != "adobeCC")) {
        invalid(location, "'context_verbs' requires a target with handler 'shell' or 'adobeCC'");
    }
    const auto* verbs = verbs_node->as_array();
    if (!verbs) {
        invalid(location, "'context_verbs' must be an array of tables");
    }
    item.context_verbs.reserve(verbs->size());
    for (size_t verb_index = 0; verb_index < verbs->size(); ++verb_index) {
        const auto verb_location = location + ".context_verbs[" + std::to_string(verb_index) + "]";
        const auto* verb_table = (*verbs)[verb_index].as_table();
        if (!verb_table) {
            invalid(verb_location, "context verb must be a table");
        }
        check_keys(*verb_table, {"label", "verb"}, verb_location);
        ContextVerb verb{read_launch_string(*verb_table, "label", verb_location),
            read_launch_string(*verb_table, "verb", verb_location)};
        if (verb.label.empty() || verb.verb.empty()) {
            invalid(verb_location, "context verbs need a nonempty label and verb");
        }
        item.context_verbs.push_back(std::move(verb));
    }
}

Item read_item(const toml::table& table, const string& location, const toml::table* paths) {
    check_keys(table, {"kind", "label", "icon", "target", "shift_target", "arguments",
        "working_directory", "show", "handler", "context_verbs", "include_beta", "root_path", "flags"}, location);
    Item item;
    item.flags = read_item_flags(table, location);
    const auto kind = read_string(table, "kind", location, "icon");
    if (kind == "icon" || kind == "adobeCC") {
        const bool adobe_cc = kind == "adobeCC";
        if (adobe_cc && !table.contains("target") && !table.contains("shift_target")) {
            invalid(location, "Adobe CC entries require 'target' or 'shift_target' with a product name");
        }
        auto target = read_path_string(table, "target", location, paths);
        item.label = read_string(table, "label", location, adobe_cc ? target : "");
        item.label_explicit = table.contains("label");
        item.icon_source = read_icon_source(table, location, paths);
        item.icon_explicit = table.contains("icon");
        if (table.contains("target")) {
            item.launch = read_launch_action(table, location, paths, std::move(target), adobe_cc);
        }
        if (table.contains("shift_target")) {
            item.shift_launch = read_launch_action(table, location, paths,
                read_path_string(table, "shift_target", location, paths), adobe_cc);
        }
        if (!item.launch && !item.shift_launch && (table.contains("arguments") ||
                   table.contains("working_directory") ||
                   table.contains("show") || table.contains("handler") ||
                   table.contains("include_beta") || table.contains("root_path"))) {
            invalid(location, "launch settings require 'target' or 'shift_target'");
        }
        read_context_verbs(table, item, location);
    } else if (kind == "spacer" || kind == "break") {
        item.kind = kind == "spacer" ? ItemKind::spacer : ItemKind::line_break;
        if (table.size() != 1U + static_cast<unsigned>(table.contains("flags"))) {
            invalid(location, "structural entries accept only 'kind' and 'flags'");
        }
    } else {
        invalid(location, "'kind' must be 'icon', 'adobeCC', 'spacer', or 'break'");
    }
    return item;
}

std::vector<Item> read_items(const toml::table& table, const string& location, const toml::table* paths) {
    std::vector<Item> result;
    const auto* items_node = table.get("items");
    if (!items_node) { return result; }
    const auto* items = items_node->as_array();
    if (!items) {
        invalid(location, "'items' must be an array of tables");
    }
    result.reserve(items->size());
    for (size_t item_index = 0; item_index < items->size(); ++item_index) {
        const auto item_location = location + ".items[" + std::to_string(item_index) + "]";
        const auto* item_table = (*items)[item_index].as_table();
        if (!item_table) {
            invalid(item_location, "item must be a table");
        }
        result.push_back(read_item(*item_table, item_location, paths));
    }
    return result;
}

Tab read_tab(const toml::table& table, const string& location, const toml::table* paths) {
    check_keys(table, {"name", "short_name", "color", "items"}, location);
    Tab tab;
    tab.name = read_string(table, "name", location);
    tab.short_name = read_string(table, "short_name", location, tab.name);
    if (tab.name.empty() || tab.short_name.empty()) {
        invalid(location, "'name' and 'short_name' must not be empty");
    }
    try {
        const auto color_name = normalize_color_name(read_string(table, "color", location, "salmon"));
        tab.colors = find_color(color_name);
        tab.color_rgb = tab.colors.page.gradient.front().rgb;
    } catch (const UnknownColorError& error) {
        throw UnknownColorError(error.name, location);
    } catch (const ConfigError& error) {
        invalid(location, error.what());
    }
    tab.items = read_items(table, location, paths);
    return tab;
}

std::vector<Tab> read_tabs(const toml::table& document, const toml::table* paths) {
    std::vector<Tab> result;
    const auto* tabs = document["tabs"].as_array();
    if (!tabs || tabs->empty()) {
        invalid("configuration", "'tabs' must contain at least one tab");
    }
    result.reserve(tabs->size());
    for (size_t tab_index = 0; tab_index < tabs->size(); ++tab_index) {
        const auto location = "tabs[" + std::to_string(tab_index) + "]";
        const auto* table = (*tabs)[tab_index].as_table();
        if (!table) {
            invalid(location, "tab must be a table");
        }
        result.push_back(read_tab(*table, location, paths));
    }
    return result;
}

Configuration read_configuration(const toml::table& document, const FileLookup& files) {
    check_keys(document, {"schema_version", "colors", "paths", "dock", "grid", "appearance", "adjustments", "logging", "tabs"},
               "configuration");
    if (const auto* version = document.get("schema_version")) {
        if (!version->is_integer() || version->value<std::int64_t>() != 1) {
            invalid("configuration", "'schema_version' must be 1");
        }
    }

    const auto paths = read_paths(document, files);
    Configuration configuration;
    read_configuration_options(document, configuration);
    configuration.tabs = read_tabs(document, &paths);
    return configuration;
}

std::vector<string> read_color_sets(const toml::table& document, const std::filesystem::path& source_file) try {
    std::vector<string> sets;
    const auto* node = document.get("colors");
    if (!node) { return sets; }
    const auto append = [&](const toml::node& value) {
        if (!value.is_string()) { invalid("configuration", "'colors' must be a string or an array of strings"); }
        auto name = normalize_color_name(*value.value<string>());
        if (name.find_first_of("/\\:<>\"|?*") != string::npos) {
            invalid("configuration", "'colors' entries must be basenames without filename separators");
        }
        if (name == "." || name == ".." || name.back() == '.' || name.back() == ' ') {
            invalid("configuration", "'colors' basenames must not end in a period or space");
        }
        sets.push_back(std::move(name));
    };
    if (const auto* array = node->as_array()) {
        for (const auto& value : *array) { append(value); }
    } else { append(*node); }
    return sets;
} catch (ConfigError& error) {
    error.source_file = source_file;
    throw;
}

void resolve_icon_files(ConfigurationFiles& loaded, const FileLookup& files) {
    const auto record = [](std::vector<string>& entries, string value) {
        if (std::ranges::find(entries, value) == entries.end()) { entries.push_back(std::move(value)); }
    };
    for (auto& tab : loaded.configuration.tabs) {
        for (auto& item : tab.items) {
            if (!item.icon_explicit) { continue; }
            std::visit([&](auto& icon) {
                using Icon = std::remove_cvref_t<decltype(icon)>;
                if constexpr (std::is_same_v<Icon, FileIcon> || std::is_same_v<Icon, IconGroup> || std::is_same_v<Icon, PeResourceIcon>) {
                    if (const auto found = files.find(icon.path)) {
                        icon.path = *found;
                        const auto utf8 = icon.path.u8string();
                        record(loaded.icon_sources, string(utf8.begin(), utf8.end()));
                    } else {
                        record(loaded.icon_warnings, files.missing_message(icon.path));
                        // Keep the decoder's generic fallback, anchored to the
                        // first searched directory rather than its caller's CWD.
                        if (!icon.path.is_absolute()) {
                            const auto root = files.directories().empty() ? files.executable().parent_path() : files.directories().front();
                            if (root.empty()) { throw ConfigError("Cannot anchor a missing icon without a file search directory"); }
                            icon.path = root / icon.path;
                        }
                    }
                }
            }, item.icon_source);
        }
    }
}

ConfigurationFiles read_configuration_files(const toml::table& document, const FileLookup& files, bool resolve_icons,
    const std::filesystem::path& source_file = {}) {
    const std::lock_guard lock(color_configuration_mutex);
    color_library.clear();
    auto sets = read_color_sets(document, source_file);
    std::vector<string> sources;
    for (const auto filename : {"basic_colors.toml", "extra_colors.toml"}) {
        sources.push_back(load_colors(filename, files));
    }
    for (const auto& basename : sets) {
        sources.push_back(load_colors("colors-" + basename + ".toml", files));
    }
    try {
        ConfigurationFiles loaded{read_configuration(document, files), std::move(sources), {}, {}};
        loaded.configuration.color_sets = std::move(sets);
        if (resolve_icons) { resolve_icon_files(loaded, files); }
        return loaded;
    } catch (UnknownColorError& error) {
        error.sources = std::move(sources);
        error.source_file = source_file;
        throw;
    } catch (ConfigError& error) {
        error.source_file = source_file;
        throw;
    }
}

} // namespace

Configuration parse_configuration(string_view source) {
    return read_configuration_files(config_text_builder::from_text(source).parse(), FileLookup({}, {}), false).configuration;
}

Configuration load_configuration(const std::filesystem::path& path) {
    return load_configuration_files(path, {}, {}).configuration;
}

ConfigurationFiles load_configuration_files(const std::filesystem::path& path,
    const std::filesystem::path& user_directory, const std::filesystem::path& executable_directory) {
    const auto full_path = std::filesystem::absolute(path);
    const auto executable = executable_directory.empty() ? std::filesystem::path{} : executable_directory / L"Launcher.exe";
    return load_configuration_files(full_path, FileLookup(executable, user_directory, full_path.parent_path(), true));
}

ConfigurationFiles load_configuration_files(const std::filesystem::path& path, const FileLookup& files) {
    const auto loaded = files.read_text(path, FileFallback::none);
    const auto document = [&] {
        try { return config_text_builder::from_text(loaded.text, loaded.source).parse(); }
        catch (ConfigError& error) {
            error.source_file = loaded.file;
            throw;
        }
    }();
    return read_configuration_files(document, files, true, loaded.file);
}

Layout layout_configuration(const Configuration& configuration) {
    if (configuration.tabs.empty()) {
        invalid("configuration", "at least one tab is required");
    }
    if (configuration.dock.reveal_duration_ms > max_dock_delay_ms ||
            configuration.dock.hide_duration_ms > max_dock_delay_ms) {
        invalid("dock", "movement durations must be from 0 through 5000 milliseconds");
    }
    if ((configuration.grid.columns &&
         (*configuration.grid.columns == 0 ||
          *configuration.grid.columns > max_grid_dimension)) ||
        (configuration.grid.rows &&
         (*configuration.grid.rows == 0 ||
          *configuration.grid.rows > max_grid_dimension))) {
        invalid("grid", "fixed dimensions must be from 1 to " +
                            std::to_string(max_grid_dimension));
    }
    if (configuration.grid.icon_size_dip &&
        (!std::isfinite(*configuration.grid.icon_size_dip) ||
         *configuration.grid.icon_size_dip <= 0 ||
         *configuration.grid.icon_size_dip > max_icon_size_dip)) {
        invalid("grid", "'icon_size_dip' must be finite, positive, and at most " +
                            std::to_string(static_cast<unsigned>(max_icon_size_dip)));
    }
    if (configuration.grid.icon_spacing_px &&
        *configuration.grid.icon_spacing_px > max_icon_spacing_px) {
        invalid("grid", "'icon_spacing_px' must be from 0 through " +
                            std::to_string(max_icon_spacing_px));
    }
    if (configuration.appearance.tooltip_delay_ms > max_tooltip_delay_ms) {
        invalid("appearance", "'tooltip_delay_ms' must be from 0 through " +
                              std::to_string(max_tooltip_delay_ms) + " milliseconds");
    }

    const bool row_major = configuration.grid.flow == Flow::row_major;
    const auto fixed_run = row_major ? configuration.grid.columns : configuration.grid.rows;
    const auto fixed_cross = row_major ? configuration.grid.rows : configuration.grid.columns;
    size_t max_cells = 1;
    for (const auto& tab : configuration.tabs) {
        max_cells = std::max(max_cells, cell_count(tab));
        if (max_cells > max_grid_dimension * max_grid_dimension) {
            invalid("tab '" + tab.name + "'", "too many cells for the maximum " +
                        std::to_string(max_grid_dimension) + " by " +
                        std::to_string(max_grid_dimension) + " grid");
        }
    }

    size_t run_capacity = 1;
    if (fixed_run) {
        run_capacity = *fixed_run;
    } else if (fixed_cross) {
        const auto max_run = std::min(max_cells, max_grid_dimension);
        if (!all_tabs_fit(configuration, max_run, *fixed_cross)) {
            invalid("grid", "items or explicit breaks cannot fit within the fixed " +
                                string(row_major ? "row" : "column") +
                                " count and the maximum automatic dimension of " +
                                std::to_string(max_grid_dimension));
        }
        size_t lower = 1;
        size_t upper = max_run;
        while (lower < upper) {
            const auto middle = lower + (upper - lower) / 2;
            if (all_tabs_fit(configuration, middle, *fixed_cross)) {
                upper = middle;
            } else {
                lower = middle + 1;
            }
        }
        run_capacity = lower;
    } else {
        // Both-auto uses a stable six-column starting shape, without window sizing.
        run_capacity = row_major ? std::min<size_t>(6, max_cells)
                                 : max_cells / 6 + (max_cells % 6 != 0 ? 1 : 0);
    }
    if (run_capacity > max_grid_dimension) {
        invalid("grid", "automatic dimension exceeds " + std::to_string(max_grid_dimension));
    }

    Layout layout;
    layout.columns = configuration.grid.columns.value_or(1);
    layout.rows = configuration.grid.rows.value_or(1);
    layout.tabs.reserve(configuration.tabs.size());
    for (const auto& tab : configuration.tabs) {
        auto result = place_tab(tab, configuration.grid.flow, run_capacity,
                                fixed_cross.value_or(max_grid_dimension));
        if (result.overflow) {
            invalid("tab '" + tab.name + "'", fixed_cross
                        ? "items exceed the fixed grid dimensions"
                        : "automatic dimension exceeds " + std::to_string(max_grid_dimension));
        }
        if (!configuration.grid.columns) {
            layout.columns = std::max(layout.columns, result.layout.used_columns);
        }
        if (!configuration.grid.rows) {
            layout.rows = std::max(layout.rows, result.layout.used_rows);
        }
        layout.tabs.push_back(std::move(result.layout));
    }
    return layout;
}

} // namespace launcher
