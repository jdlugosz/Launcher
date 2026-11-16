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

#include "launcher/settings.hpp"
#include "launcher/config_text.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace launcher {
namespace {

namespace fs = std::filesystem;

void require_range(double value, double minimum, double maximum, std::string_view key) {
    if (!std::isfinite(value) || value < minimum || value > maximum) {
        std::ostringstream message;
        message << key << " must be finite and from " << minimum << " through " << maximum;
        throw ConfigError(message.str());
    }
}

void validate_runtime(const RuntimeAdjustments& values) {
    require_range(values.text_scale, min_size_scale, max_adjustment_scale, "runtime text_scale");
    require_range(values.icon_scale, min_size_scale, max_adjustment_scale, "runtime icon_scale");
    require_range(values.spacing_scale, 0, max_adjustment_scale, "runtime spacing_scale");
    if (values.icon_spacing_px && *values.icon_spacing_px > max_icon_spacing_px) {
        throw ConfigError("runtime icon_spacing_px must be from 0 through " +
                          std::to_string(max_icon_spacing_px));
    }
    if (values.edge_position) {
        require_range(*values.edge_position, 0, 1, "runtime dock.position");
    }
    if (values.thickness_dip) {
        require_range(*values.thickness_dip, 0, max_thickness_dip, "runtime appearance.thickness");
    }
}

void known_keys(const toml::table& table,
                std::initializer_list<std::string_view> allowed,
                std::string_view location) {
    for (const auto& [key, value] : table) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end()) {
            throw ConfigError(std::string(location) + ": unknown setting '" + std::string(key.str()) + "'");
        }
    }
}

double number(const toml::table& table, std::string_view key, double fallback) {
    const auto* node = table.get(key);
    if (!node) {
        return fallback;
    }
    if (!node->is_integer() && !node->is_floating_point()) {
        throw ConfigError("runtime '" + std::string(key) + "' must be a number");
    }
    const auto value = node->value<double>();
    if (!value) {
        throw ConfigError("runtime '" + std::string(key) + "' cannot be represented as a number");
    }
    return *value;
}

std::uint32_t spacing_pixels(const toml::table& table) {
    const auto* node = table.get("icon_spacing_px");
    const auto value = node ? node->value<std::int64_t>() : std::nullopt;
    if (!node || !node->is_integer() || !value || *value < 0 ||
        *value > static_cast<std::int64_t>(max_icon_spacing_px)) {
        throw ConfigError("runtime 'icon_spacing_px' must be an integer from 0 through " +
                          std::to_string(max_icon_spacing_px));
    }
    return static_cast<std::uint32_t>(*value);
}

std::string path_text(const fs::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

[[noreturn]] void storage_error(std::string_view action, const fs::path& path,
                                const std::error_code& error) {
    throw ConfigError(std::string(action) + " '" + path_text(path) + "': " + error.message());
}

class RuntimeStaging {
public:
    explicit RuntimeStaging(const fs::path& parent) {
        static std::atomic<std::uint64_t> sequence = 0;
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            directory_ = parent / (".launcher-runtime-" + std::to_string(GetCurrentProcessId()) +
                "-" + std::to_string(GetTickCount64()) + "-" +
                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
            std::error_code error;
            if (fs::create_directory(directory_, error)) {
                file_ = directory_ / "runtime.toml";
                return;
            }
            if (error && error != std::errc::file_exists) {
                storage_error("Cannot stage runtime settings", directory_, error);
            }
        }
        throw ConfigError("Cannot allocate a unique runtime settings staging directory");
    }
    RuntimeStaging(const RuntimeStaging&) = delete;
    RuntimeStaging& operator=(const RuntimeStaging&) = delete;
    ~RuntimeStaging() {
        std::error_code ignored;
        fs::remove(file_, ignored);
        fs::remove(directory_, ignored);
    }
    const fs::path& path() const { return file_; }

private:
    fs::path directory_;
    fs::path file_;
};

float effective_value(double value, double maximum, std::string_view key, bool positive) {
    require_range(value, 0, maximum, key);
    const auto converted = static_cast<float>(value);
    if (positive && converted <= 0) {
        throw ConfigError(std::string(key) + " must be positive");
    }
    return converted;
}

} // namespace

