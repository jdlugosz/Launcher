#pragma once

#include "launcher/config.hpp"
#include "launcher/file_lookup.hpp"

#include <functional>

namespace launcher {

struct RuntimeAdjustments {
    double text_scale = 1.0;
    double icon_scale = 1.0;
    double spacing_scale = 1.0; // Legacy spacing reader only.
    std::optional<double> edge_position;
    std::optional<std::uint32_t> icon_spacing_px;
    std::optional<double> thickness_dip; // Absolute DIPs, independent of text/icon scale.
};

struct AdjustmentPreset {
    std::string name;
    std::optional<RuntimeAdjustments> values; // A name-only entry is an empty slot.
};

struct VersionItemIdentity {
    std::string tab;
    LaunchAction launch;
    std::string label;
    bool label_explicit = false;
    bool icon_explicit = false;
    IconSource icon = StockIcon::application;
    std::size_t occurrence = 0;
    bool operator==(const VersionItemIdentity&) const = default;
};

struct ActiveVersion {
    VersionItemIdentity item;
    std::filesystem::path executable;
};

// Missing entries mean Always use latest. Paths name the exact installation;
// unavailable installations retain their saved preference.
using ActiveVersions = std::vector<ActiveVersion>;

struct EffectiveSettings {
    std::string font_family;
    std::uint32_t font_weight = 600;
    float text_size_dip = 12.0F;
    float icon_size_dip = 0;
    double edge_position = 0.5;
    std::uint32_t icon_spacing_px = 8U;
    double thickness_dip = 2.0;
};

std::filesystem::path runtime_settings_path(const std::filesystem::path& configuration_path);
// Runtime reads and writes use exact paths through this same lookup context;
// they never borrow a matching filename from another directory root.
// Missing runtime settings mean identity. Invalid existing content throws
// ConfigError and is preserved for the user to repair.
RuntimeAdjustments load_runtime_adjustments(const std::filesystem::path& path, const FileLookup& files = FileLookup({}, {}));
// Atomically replaces only this runtime file. The base configuration is untouched.
// Resolve legacy spacing and set icon_spacing_px before saving.
void save_runtime_adjustments(const std::filesystem::path& path,
                              const RuntimeAdjustments& adjustments, const FileLookup& files = FileLookup({}, {}));
// Read afresh so hand-edited slots appear without restarting Launcher.
std::vector<AdjustmentPreset> load_adjustment_presets(const std::filesystem::path& path, const FileLookup& files = FileLookup({}, {}));
// Replace an existing named slot, preserving the active settings and other slots.
void store_adjustment_preset(const std::filesystem::path& path, std::string_view name, const RuntimeAdjustments& values, const FileLookup& files = FileLookup({}, {}));
ActiveVersions load_active_versions(const std::filesystem::path& path, const FileLookup& files = FileLookup({}, {}));
std::optional<std::filesystem::path> active_version(const ActiveVersions& versions, const VersionItemIdentity& item_identity);
// These actions preserve the active adjustments, preset slots, and other items.
void set_active_version(const std::filesystem::path& path, const VersionItemIdentity& item_identity, const std::filesystem::path& executable, const FileLookup& files = FileLookup({}, {}));
void reset_active_version(const std::filesystem::path& path, const VersionItemIdentity& item_identity, const FileLookup& files = FileLookup({}, {}));
// Uses the tab name, complete launch binding, label, and icon description,
// independent of grid position. Changing those configured details changes the
// identity. Exact duplicate items within same-named tabs use occurrence order;
// swapping indistinguishable duplicates cannot preserve individual identities.
VersionItemIdentity version_item_identity(const Configuration& configuration, std::size_t tab_index, std::size_t item_index);
EffectiveSettings resolve_effective_settings(const Configuration& base,
                                              const RuntimeAdjustments& runtime,
                                              float desktop_nominal_dip,
                                              std::uint32_t target_dpi = 96);

} // namespace launcher
