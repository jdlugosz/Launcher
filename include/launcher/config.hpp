#pragma once

#include "launcher/colors.hpp"
#include "launcher/icon_source.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

class FileLookup;

inline constexpr std::size_t max_grid_dimension = 128;
inline constexpr float max_icon_size_dip = 256.0F;
inline constexpr float max_text_size_dip = 128.0F;
inline constexpr float max_text_size_pt = 96.0F;
inline constexpr float max_padding_dip = 256.0F;
inline constexpr double max_thickness_dip = 16.0;
inline constexpr std::uint32_t max_icon_spacing_px = 1024;
inline constexpr double min_size_scale = 0.25;
inline constexpr double max_adjustment_scale = 4.0;
inline constexpr std::uint32_t max_dock_delay_ms = 5000;
inline constexpr std::uint32_t max_tooltip_delay_ms = 5000;

enum class Flow { row_major, column_major };
enum class ItemKind { icon, spacer, line_break };
inline constexpr std::uint32_t item_flag_log_icon_import = 1U; // Reserved; detailed logging is deferred.
enum class Edge { top, bottom, left, right };
enum class TabTextDirection { automatic, top_to_bottom, bottom_to_top };
enum class LaunchShow { normal, hidden, minimized };

struct DockOptions {
    Edge edge = Edge::right;
    std::string display = "primary";
    bool auto_hide = true;
    std::uint32_t hover_delay_ms = 180;
    std::uint32_t hide_delay_ms = 350;
    std::uint32_t reveal_duration_ms = 220;
    std::uint32_t hide_duration_ms = 220; // Omitted TOML value follows reveal_duration_ms.
    bool suppress_fullscreen = true;
    double position = 0.5;  // position along the edge, whole screen is 0.0 though 1.0
};

struct GridOptions {
    std::optional<std::size_t> columns = 6;
    std::optional<std::size_t> rows;
    Flow flow = Flow::row_major;
    std::optional<float> icon_size_dip; // nullopt follows the desktop's nominal size.
    float icon_padding_dip = 4.0F; // Legacy half-gap in DIPs.
    std::optional<std::uint32_t> icon_spacing_px = 8U;
};

struct AppearanceOptions {
    std::string font_family = "Segoe UI Variable";
    std::uint32_t font_weight = 600;
    float text_size_pt = 9.0F;
    float panel_padding_dip = 8.0F; // Parsed for compatibility; no longer a grid margin.
    std::uint32_t tooltip_delay_ms = 700;
    double thickness_dip = 2.0;
    bool reverse_thickness = false;
    TabTextDirection tab_text_direction = TabTextDirection::automatic;
};

struct AdjustmentFactors {
    double text_scale = 1.0;
    double icon_scale = 1.0;
    double spacing_scale = 1.0; // Legacy spacing reader only.
};

struct LoggingOptions {
    // Omitted: the normal AppData log. Relative filenames are based on the configuration directory.
    std::optional<std::filesystem::path> file;
};

struct LaunchAction {
    // Configuration substitutions are complete before an action is stored.
    std::string handler = "shell";
    std::string target;
    std::string arguments;
    std::string working_directory;
    LaunchShow show = LaunchShow::normal;
    bool include_beta = true; // Adobe CC selects the newest installed edition, including Beta.
    std::vector<std::filesystem::path> root_paths{}; // Additional Adobe search directories; relative to the configuration.
    bool operator==(const LaunchAction&) const = default;
};

struct ContextVerb {
    std::string label;
    std::string verb;
};

struct Item {
    ItemKind kind = ItemKind::icon;
    std::string label;
    IconSource icon_source = StockIcon::application;
    std::optional<LaunchAction> launch;
    bool icon_explicit = false;
    std::vector<ContextVerb> context_verbs;
    bool label_explicit = false; // An explicit empty label suppresses automatic naming.
    std::uint32_t flags = 0; // Shared by all item kinds; unassigned bits are retained.
    std::optional<LaunchAction> shift_launch; // Shares launch options; only the target differs.
};

struct Tab {
    std::string name;
    std::string short_name;
    rgb_t color_rgb = 0xf2a5a5;
    std::vector<Item> items;
    ColorScheme colors = adhoc_color(color_rgb);
};

struct Configuration {
    DockOptions dock;
    GridOptions grid;
    AppearanceOptions appearance;
    AdjustmentFactors adjustments;
    LoggingOptions logging;
    std::vector<Tab> tabs;
    std::vector<std::string> color_sets; // Applied in order after basic_colors and extra_colors.
};

struct ItemPlacement {
    std::size_t item_index = 0;
    std::size_t column = 0;
    std::size_t row = 0;
};

struct TabLayout {
    std::vector<ItemPlacement> placements;
    std::size_t used_columns = 0;
    std::size_t used_rows = 0;
};

struct Layout {
    std::size_t columns = 0;
    std::size_t rows = 0;
    std::vector<TabLayout> tabs;
};

// Text and labels remain UTF-8; native UI code converts at the API boundary.
Configuration parse_configuration(std::string_view source);
Configuration load_configuration(const std::filesystem::path& path);

struct ConfigurationFiles {
    Configuration configuration;
    std::vector<std::string> color_sources; // Full file paths or executable resource labels, in load order.
    std::vector<std::string> icon_sources; // Successfully resolved explicit icon files.
    std::vector<std::string> icon_warnings; // Missing icon files retain generic fallback, with full searched paths.
};

ConfigurationFiles load_configuration_files(const std::filesystem::path& path, const FileLookup& files);

ConfigurationFiles load_configuration_files(
    const std::filesystem::path& path,  // e.g. "Launcher.toml", could be an explicit path different from user_directory
    const std::filesystem::path& user_directory, // %LOCALAPPDATA%\Długosz\Launcher
    const std::filesystem::path& executable_directory // supplies $EXE; empty discovers the running executable's directory
);

// Placements include spacers. Structural breaks never produce a placement.
Layout layout_configuration(const Configuration& configuration);

} // namespace launcher