fs::path runtime_settings_path(const fs::path& configuration_path) {
    return FileLookup({}, {}).runtime_path(configuration_path);
}

namespace {

void validate_identity_text(std::string_view text, std::string_view field, bool allow_empty = true) {
    if ((!allow_empty && text.empty()) || text.find('\0') != std::string_view::npos) {
        throw ConfigError("Runtime active version '" + std::string(field) + "' must be a " +
            (allow_empty ? "string" : "nonempty string") + " without NUL characters");
    }
}

std::string identity_string(const toml::table& table, std::string_view key, bool allow_empty = true) {
    const auto value = table[key].value<std::string>();
    if (!value) { throw ConfigError("Runtime active version '" + std::string(key) + "' must be a string"); }
    validate_identity_text(*value, key, allow_empty);
    return *value;
}

bool identity_boolean(const toml::table& table, std::string_view key) {
    const auto* node = table.get(key);
    const auto value = node ? node->value<bool>() : std::nullopt;
    if (!node || !node->is_boolean() || !value) { throw ConfigError("Runtime active version '" + std::string(key) + "' must be a boolean"); }
    return *value;
}

std::int64_t identity_integer(const toml::table& table, std::string_view key, std::int64_t maximum) {
    const auto* node = table.get(key);
    const auto value = node ? node->value<std::int64_t>() : std::nullopt;
    if (!node || !node->is_integer() || !value || *value < 0 || *value > maximum) {
        throw ConfigError("Runtime active version '" + std::string(key) + "' must be an integer from 0 through " + std::to_string(maximum));
    }
    return *value;
}

const toml::table& identity_table(const toml::table& table, std::string_view key) {
    const auto* node = table.get(key);
    const auto* result = node ? node->as_table() : nullptr;
    if (!result) { throw ConfigError("Runtime active version '" + std::string(key) + "' must be a table"); }
    return *result;
}

fs::path identity_path(const toml::table& table, std::string_view key) {
    // These are already-resolved identity fields, not authored configuration
    // paths. Environment variables and dollar symbols must remain literal.
    const auto text = identity_string(table, key, false);
    return fs::path(std::u8string(text.begin(), text.end()));
}

LaunchAction read_identity_launch(const toml::table& table) {
    known_keys(table, {"handler", "target", "arguments", "working_directory", "show", "include_beta", "root_path"}, "runtime active version launch");
    LaunchAction launch;
    launch.handler = identity_string(table, "handler", false);
    launch.target = identity_string(table, "target", false);
    launch.arguments = identity_string(table, "arguments");
    launch.working_directory = identity_string(table, "working_directory");
    const auto show = identity_string(table, "show");
    if (show == "normal") { launch.show = LaunchShow::normal; }
    else if (show == "hidden") { launch.show = LaunchShow::hidden; }
    else if (show == "minimized") { launch.show = LaunchShow::minimized; }
    else { throw ConfigError("Runtime active version 'show' must be normal, hidden, or minimized"); }
    launch.include_beta = identity_boolean(table, "include_beta");
    if (const auto* node = table.get("root_path")) {
        const auto* roots = node->as_array();
        if (!roots) { throw ConfigError("Runtime active version 'root_path' must be an array of strings"); }
        for (const auto& entry : *roots) {
            const auto text = entry.value<std::string>();
            if (!entry.is_string() || !text || text->empty() || text->find('\0') != std::string::npos) {
                throw ConfigError("Runtime active version 'root_path' entries must be nonempty strings without NUL");
            }
            launch.root_paths.emplace_back(std::u8string(text->begin(), text->end()));
        }
    }
    return launch;
}

ResourceIdentifier read_identity_resource(const toml::table& table, std::string_view key) {
    const auto* node = table.get(key);
    if (node && node->is_string()) { return identity_string(table, key, false); }
    return static_cast<std::uint16_t>(identity_integer(table, key, 65535));
}

IconSource read_identity_icon(const toml::table& table) {
    if (table.contains("stock")) {
        known_keys(table, {"stock"}, "runtime active version stock icon");
        const auto stock = stock_icon_named(identity_string(table, "stock", false));
        if (!stock) { throw ConfigError("Unknown runtime active version stock icon"); }
        return *stock;
    }
    if (table.contains("shell")) {
        known_keys(table, {"shell"}, "runtime active version shell icon");
        return ShellItemIcon{identity_path(table, "shell")};
    }
    if (table.contains("resource")) {
        known_keys(table, {"resource", "type", "name", "language"}, "runtime active version resource icon");
        const auto language = table.contains("language")
            ? std::optional{static_cast<std::uint16_t>(identity_integer(table, "language", 65535))} : std::nullopt;
        return PeResourceIcon{identity_path(table, "resource"), read_identity_resource(table, "type"),
            read_identity_resource(table, "name"), language};
    }
    known_keys(table, {"file", "index", "id"}, "runtime active version file icon");
    auto path = identity_path(table, "file");
    if (table.contains("index") && table.contains("id")) {
        throw ConfigError("Runtime active version icon must specify either index or id, not both");
    }
    if (table.contains("index")) {
        return IconGroup{std::move(path), IconIndex{static_cast<int>(identity_integer(table, "index", std::numeric_limits<int>::max()))}};
    }
    if (table.contains("id")) {
        return IconGroup{std::move(path), IconResourceId{static_cast<std::uint16_t>(identity_integer(table, "id", 65535))}};
    }
    return FileIcon{std::move(path)};
}

VersionItemIdentity read_item_identity(const toml::table& table) {
    known_keys(table, {"tab", "launch", "label", "label_explicit", "icon_explicit", "icon", "occurrence"}, "runtime active version item");
    VersionItemIdentity item;
    item.tab = identity_string(table, "tab", false);
    item.launch = read_identity_launch(identity_table(table, "launch"));
    item.label = identity_string(table, "label");
    item.label_explicit = identity_boolean(table, "label_explicit");
    item.icon_explicit = identity_boolean(table, "icon_explicit");
    item.icon = read_identity_icon(identity_table(table, "icon"));
    item.occurrence = static_cast<std::size_t>(identity_integer(table, "occurrence", std::numeric_limits<std::int64_t>::max()));
    return item;
}

void validate_active_executable(const fs::path& executable) {
    if (!executable.is_absolute() || executable.filename().empty() || executable.native().find(L'\0') != std::wstring::npos) {
        throw ConfigError("Each runtime active version must name an absolute executable path without NUL characters");
    }
}

ActiveVersions read_active_versions(const toml::table& document) {
    const auto* node = document.get("active_version");
    if (!node) { return {}; }
    const auto* entries = node->as_array();
    if (!entries) { throw ConfigError("runtime 'active_version' must be an array of tables"); }
    ActiveVersions versions;
    for (const auto& entry : *entries) {
        const auto* table = entry.as_table();
        if (!table) { throw ConfigError("Each runtime active version must be a table with an item table and executable path"); }
        known_keys(*table, {"item", "executable"}, "runtime active version");
        auto identity = read_item_identity(identity_table(*table, "item"));
        auto executable = identity_path(*table, "executable");
        validate_active_executable(executable);
        if (std::ranges::any_of(versions, [&](const ActiveVersion& value) { return value.item == identity; })) {
            throw ConfigError("Duplicate runtime active version item in tab '" + identity.tab + "'");
        }
        versions.push_back({std::move(identity), std::move(executable)});
    }
    return versions;
}

RuntimeAdjustments read_runtime_values(const toml::table& document);
std::vector<AdjustmentPreset> read_presets(const toml::table& document);

toml::table read_runtime_document(const fs::path& path, const FileLookup& files) {
    const auto loaded = files.read_optional_text(path, FileFallback::exact_file);
    if (!loaded) { return {}; }
    auto document = config_text_builder::from_text(loaded->text, loaded->source).parse();
    known_keys(document, {"adjustments", "dock", "grid", "appearance", "preset", "active_version"}, "runtime settings");
    (void)read_runtime_values(document);
    (void)read_presets(document);
    (void)read_active_versions(document);
    return document;
}

RuntimeAdjustments read_runtime_values(const toml::table& document) {
    RuntimeAdjustments values;
    bool legacy_spacing_explicit = false;
    if (const auto* node = document.get("adjustments")) {
        const auto* adjustments = node->as_table();
        if (!adjustments) {
            throw ConfigError("runtime 'adjustments' must be a table");
        }
        known_keys(*adjustments, {"text_scale", "icon_scale", "spacing_scale"}, "runtime adjustments");
        values.text_scale = number(*adjustments, "text_scale", 1);
        values.icon_scale = number(*adjustments, "icon_scale", 1);
        values.spacing_scale = number(*adjustments, "spacing_scale", 1);
        legacy_spacing_explicit = adjustments->contains("spacing_scale");
    }
    if (const auto* node = document.get("grid")) {
        const auto* grid = node->as_table();
        if (!grid) {
            throw ConfigError("runtime 'grid' must be a table");
        }
        known_keys(*grid, {"icon_spacing_px"}, "runtime grid");
        if (grid->contains("icon_spacing_px")) {
            if (legacy_spacing_explicit) {
                throw ConfigError("runtime 'grid.icon_spacing_px' cannot be combined with "
                                  "legacy 'adjustments.spacing_scale'");
            }
            values.icon_spacing_px = spacing_pixels(*grid);
        }
    }
    if (const auto* node = document.get("dock")) {
        const auto* dock = node->as_table();
        if (!dock) {
            throw ConfigError("runtime 'dock' must be a table");
        }
        known_keys(*dock, {"position"}, "runtime dock");
        if (dock->contains("position")) {
            values.edge_position = number(*dock, "position", 0.5);
        }
    }
    if (const auto* node = document.get("appearance")) {
        const auto* appearance = node->as_table();
        if (!appearance) { throw ConfigError("runtime 'appearance' must be a table"); }
        known_keys(*appearance, {"thickness"}, "runtime appearance");
        if (appearance->contains("thickness")) {
            values.thickness_dip = number(*appearance, "thickness", 2);
        }
    }
    validate_runtime(values);
    return values;
}

void require_snapshot(const RuntimeAdjustments& values) {
    validate_runtime(values);
    if (!values.icon_spacing_px || !values.edge_position || !values.thickness_dip || values.spacing_scale != 1) {
        throw ConfigError("A preset must contain all five current adjustments, using pixel spacing");
    }
}

std::vector<AdjustmentPreset> read_presets(const toml::table& document) {
    const auto* node = document.get("preset");
    if (!node) { return {}; }
    const auto* entries = node->as_array();
    if (!entries) { throw ConfigError("runtime 'preset' must be an array of tables"); }
    std::vector<AdjustmentPreset> presets;
    std::set<std::string> names;
    for (const auto& entry : *entries) {
        const auto* table = entry.as_table();
        if (!table) { throw ConfigError("Each runtime preset must be a table with a name"); }
        known_keys(*table, {"name", "adjustments", "grid", "dock", "appearance"}, "runtime preset");
        const auto name = (*table)["name"].value<std::string>();
        if (!name || name->find_first_not_of(" \t\r\n") == std::string::npos) {
            throw ConfigError("Each runtime preset must have a nonempty string name");
        }
        if (!names.insert(*name).second) { throw ConfigError("Duplicate runtime preset name: '" + *name + "'"); }
        AdjustmentPreset preset{*name, std::nullopt};
        if (table->size() > 1) {
            if (!(*table)["adjustments"]["text_scale"] || !(*table)["adjustments"]["icon_scale"] ||
                (*table)["adjustments"]["spacing_scale"]) {
                throw ConfigError("Preset '" + *name + "' must contain both scale factors and use pixel spacing");
            }
            preset.values = read_runtime_values(*table);
            require_snapshot(*preset.values);
        }
        presets.push_back(std::move(preset));
    }
    return presets;
}

void set_runtime_values(toml::table& document, const RuntimeAdjustments& values) {
    document.insert_or_assign("adjustments", toml::table{{"text_scale", values.text_scale}, {"icon_scale", values.icon_scale}});
    document.insert_or_assign("grid", toml::table{{"icon_spacing_px", *values.icon_spacing_px}});
    if (values.edge_position) { document.insert_or_assign("dock", toml::table{{"position", *values.edge_position}}); }
    else { document.erase("dock"); }
    if (values.thickness_dip) { document.insert_or_assign("appearance", toml::table{{"thickness", *values.thickness_dip}}); }
    else { document.erase("appearance"); }
}

std::string_view launch_show_name(LaunchShow show) {
    switch (show) {
    case LaunchShow::normal: return "normal";
    case LaunchShow::hidden: return "hidden";
    case LaunchShow::minimized: return "minimized";
    }
    throw ConfigError("Invalid runtime active version launch show value");
}

toml::table identity_launch_table(const LaunchAction& launch) {
    toml::table table{{"handler", launch.handler}, {"target", launch.target}, {"arguments", launch.arguments},
        {"working_directory", launch.working_directory}, {"show", launch_show_name(launch.show)},
        {"include_beta", launch.include_beta}};
    if (!launch.root_paths.empty()) {
        toml::array roots;
        for (const auto& root : launch.root_paths) { roots.push_back(path_text(root)); }
        table.insert("root_path", std::move(roots));
    }
    return table;
}

void insert_resource_identifier(toml::table& table, std::string_view key, const ResourceIdentifier& identifier) {
    std::visit([&](const auto& value) { table.insert(key, value); }, identifier);
}

toml::table identity_icon_table(const IconSource& source) {
    toml::table table;
    if (const auto* stock = std::get_if<StockIcon>(&source)) {
        const auto index = static_cast<std::size_t>(*stock);
        if (index >= stock_icon_names.size()) { throw ConfigError("Invalid runtime active version stock icon"); }
        table.insert("stock", stock_icon_names[index]);
    } else if (const auto* file = std::get_if<FileIcon>(&source)) {
        table.insert("file", path_text(file->path));
    } else if (const auto* group = std::get_if<IconGroup>(&source)) {
        table.insert("file", path_text(group->path));
        if (const auto* index = std::get_if<IconIndex>(&group->selector)) { table.insert("index", index->value); }
        else { table.insert("id", std::get<IconResourceId>(group->selector).value); }
    } else if (const auto* resource = std::get_if<PeResourceIcon>(&source)) {
        table.insert("resource", path_text(resource->path));
        insert_resource_identifier(table, "type", resource->type);
        insert_resource_identifier(table, "name", resource->name);
        if (resource->language) { table.insert("language", *resource->language); }
    } else {
        table.insert("shell", path_text(std::get<ShellItemIcon>(source).path));
    }
    return table;
}

toml::table item_identity_table(const VersionItemIdentity& item) {
    if (item.occurrence > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw ConfigError("Runtime active version occurrence is too large");
    }
    return toml::table{{"tab", item.tab}, {"launch", identity_launch_table(item.launch)}, {"label", item.label},
        {"label_explicit", item.label_explicit}, {"icon_explicit", item.icon_explicit},
        {"icon", identity_icon_table(item.icon)}, {"occurrence", static_cast<std::int64_t>(item.occurrence)}};
}

void validate_item_identity(const VersionItemIdentity& item) {
    (void)read_item_identity(item_identity_table(item));
}

void set_active_versions(toml::table& document, const ActiveVersions& versions) {
    if (versions.empty()) {
        document.erase("active_version");
        return;
    }
    toml::array entries;
    for (const auto& version : versions) {
        entries.push_back(toml::table{{"item", item_identity_table(version.item)}, {"executable", path_text(version.executable)}});
    }
    document.insert_or_assign("active_version", std::move(entries));
}

VersionItemIdentity item_identity(std::string_view tab, const Item& item) {
    if (item.kind != ItemKind::icon || !item.launch) {
        throw ConfigError("An active version identity requires a launchable icon");
    }
    return {std::string(tab), *item.launch, item.label, item.label_explicit, item.icon_explicit, item.icon_source};
}

void write_runtime_document(const fs::path& path, const toml::table& document, const FileLookup& files) {
    if (path.empty() || path.filename().empty()) {
        throw ConfigError("Runtime settings path must name a file");
    }
    const auto destination = files.exact_path(path);
    std::error_code error;
    fs::create_directories(destination.parent_path(), error);
    if (error) {
        storage_error("Cannot create runtime settings directory", destination.parent_path(), error);
    }
    const RuntimeStaging staging(destination.parent_path());
    std::ostringstream content;
    content.imbue(std::locale::classic());
    content << "# Launcher runtime adjustments; the base configuration is never rewritten.\n"
            << "# To merge, copy the text/icon factors into base [adjustments] when its factors are 1.\n"
            << "# Otherwise multiply the existing base factors by these factors, then remove\n"
            << "# the corresponding runtime keys. Keep merged factors within their allowed ranges.\n"
            << "# Alternatively multiply base text_size_pt and numeric icon_size_dip by their\n"
            << "# runtime text_scale and icon_scale respectively, then remove those runtime keys;\n"
            << "# Desktop icon mode should be merged using [adjustments].\n"
            << "# Copy grid.icon_spacing_px into base [grid], then remove the runtime grid key.\n"
            << "# It is an absolute physical-pixel gap; remove legacy base icon_padding_dip and\n"
            << "# spacing_scale when migrating. The outer grid margin uses this same gap.\n"
            << "# Copy runtime dock.position into base [dock] and remove the runtime position.\n"
            << "# Copy runtime appearance.thickness into base [appearance] and remove the runtime key.\n"
            << "# Add [[preset]] entries with name = 'Normal' to define empty preset slots.\n"
            << "# Store fills a named slot; Recall previews it until Apply/OK. Comments are regenerated.\n\n"
            << "# [[active_version]] entries retain exact installation choices; omit an item to use the latest.\n\n"
            << toml::toml_formatter{document} << '\n';
    files.write_text(staging.path(), content.view());
    if (!MoveFileExW(staging.path().c_str(), destination.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        storage_error("Cannot replace runtime settings", destination,
                      {static_cast<int>(GetLastError()), std::system_category()});
    }
}

} // namespace

RuntimeAdjustments load_runtime_adjustments(const fs::path& path, const FileLookup& files) {
    return read_runtime_values(read_runtime_document(path, files));
}

void save_runtime_adjustments(const fs::path& path, const RuntimeAdjustments& values, const FileLookup& files) {
    validate_runtime(values);
    if (!values.icon_spacing_px) {
        throw ConfigError("Resolve legacy spacing at the target display DPI and set runtime icon_spacing_px before saving");
    }
    auto document = read_runtime_document(path, files);
    set_runtime_values(document, values);
    write_runtime_document(path, document, files);
}

std::vector<AdjustmentPreset> load_adjustment_presets(const fs::path& path, const FileLookup& files) {
    return read_presets(read_runtime_document(path, files));
}

void store_adjustment_preset(const fs::path& path, std::string_view name, const RuntimeAdjustments& values, const FileLookup& files) {
    require_snapshot(values);
    auto document = read_runtime_document(path, files);
    if (auto* entries = document["preset"].as_array()) {
        for (auto& entry : *entries) {
            auto& table = *entry.as_table();
            if (table["name"].value<std::string>() == name) {
                set_runtime_values(table, values);
                write_runtime_document(path, document, files);
                return;
            }
        }
    }
    throw ConfigError("Runtime preset no longer exists: '" + std::string(name) + "'");
}

ActiveVersions load_active_versions(const fs::path& path, const FileLookup& files) {
    return read_active_versions(read_runtime_document(path, files));
}

std::optional<fs::path> active_version(const ActiveVersions& versions, const VersionItemIdentity& item_identity) {
    const auto found = std::ranges::find(versions, item_identity, &ActiveVersion::item);
    return found == versions.end() ? std::nullopt : std::optional(found->executable);
}

void set_active_version(const fs::path& path, const VersionItemIdentity& item_identity, const fs::path& executable, const FileLookup& files) {
    validate_item_identity(item_identity);
    validate_active_executable(executable);
    auto document = read_runtime_document(path, files);
    auto versions = read_active_versions(document);
    const auto found = std::ranges::find(versions, item_identity, &ActiveVersion::item);
    if (found == versions.end()) { versions.push_back({item_identity, executable}); }
    else { found->executable = executable; }
    set_active_versions(document, versions);
    write_runtime_document(path, document, files);
}

void reset_active_version(const fs::path& path, const VersionItemIdentity& item_identity, const FileLookup& files) {
    validate_item_identity(item_identity);
    auto document = read_runtime_document(path, files);
    auto versions = read_active_versions(document);
    const auto found = std::ranges::find(versions, item_identity, &ActiveVersion::item);
    if (found == versions.end()) { return; }
    versions.erase(found);
    set_active_versions(document, versions);
    write_runtime_document(path, document, files);
}

VersionItemIdentity version_item_identity(const Configuration& configuration, std::size_t tab_index, std::size_t item_index) {
    if (tab_index >= configuration.tabs.size() || item_index >= configuration.tabs[tab_index].items.size()) {
        throw ConfigError("An active version identity requires an existing tab item");
    }
    const auto& selected_tab = configuration.tabs[tab_index];
    auto identity = item_identity(selected_tab.name, selected_tab.items[item_index]);
    std::size_t occurrence = 0;
    for (std::size_t tab = 0; tab <= tab_index; ++tab) {
        if (configuration.tabs[tab].name != selected_tab.name) { continue; }
        const auto& items = configuration.tabs[tab].items;
        const auto limit = tab == tab_index ? item_index : items.size();
        for (std::size_t item = 0; item < limit; ++item) {
            if (items[item].kind == ItemKind::icon && items[item].launch && item_identity(selected_tab.name, items[item]) == identity) {
                ++occurrence;
            }
        }
    }
    identity.occurrence = occurrence;
    return identity;
}

EffectiveSettings resolve_effective_settings(const Configuration& base,
                                              const RuntimeAdjustments& runtime,
                                              float desktop_nominal_dip,
                                              std::uint32_t target_dpi) {
    validate_runtime(runtime);
    if (target_dpi == 0) {
        throw ConfigError("Target display DPI must be positive");
    }
    if (base.grid.icon_spacing_px && *base.grid.icon_spacing_px > max_icon_spacing_px) {
        throw ConfigError("base icon_spacing_px must be from 0 through " +
                          std::to_string(max_icon_spacing_px));
    }
    require_range(base.adjustments.text_scale, min_size_scale, max_adjustment_scale, "base text_scale");
    require_range(base.adjustments.icon_scale, min_size_scale, max_adjustment_scale, "base icon_scale");
    require_range(base.adjustments.spacing_scale, 0, max_adjustment_scale, "base spacing_scale");
    require_range(base.dock.position, 0, 1, "base dock.position");
    require_range(base.appearance.thickness_dip, 0, max_thickness_dip, "base appearance.thickness");
    if (base.appearance.font_family.empty() || base.appearance.font_weight < 1 ||
        base.appearance.font_weight > 999) {
        throw ConfigError("Base font family must be nonempty and its weight from 1 through 999");
    }
    const double nominal_icon = base.grid.icon_size_dip.value_or(desktop_nominal_dip);
    const auto nominal_text_pt = effective_value(base.appearance.text_size_pt, max_text_size_pt,
                                                  "nominal text size in points", true);
    const double nominal_text_dip = static_cast<double>(nominal_text_pt) * 96.0 / 72.0;
    const auto checked_icon = effective_value(nominal_icon, max_icon_size_dip,
                                               "nominal icon size", true);
    (void)effective_value(base.appearance.panel_padding_dip,
                         max_padding_dip, "legacy nominal panel padding", false);
    const auto nominal_icon_padding = effective_value(base.grid.icon_padding_dip,
                                                       max_padding_dip, "nominal icon padding", false);
    const double text_scale = base.adjustments.text_scale * runtime.text_scale;
    const double icon_scale = base.adjustments.icon_scale * runtime.icon_scale;
    std::uint32_t gap = 0;
    if (runtime.icon_spacing_px) {
        gap = *runtime.icon_spacing_px;
    } else {
        const double base_gap = base.grid.icon_spacing_px
            ? static_cast<double>(*base.grid.icon_spacing_px)
            : 2.0 * nominal_icon_padding * base.adjustments.spacing_scale *
                static_cast<double>(target_dpi) / 96.0;
        const double legacy_gap = std::round(base_gap * runtime.spacing_scale);
        require_range(legacy_gap, 0, max_icon_spacing_px, "legacy spacing in pixels");
        gap = static_cast<std::uint32_t>(legacy_gap);
    }
    return {base.appearance.font_family, base.appearance.font_weight,
        effective_value(nominal_text_dip * text_scale, max_text_size_dip, "effective text size", true),
        effective_value(checked_icon * icon_scale, max_icon_size_dip, "effective icon size", true),
        runtime.edge_position.value_or(base.dock.position), gap,
        runtime.thickness_dip.value_or(base.appearance.thickness_dip)};
}

} // namespace launcher
