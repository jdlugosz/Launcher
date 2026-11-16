#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/settings.hpp"
#include "launcher/file_lookup.hpp"
#include "launcher/launch.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

class Fixture {
public:
    Fixture() : parent_(fs::weakly_canonical(fs::temp_directory_path())) {
        static std::atomic<unsigned> sequence = 0;
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            root_ = parent_ / ("LauncherSettingsTests-" + std::to_string(GetCurrentProcessId()) +
                "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence++));
            std::error_code error;
            if (fs::create_directory(root_, error)) {
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::runtime_error("Cannot create settings test directory");
            }
        }
        throw std::runtime_error("Cannot allocate settings test directory");
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        std::error_code error;
        const auto resolved = fs::weakly_canonical(root_, error);
        if (!error && resolved == root_ && resolved.parent_path() == parent_) {
            fs::remove_all(resolved, error);
        }
    }
    fs::path path(std::string_view name) const { return root_ / name; }
    const fs::path& directory() const { return root_; }

private:
    fs::path parent_;
    fs::path root_;
};

void write(const fs::path& path, std::string_view content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    file.close();
    REQUIRE(file.good());
}

std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void no_staging(const fs::path& directory) {
    for (const auto& entry : fs::directory_iterator(directory)) {
        REQUIRE_FALSE(entry.path().filename().wstring().starts_with(L".launcher-runtime-"));
    }
}

class FileLock {
public:
    explicit FileLock(const fs::path& path)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ~FileLock() { if (handle_ != INVALID_HANDLE_VALUE) { CloseHandle(handle_); } }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_;
};

launcher::VersionItemIdentity version_identity(std::string_view label = "Tool") {
    launcher::VersionItemIdentity item;
    item.tab = "Tools";
    item.launch = launcher::LaunchAction{"shell", R"(C:\Tools\Tool-$version\tool.exe)"};
    item.label = label;
    item.label_explicit = true;
    return item;
}

std::string active_version_source() {
    return R"([[active_version]]
executable = 'C:\Tool\tool.exe'
[active_version.item]
tab = 'Tools'
label = 'Tool'
label_explicit = true
icon_explicit = false
occurrence = 0
[active_version.item.launch]
handler = 'shell'
target = 'C:\Tools\Tool-$version\tool.exe'
arguments = ''
working_directory = ''
show = 'normal'
include_beta = true
[active_version.item.icon]
stock = 'application'
)";
}

std::string changed_source(std::string source, std::string_view old_text, std::string_view new_text) {
    const auto offset = source.find(old_text);
    REQUIRE(offset != source.npos);
    source.replace(offset, old_text.size(), new_text);
    return source;
}

} // namespace

TEST_CASE("Nominal sizing defaults follow desktop icons and compact appearance", "[settings][config]") {
    const auto base = launcher::parse_configuration("[[tabs]]\nname='General'\n");
    REQUIRE_FALSE(base.grid.icon_size_dip.has_value());
    REQUIRE(base.grid.icon_padding_dip == 4);
    REQUIRE(base.grid.icon_spacing_px == 8U);
    REQUIRE(base.appearance.font_family == "Segoe UI Variable");
    REQUIRE(base.appearance.font_weight == 600);
    REQUIRE(base.appearance.text_size_pt == 9);
    REQUIRE(launcher::resolve_effective_settings(base, {}, 32).text_size_dip == 12);
    REQUIRE(base.appearance.panel_padding_dip == 8);
    REQUIRE(base.adjustments.text_scale == 1);
    REQUIRE(base.adjustments.icon_scale == 1);
    REQUIRE(base.adjustments.spacing_scale == 1);
    REQUIRE(base.dock.position == 0.5);

    const auto appearance = launcher::parse_configuration(
        "[appearance]\nfont_weight=600\n[[tabs]]\nname='General'\n");
    REQUIRE(appearance.appearance.text_size_pt == 9);
    REQUIRE(launcher::resolve_effective_settings(appearance, {}, 32).text_size_dip == 12);

    const auto desktop = launcher::parse_configuration(
        "[grid]\nicon_size_dip='desktop'\n[[tabs]]\nname='General'\n");
    REQUIRE_FALSE(desktop.grid.icon_size_dip.has_value());
    const auto numeric = launcher::parse_configuration(
        "[grid]\nicon_size_dip=48.5\n[[tabs]]\nname='General'\n");
    REQUIRE(numeric.grid.icon_size_dip == 48.5F);
}

TEST_CASE("Effective settings multiply base and runtime factors with absolute position override", "[settings]") {
    launcher::Configuration base;
    base.grid.icon_size_dip = 32.0F;
    base.grid.icon_padding_dip = 4;
    base.grid.icon_spacing_px.reset();
    base.appearance = {"Custom Font", 650, 15, 8};
    base.adjustments = {1.5, 0.5, 2};
    base.dock.position = 0.2;
    const launcher::RuntimeAdjustments runtime{2, 1.5, 0.25, 0.75};

    const auto result = launcher::resolve_effective_settings(base, runtime, 96);
    REQUIRE(result.font_family == "Custom Font");
    REQUIRE(result.font_weight == 650);
    REQUIRE(result.text_size_dip == 60);
    REQUIRE(result.icon_size_dip == 24);
    REQUIRE(result.icon_spacing_px == 4);
    REQUIRE(result.edge_position == 0.75);
}

TEST_CASE("Point-sized text is converted once to renderer DIPs", "[settings][config]") {
    const float points[] = {9.0F, 12.0F, launcher::max_text_size_pt};
    const float expected_dips[] = {12.0F, 16.0F, launcher::max_text_size_dip};
    for (std::size_t index = 0; index < std::size(points); ++index) {
        CAPTURE(points[index]);
        const auto base = launcher::parse_configuration(
            "[appearance]\ntext_size_pt=" + std::to_string(points[index]) +
            "\n[[tabs]]\nname='General'\n");
        REQUIRE(base.appearance.text_size_pt == points[index]);
        REQUIRE(launcher::resolve_effective_settings(base, {}, 32).text_size_dip ==
                expected_dips[index]);
    }

    auto compact = launcher::parse_configuration(
        "[appearance]\ntext_size_pt=9\n[[tabs]]\nname='General'\n");
    compact.adjustments.text_scale = 1.5;
    REQUIRE(launcher::resolve_effective_settings(compact, {}, 32).text_size_dip == 18);
    compact.adjustments.text_scale = 1;
    launcher::RuntimeAdjustments runtime;
    runtime.text_scale = 1.5;
    REQUIRE(launcher::resolve_effective_settings(compact, runtime, 32).text_size_dip == 18);
}

TEST_CASE("Legacy DIP-sized text remains visually equivalent and cannot be ambiguous", "[settings][config]") {
    const float dips[] = {16.0F, 18.0F, launcher::max_text_size_dip};
    const float expected_points[] = {12.0F, 13.5F, launcher::max_text_size_pt};
    for (std::size_t index = 0; index < std::size(dips); ++index) {
        CAPTURE(dips[index]);
        const auto base = launcher::parse_configuration(
            "[appearance]\ntext_size_dip=" + std::to_string(dips[index]) +
            "\n[[tabs]]\nname='General'\n");
        REQUIRE(base.appearance.text_size_pt == expected_points[index]);
        REQUIRE(launcher::resolve_effective_settings(base, {}, 32).text_size_dip == dips[index]);
    }
    REQUIRE_THROWS_AS(launcher::parse_configuration(
        "[appearance]\ntext_size_pt=12\ntext_size_dip=16\n[[tabs]]\nname='General'\n"),
        launcher::ConfigError);
}

TEST_CASE("Desktop nominal icons are used only in desktop mode and zero factors are meaningful", "[settings]") {
    launcher::Configuration base;
    base.dock.position = 0.2;
    auto result = launcher::resolve_effective_settings(base, {}, 48);
    REQUIRE(result.icon_size_dip == 48);
    REQUIRE(result.edge_position == 0.2);

    launcher::RuntimeAdjustments runtime;
    runtime.spacing_scale = 0;
    runtime.edge_position = 0;
    result = launcher::resolve_effective_settings(base, runtime, 48);
    REQUIRE(result.icon_spacing_px == 0);
    REQUIRE(result.edge_position == 0);

    base.grid.icon_size_dip = 36.0F;
    result = launcher::resolve_effective_settings(base, {}, std::numeric_limits<float>::quiet_NaN());
    REQUIRE(result.icon_size_dip == 36);
}

TEST_CASE("Effective settings reject invalid programmatic values and excessive merged sizes", "[settings]") {
    launcher::Configuration base;
    launcher::RuntimeAdjustments runtime;
    float desktop_nominal = 32;
    SECTION("Negative nominal text") { base.appearance.text_size_pt = -1; }
    SECTION("Zero nominal text") { base.appearance.text_size_pt = 0; }
    SECTION("Nonfinite nominal text") { base.appearance.text_size_pt = std::numeric_limits<float>::quiet_NaN(); }
    SECTION("Excessive nominal text") { base.appearance.text_size_pt = launcher::max_text_size_pt + 1.0F; }
    SECTION("Zero nominal icon") { base.grid.icon_size_dip = 0.0F; }
    SECTION("Nonfinite numeric nominal icon") { base.grid.icon_size_dip = std::numeric_limits<float>::quiet_NaN(); }
    SECTION("Invalid desktop nominal icon") { desktop_nominal = -1; }
    SECTION("Nonfinite desktop nominal icon") { desktop_nominal = std::numeric_limits<float>::quiet_NaN(); }
    SECTION("Negative padding") { base.appearance.panel_padding_dip = -1; }
    SECTION("Nonfinite padding") { base.grid.icon_padding_dip = std::numeric_limits<float>::quiet_NaN(); }
    SECTION("Invalid font weight") { base.appearance.font_weight = 1000; }
    SECTION("Empty font family") { base.appearance.font_family.clear(); }
    SECTION("Too-small text factor") { runtime.text_scale = 0.2; }
    SECTION("Negative icon factor") { runtime.icon_scale = -1; }
    SECTION("Nonfinite spacing factor") { runtime.spacing_scale = std::numeric_limits<double>::quiet_NaN(); }
    SECTION("Excessive merged icon") { base.adjustments.icon_scale = 4; runtime.icon_scale = 4; }
    SECTION("Excessive merged text") { base.adjustments.text_scale = 4; runtime.text_scale = 4; }
    SECTION("Excessive legacy pixel gap") {
        base.grid.icon_spacing_px.reset();
        base.grid.icon_padding_dip = 256; base.adjustments.spacing_scale = 4; runtime.spacing_scale = 4;
    }
    SECTION("Excessive base pixel gap") { base.grid.icon_spacing_px = launcher::max_icon_spacing_px + 1U; }
    SECTION("Excessive runtime pixel gap") { runtime.icon_spacing_px = launcher::max_icon_spacing_px + 1U; }
    SECTION("Negative base position") { base.dock.position = -0.01; }
    SECTION("Nonfinite runtime position") { runtime.edge_position = std::numeric_limits<double>::quiet_NaN(); }
    REQUIRE_THROWS_AS(launcher::resolve_effective_settings(base, runtime, desktop_nominal), launcher::ConfigError);
}

TEST_CASE("New nominal and adjustment TOML keys are strictly validated", "[settings][config]") {
    const std::string_view invalid_sections[] = {
        "[grid]\nicon_size_dip='auto'\n",
        "[grid]\nicon_padding_dip=-1\n",
        "[appearance]\ntext_size_pt=nan\n",
        "[appearance]\ntext_size_pt=inf\n",
        "[appearance]\ntext_size_pt=0\n",
        "[appearance]\ntext_size_pt=-1\n",
        "[appearance]\ntext_size_pt=96.01\n",
        "[appearance]\ntext_size_pt='12'\n",
        "[appearance]\ntext_size_pt=1e-300\n",
        "[appearance]\ntext_size_dip=nan\n",
        "[appearance]\ntext_size_dip=129\n",
        "[appearance]\ntext_size_dip=1e-300\n",
        "[appearance]\nfont_family=''\n",
        "[appearance]\nfont_weight=600.0\n",
        "[appearance]\npanel_padding_dip=257\n",
        "[adjustments]\ntext_scale=0\n",
        "[adjustments]\nicon_scale=4.1\n",
        "[adjustments]\nspacing_scale=-1\n",
        "[dock]\nposition=nan\n",
        "[dock]\nposition=1.01\n",
    };
    for (const auto section : invalid_sections) {
        CAPTURE(section);
        REQUIRE_THROWS_AS(launcher::parse_configuration(std::string(section) +
                            "[[tabs]]\nname='General'\n"), launcher::ConfigError);
    }
    const auto valid = launcher::parse_configuration(R"toml(
[appearance]
font_family = "Segoe UI"
font_weight = 700
text_size_pt = 13.5
panel_padding_dip = 10
[grid]
icon_size_dip = "desktop"
icon_padding_dip = 3
[adjustments]
text_scale = 0.75
icon_scale = 1.25
spacing_scale = 0
[dock]
position = 1
[[tabs]]
name = "General"
)toml");
    REQUIRE(valid.appearance.font_weight == 700);
    REQUIRE(valid.appearance.text_size_pt == 13.5F);
    REQUIRE(valid.adjustments.spacing_scale == 0);
    REQUIRE_FALSE(valid.grid.icon_spacing_px.has_value());
    REQUIRE(valid.dock.position == 1);
}

TEST_CASE("Absolute icon spacing is independent of DPI and icon scale", "[settings][spacing]") {
    launcher::Configuration base;
    base.grid.icon_size_dip = 16.0F;
    base.grid.icon_spacing_px = 7U;
    base.appearance.panel_padding_dip = launcher::max_padding_dip;
    base.adjustments.icon_scale = 4;
    launcher::RuntimeAdjustments runtime;
    runtime.icon_scale = 4;
    for (const std::uint32_t dpi : {96U, 144U, 192U}) {
        CAPTURE(dpi);
        auto result = launcher::resolve_effective_settings(base, runtime, 32, dpi);
        REQUIRE(result.icon_size_dip == 256);
        REQUIRE(result.icon_spacing_px == 7);
        for (const std::uint32_t gap : {0U, 8U, launcher::max_icon_spacing_px}) {
            CAPTURE(gap);
            runtime.icon_spacing_px = gap;
            result = launcher::resolve_effective_settings(base, runtime, 32, dpi);
            REQUIRE(result.icon_spacing_px == gap);
        }
        runtime.icon_spacing_px.reset();
    }
    REQUIRE_THROWS_AS(launcher::resolve_effective_settings(base, runtime, 32, 0),
                      launcher::ConfigError);
}

TEST_CASE("Legacy spacing converts at the target DPI and rounds after all legacy factors", "[settings][spacing]") {
    auto base = launcher::parse_configuration(
        "[grid]\nicon_padding_dip=3\n[adjustments]\nspacing_scale=1.5\n"
        "[[tabs]]\nname='General'\n");
    REQUIRE_FALSE(base.grid.icon_spacing_px.has_value());
    launcher::RuntimeAdjustments runtime;
    runtime.spacing_scale = 0.5;
    REQUIRE(launcher::resolve_effective_settings(base, runtime, 32, 96).icon_spacing_px == 5);
    REQUIRE(launcher::resolve_effective_settings(base, runtime, 32, 144).icon_spacing_px == 7);
    base.adjustments.icon_scale = 4;
    REQUIRE(launcher::resolve_effective_settings(base, runtime, 32, 144).icon_spacing_px == 7);
    runtime.spacing_scale = 0;
    REQUIRE(launcher::resolve_effective_settings(base, runtime, 32, 192).icon_spacing_px == 0);

    // A previously saved percentage still applies when the base file migrates first.
    base.grid.icon_spacing_px = 8U;
    runtime.spacing_scale = 1.5;
    REQUIRE(launcher::resolve_effective_settings(base, runtime, 32, 192).icon_spacing_px == 12);
    runtime.icon_spacing_px = 9U;
    REQUIRE(launcher::resolve_effective_settings(base, runtime, 32, 192).icon_spacing_px == 9);
}

TEST_CASE("Base pixel spacing accepts zero and rejects ambiguous legacy keys", "[settings][config][spacing]") {
    const std::uint32_t gaps[] = {0U, 8U, launcher::max_icon_spacing_px};
    for (const auto gap : gaps) {
        CAPTURE(gap);
        const auto base = launcher::parse_configuration(
            "[grid]\nicon_spacing_px=" + std::to_string(gap) +
            "\n[[tabs]]\nname='General'\n");
        REQUIRE(base.grid.icon_spacing_px == gap);
    }
    const std::string_view invalid[] = {
        "[grid]\nicon_spacing_px=-1\n",
        "[grid]\nicon_spacing_px=1025\n",
        "[grid]\nicon_spacing_px=8.0\n",
        "[grid]\nicon_spacing_px='8'\n",
        "[grid]\nicon_spacing_px=true\n",
        "[grid]\nicon_spacing_px=0\nicon_padding_dip=0\n",
        "[grid]\nicon_spacing_px=8\n[adjustments]\nspacing_scale=1\n",
    };
    for (const auto section : invalid) {
        CAPTURE(section);
        REQUIRE_THROWS_AS(launcher::parse_configuration(std::string(section) +
                          "[[tabs]]\nname='General'\n"), launcher::ConfigError);
    }
    const auto old_factor_only = launcher::parse_configuration(
        "[adjustments]\nspacing_scale=1\n[[tabs]]\nname='General'\n");
    REQUIRE_FALSE(old_factor_only.grid.icon_spacing_px.has_value());
    REQUIRE(launcher::resolve_effective_settings(old_factor_only, {}, 32, 192).icon_spacing_px == 16);
}

TEST_CASE("Runtime settings paths stay beside each base configuration", "[settings][storage]") {
    REQUIRE(launcher::runtime_settings_path(L"D:\\Example\\Launcher.toml") ==
            fs::path(L"D:\\Example\\Launcher.runtime.toml"));
    REQUIRE(launcher::runtime_settings_path(L"D:\\Example\\custom.v2.toml") ==
            fs::path(L"D:\\Example\\custom.v2.runtime.toml"));
    REQUIRE(launcher::runtime_settings_path(L"D:\\Example\\设置.toml") ==
            fs::path(L"D:\\Example\\设置.runtime.toml"));
}

TEST_CASE("Missing runtime settings mean identity without creating a file", "[settings][storage]") {
    Fixture fixture;
    const auto path = fixture.path("absent.runtime.toml");
    const auto values = launcher::load_runtime_adjustments(path);
    REQUIRE(values.text_scale == 1);
    REQUIRE(values.icon_scale == 1);
    REQUIRE(values.spacing_scale == 1);
    REQUIRE_FALSE(values.edge_position.has_value());
    REQUIRE_FALSE(values.icon_spacing_px.has_value());
    REQUIRE(launcher::load_adjustment_presets(path).empty());
    REQUIRE(launcher::load_active_versions(path).empty());
    REQUIRE_FALSE(fs::exists(path));
}

TEST_CASE("Runtime I/O uses only the selected configuration sibling through the shared lookup", "[settings][storage][file-lookup]") {
    Fixture fixture;
    const auto config_dir = fixture.path("Settings");
    const auto portable_dir = fixture.path("Portable");
    const auto user_dir = fixture.path("AppData");
    for (const auto& directory : {config_dir, portable_dir, user_dir}) { fs::create_directory(directory); }
    const auto base = config_dir / "desk.v2.toml";
    const std::string base_source = "# Keep this configuration intact\n[[tabs]]\nname='Tools'\n";
    write(base, base_source);
    const launcher::FileLookup files(portable_dir / "Launcher.exe", user_dir, config_dir, true);
    const auto runtime = files.runtime_path(base);
    REQUIRE(runtime == config_dir / "desk.v2.runtime.toml");
    REQUIRE(files.runtime_path(config_dir / "desk.runtime.toml") == config_dir / "desk.runtime.runtime.toml");
    const std::string other_source = "[adjustments]\ntext_scale=2\nicon_scale=2\n[grid]\nicon_spacing_px=12\n"
        "[[preset]]\nname='Another profile'\n" + active_version_source();
    for (const auto& directory : {portable_dir, user_dir}) { write(directory / runtime.filename(), other_source); }
    const auto relative_runtime = fs::path(fixture.directory().filename().wstring() + L".runtime.toml");
    REQUIRE_FALSE(fs::exists(relative_runtime));
    for (const auto& directory : {config_dir, portable_dir, user_dir}) { write(directory / relative_runtime, other_source); }
    REQUIRE(launcher::load_runtime_adjustments(relative_runtime, files).text_scale == 1);
    REQUIRE(launcher::load_adjustment_presets(relative_runtime, files).empty());
    REQUIRE(launcher::load_active_versions(relative_runtime, files).empty());

    REQUIRE(launcher::load_runtime_adjustments(runtime, files).text_scale == 1);
    REQUIRE(launcher::load_adjustment_presets(runtime, files).empty());
    REQUIRE(launcher::load_active_versions(runtime, files).empty());
    REQUIRE_FALSE(fs::exists(runtime));

    const launcher::RuntimeAdjustments values{1.25, 0.75, 1, 0.25, 9U, 3.0};
    launcher::save_runtime_adjustments(runtime, values, files);
    REQUIRE(launcher::load_runtime_adjustments(runtime, files).text_scale == values.text_scale);
    write(runtime, read(runtime) + "\n[[preset]]\nname='My slot'\n");
    launcher::store_adjustment_preset(runtime, "My slot", values, files);
    const auto presets = launcher::load_adjustment_presets(runtime, files);
    REQUIRE(presets.size() == 1);
    REQUIRE(presets.front().values.has_value());
    REQUIRE(presets.front().values->icon_scale == values.icon_scale);
    const auto identity = version_identity();
    const auto active = fixture.path("Tool 2026/tool.exe");
    launcher::set_active_version(runtime, identity, active, files);
    REQUIRE(launcher::active_version(launcher::load_active_versions(runtime, files), identity) == active);
    launcher::reset_active_version(runtime, identity, files);
    REQUIRE(launcher::load_active_versions(runtime, files).empty());
    const auto retained = launcher::load_adjustment_presets(runtime, files);
    REQUIRE(retained.size() == 1);
    REQUIRE(retained.front().name == "My slot");

    REQUIRE_THROWS_AS(launcher::save_runtime_adjustments(base, values, files), launcher::ConfigError);
    REQUIRE(read(base) == base_source);
    for (const auto& directory : {portable_dir, user_dir}) { REQUIRE(read(directory / runtime.filename()) == other_source); }
    no_staging(config_dir);
}

TEST_CASE("Runtime roundtrip writes merge guidance without changing the base TOML", "[settings][storage]") {
    Fixture fixture;
    const auto base = fixture.path("custom.toml");
    const std::string original = R"(# Preserve my comments and macro source
[paths]
P = 'C:\Program Files'
[[tabs]]
name = 'Mine'
items = [{ label = 'Tool', target = '$P\Tools\tool.exe' }]
)";
    write(base, original);
    const auto path = launcher::runtime_settings_path(base);
    const launcher::RuntimeAdjustments saved{1.1234567890123457, 0.75, 1, 0.1234567890123456, 11U};
    launcher::save_runtime_adjustments(path, saved);
    const auto restored = launcher::load_runtime_adjustments(path);
    REQUIRE(restored.text_scale == saved.text_scale);
    REQUIRE(restored.icon_scale == saved.icon_scale);
    REQUIRE(restored.spacing_scale == saved.spacing_scale);
    REQUIRE(restored.edge_position == saved.edge_position);
    REQUIRE(restored.icon_spacing_px == saved.icon_spacing_px);
    REQUIRE(read(base) == original);
    REQUIRE(launcher::load_configuration(base).tabs[0].items[0].launch->target ==
        R"(C:\Program Files\Tools\tool.exe)");
    const auto content = read(path);
    REQUIRE(content.find("base [adjustments]") != std::string::npos);
    REQUIRE(content.find("existing base factors") != std::string::npos);
    REQUIRE(content.find("text_size_pt") != std::string::npos);
    REQUIRE(content.find("base [grid]") != std::string::npos);
    REQUIRE(content.find("icon_spacing_px = 11") != std::string::npos);
    REQUIRE(content.find("\nspacing_scale =") == std::string::npos);
    REQUIRE(content.find("remove") != std::string::npos);
    REQUIRE(content.find("base [dock]") != std::string::npos);
    no_staging(fixture.directory());

    launcher::save_runtime_adjustments(path, {1, 1, 1, std::nullopt, 0U});
    REQUIRE_FALSE(launcher::load_runtime_adjustments(path).edge_position.has_value());
    REQUIRE(launcher::load_runtime_adjustments(path).icon_spacing_px == 0U);
    REQUIRE(read(base) == original);
    no_staging(fixture.directory());
}

TEST_CASE("Invalid existing runtime content is preserved and reported", "[settings][storage][runtime-errors]") {
    Fixture fixture;
    const auto path = fixture.path("bad.runtime.toml");
    const std::string_view invalid[] = {
        "[adjustments\n", "[adjustments]\ntext_scale=nan\n",
        "[adjustments]\nicon_scale=-1\n", "[adjustments]\nspacing_scale=5\n",
        "[adjustments]\ntext_scale='1'\n", "[dock]\nposition=-1\n",
        "[dock]\nposition=inf\n", "[dock]\nunknown=1\n",
        "[grid]\nicon_spacing_px=-1\n", "[grid]\nicon_spacing_px=1025\n",
        "[grid]\nicon_spacing_px=1.5\n", "[grid]\nicon_spacing_px='8'\n",
        "[grid]\nicon_spacing_px=true\n", "grid=8\n",
        "[adjustments]\nspacing_scale=1\n[grid]\nicon_spacing_px=8\n",
    };
    for (const auto content : invalid) {
        CAPTURE(content);
        write(path, content);
        const auto timestamp = fs::last_write_time(path);
        REQUIRE_THROWS_AS(launcher::load_runtime_adjustments(path), launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::load_adjustment_presets(path), launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::load_active_versions(path), launcher::ConfigError);
        REQUIRE(read(path) == content);
        REQUIRE(fs::last_write_time(path) == timestamp);
    }
    no_staging(fixture.directory());
}

TEST_CASE("Legacy runtime spacing requires explicit pixel migration before publication", "[settings][storage][spacing]") {
    Fixture fixture;
    const auto path = fixture.path("legacy.runtime.toml");
    const std::string original = "# Saved percentage\n[adjustments]\ntext_scale=1.25\nspacing_scale=0.5\n";
    write(path, original);
    auto runtime = launcher::load_runtime_adjustments(path);
    REQUIRE(runtime.text_scale == 1.25);
    REQUIRE(runtime.spacing_scale == 0.5);
    REQUIRE_FALSE(runtime.icon_spacing_px.has_value());
    const auto timestamp = fs::last_write_time(path);
    REQUIRE_THROWS_AS(launcher::save_runtime_adjustments(path, runtime), launcher::ConfigError);
    REQUIRE(read(path) == original);
    REQUIRE(fs::last_write_time(path) == timestamp);
    no_staging(fixture.directory());

    const auto base = launcher::parse_configuration(
        "[grid]\nicon_padding_dip=3\n[[tabs]]\nname='General'\n");
    const auto resolved = launcher::resolve_effective_settings(base, runtime, 32, 144);
    REQUIRE(resolved.icon_spacing_px == 5);
    runtime.icon_spacing_px = resolved.icon_spacing_px;
    runtime.spacing_scale = 1;
    launcher::save_runtime_adjustments(path, runtime);
    const auto migrated = launcher::load_runtime_adjustments(path);
    REQUIRE(migrated.text_scale == 1.25);
    REQUIRE(migrated.spacing_scale == 1);
    REQUIRE(migrated.icon_spacing_px == 5U);
    REQUIRE(launcher::resolve_effective_settings(base, migrated, 32, 192).icon_spacing_px == 5);
    REQUIRE(read(path).find("\nspacing_scale =") == std::string::npos);
    no_staging(fixture.directory());
}

TEST_CASE("Runtime file errors never become missing-file defaults or overwrite attempts", "[settings][storage][runtime-errors]") {
    Fixture fixture;
    auto path = fixture.path("blocked.runtime.toml");
    const launcher::RuntimeAdjustments snapshot{1.25, 1.5, 1, 0.7, 12U, 3.5};
    const auto expect_errors = [&] {
        const auto expect_file_error = [&](auto&& operation) {
            try {
                operation();
                FAIL("An existing runtime file error was treated as success or absence");
            } catch (const launcher::ConfigError& error) {
                REQUIRE(std::string_view(error.what()).find(path.filename().string()) != std::string_view::npos);
            }
        };
        expect_file_error([&] { (void)launcher::load_runtime_adjustments(path); });
        expect_file_error([&] { (void)launcher::load_adjustment_presets(path); });
        expect_file_error([&] { (void)launcher::load_active_versions(path); });
        expect_file_error([&] { launcher::save_runtime_adjustments(path, snapshot); });
        expect_file_error([&] { launcher::store_adjustment_preset(path, "Normal", snapshot); });
        expect_file_error([&] { launcher::set_active_version(path, version_identity(), fixture.path("tool.exe")); });
        expect_file_error([&] { launcher::reset_active_version(path, version_identity()); });
        no_staging(fixture.directory());
    };
    SECTION("A directory occupies the exact runtime filename") {
        REQUIRE(fs::create_directory(path));
        expect_errors();
        REQUIRE(fs::is_directory(path));
        REQUIRE(fs::is_empty(path));
    }
    SECTION("An ordinary file occupies the runtime parent directory") {
        const auto parent = fixture.path("parent-file");
        write(parent, "Keep this ordinary file intact");
        path = parent / "blocked.runtime.toml";
        expect_errors();
        REQUIRE(read(parent) == "Keep this ordinary file intact");
    }
    SECTION("A sharing violation prevents reading existing runtime settings") {
        const std::string original = "# Keep my settings\n[adjustments]\ntext_scale=1.2\n[[preset]]\nname='Normal'\n";
        write(path, original);
        const auto timestamp = fs::last_write_time(path);
        {
            FileLock lock(path);
            REQUIRE(lock.valid());
            expect_errors();
        }
        REQUIRE(read(path) == original);
        REQUIRE(fs::last_write_time(path) == timestamp);
    }
}

TEST_CASE("Atomic publication failures preserve existing runtime settings", "[settings][storage][runtime-errors]") {
    Fixture fixture;
    const auto path = fixture.path("existing.runtime.toml");
    const std::string original = "# My saved settings\n[adjustments]\ntext_scale=1.1\n";
    write(path, original);
    {
        FileLock lock(path);
        REQUIRE(lock.valid());
        REQUIRE_THROWS_AS(launcher::save_runtime_adjustments(path, {2, 2, 1, 0.75, 16U}),
                          launcher::ConfigError);
    }
    REQUIRE(read(path) == original);
    no_staging(fixture.directory());

    launcher::RuntimeAdjustments invalid;
    invalid.text_scale = -1;
    REQUIRE_THROWS_AS(launcher::save_runtime_adjustments(path, invalid), launcher::ConfigError);
    REQUIRE(read(path) == original);
    no_staging(fixture.directory());
}

TEST_CASE("Fractional sheet thickness is absolute and round trips independently of scales", "[settings][thickness]") {
    Fixture fixture;
    auto base = launcher::parse_configuration("[appearance]\nthickness=2.4\n[[tabs]]\nname='General'\n");
    REQUIRE(base.appearance.thickness_dip == 2.4);
    launcher::RuntimeAdjustments runtime{2, 3, 1, std::nullopt, 8U, 1.6};
    REQUIRE(launcher::resolve_effective_settings(base, {}, 32, 120).thickness_dip == 2.4);
    REQUIRE(launcher::resolve_effective_settings(base, runtime, 32, 120).thickness_dip == 1.6);
    base.adjustments = {0.5, 0.5, 1};
    REQUIRE(launcher::resolve_effective_settings(base, runtime, 64, 96).thickness_dip == 1.6);
    const auto path = fixture.path("thickness.runtime.toml");
    launcher::save_runtime_adjustments(path, runtime);
    REQUIRE(launcher::load_runtime_adjustments(path).thickness_dip == runtime.thickness_dip);
    REQUIRE(read(path).find("[appearance]\nthickness = ") != std::string::npos);
    runtime.thickness_dip = 0;
    launcher::save_runtime_adjustments(path, runtime);
    REQUIRE(launcher::load_runtime_adjustments(path).thickness_dip == 0);
    REQUIRE(launcher::resolve_effective_settings(base, runtime, 32).thickness_dip == 0);
    for (const auto invalid : {"-0.01", "16.01", "nan", "inf", "'2.4'"}) {
        const auto source = std::string("[appearance]\nthickness=") + invalid + "\n";
        REQUIRE_THROWS_AS(launcher::parse_configuration(source + "[[tabs]]\nname='General'\n"), launcher::ConfigError);
        write(path, source);
        REQUIRE_THROWS_AS(launcher::load_runtime_adjustments(path), launcher::ConfigError);
    }
}

TEST_CASE("Preset slots store full snapshots without applying them and survive active saves", "[settings][storage][presets]") {
    Fixture fixture;
    const auto path = fixture.path("presets.runtime.toml");
    REQUIRE(launcher::load_adjustment_presets(path).empty());
    const std::string original = R"(# A handwritten comment
[adjustments]
text_scale = 1.1
icon_scale = 1.2
[grid]
icon_spacing_px = 8
[dock]
position = 0.3
[appearance]
thickness = 2.0
[[preset]]
name = 'Normal'
[[preset]]
name = '图标测试'
)";
    write(path, original);
    const auto timestamp = fs::last_write_time(path);
    auto slots = launcher::load_adjustment_presets(path);
    REQUIRE(slots.size() == 2);
    REQUIRE(slots[0].name == "Normal");
    REQUIRE(slots[1].name == "图标测试");
    REQUIRE_FALSE(slots[0].values);
    REQUIRE_FALSE(slots[1].values);
    REQUIRE(read(path) == original);
    REQUIRE(fs::last_write_time(path) == timestamp);

    const launcher::RuntimeAdjustments snapshot{1.1234567890123457, 0.75, 1, 0.1234567890123456, 11U, 2.5};
    launcher::store_adjustment_preset(path, "图标测试", snapshot);
    auto active = launcher::load_runtime_adjustments(path);
    REQUIRE(active.text_scale == 1.1);
    REQUIRE(active.icon_scale == 1.2);
    REQUIRE(active.icon_spacing_px == 8U);
    REQUIRE(active.edge_position == 0.3);
    REQUIRE(active.thickness_dip == 2);
    slots = launcher::load_adjustment_presets(path);
    REQUIRE_FALSE(slots[0].values);
    REQUIRE(slots[1].values);
    const auto restored = *slots[1].values;
    REQUIRE(restored.text_scale == snapshot.text_scale);
    REQUIRE(restored.icon_scale == snapshot.icon_scale);
    REQUIRE(restored.edge_position == snapshot.edge_position);
    REQUIRE(restored.icon_spacing_px == snapshot.icon_spacing_px);
    REQUIRE(restored.thickness_dip == snapshot.thickness_dip);
    REQUIRE(restored.spacing_scale == 1);
    REQUIRE(read(path).find("# A handwritten comment") == std::string::npos);
    REQUIRE(read(path).find("# Launcher runtime adjustments") == 0);

    launcher::save_runtime_adjustments(path, {2, 1, 1, 0.8, 6U, 0});
    active = launcher::load_runtime_adjustments(path);
    REQUIRE(active.text_scale == 2);
    REQUIRE(active.edge_position == 0.8);
    REQUIRE(active.thickness_dip == 0);
    slots = launcher::load_adjustment_presets(path);
    REQUIRE_FALSE(slots[0].values);
    REQUIRE(slots[1].values->text_scale == snapshot.text_scale);
    REQUIRE(slots[1].values->edge_position == snapshot.edge_position);

    // Reload the whole document for each action, including slots added while running.
    write(path, read(path) + "\n[[preset]]\nname='Added later'\n");
    launcher::store_adjustment_preset(path, "Normal", {1, 1.5, 1, 0.9, 0U, 0});
    slots = launcher::load_adjustment_presets(path);
    REQUIRE(slots.size() == 3);
    REQUIRE(slots[0].values->icon_scale == 1.5);
    REQUIRE(slots[1].values->text_scale == snapshot.text_scale);
    REQUIRE(slots[2].name == "Added later");
    REQUIRE_FALSE(slots[2].values);
    launcher::store_adjustment_preset(path, "Normal", snapshot);
    REQUIRE(launcher::load_adjustment_presets(path)[0].values->text_scale == snapshot.text_scale);
    REQUIRE(launcher::load_runtime_adjustments(path).text_scale == 2);
    no_staging(fixture.directory());
}

TEST_CASE("Invalid or removed preset slots cannot overwrite runtime data", "[settings][storage][presets][runtime-errors]") {
    Fixture fixture;
    const auto path = fixture.path("presets.runtime.toml");
    const launcher::RuntimeAdjustments snapshot{1.25, 1.5, 1, 0.7, 12U, 3.5};
    const std::string_view invalid[] = {
        "preset=1\n", "preset=[1]\n", "[[preset]]\n", "[[preset]]\nname=3\n",
        "[[preset]]\nname='  '\n", "[[preset]]\nname='A'\n[[preset]]\nname='A'\n",
        "[[preset]]\nname='A'\nunknown=1\n", "[[preset]]\nname='A'\nappearance.thickness=2\n",
        "[[preset]]\nname='A'\nadjustments={text_scale=1,icon_scale=1}\n",
        "[[preset]]\nname='A'\nadjustments={text_scale=1,icon_scale=1}\n"
            "grid.icon_spacing_px=8\nappearance.thickness=2\ndock.position=nan\n",
        "[[preset]]\nname='A'\nadjustments={text_scale=1,icon_scale=1,spacing_scale=1}\n"
            "grid.icon_spacing_px=8\nappearance.thickness=2\ndock.position=0.5\n",
    };
    for (const auto source : invalid) {
        CAPTURE(source);
        write(path, source);
        const auto timestamp = fs::last_write_time(path);
        REQUIRE_THROWS_AS(launcher::load_adjustment_presets(path), launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::load_runtime_adjustments(path), launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::load_active_versions(path), launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::store_adjustment_preset(path, "A", snapshot), launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::save_runtime_adjustments(path, snapshot), launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::set_active_version(path, version_identity(), fixture.path("tool.exe")), launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::reset_active_version(path, version_identity()), launcher::ConfigError);
        REQUIRE(read(path) == source);
        REQUIRE(fs::last_write_time(path) == timestamp);
        no_staging(fixture.directory());
    }
    const std::string empty = "[[preset]]\nname='A'\n";
    write(path, empty);
    REQUIRE_THROWS_AS(launcher::store_adjustment_preset(path, "Removed", snapshot), launcher::ConfigError);
    auto incomplete = snapshot;
    incomplete.edge_position.reset();
    REQUIRE_THROWS_AS(launcher::store_adjustment_preset(path, "A", incomplete), launcher::ConfigError);
    auto invalid_value = snapshot;
    invalid_value.text_scale = 5;
    REQUIRE_THROWS_AS(launcher::store_adjustment_preset(path, "A", invalid_value), launcher::ConfigError);
    {
        FileLock lock(path);
        REQUIRE(lock.valid());
        REQUIRE_THROWS_AS(launcher::store_adjustment_preset(path, "A", snapshot), launcher::ConfigError);
    }
    REQUIRE(read(path) == empty);
    no_staging(fixture.directory());
}

TEST_CASE("Active versions retain exact installation paths and reset items independently", "[settings][storage][active-version]") {
    Fixture fixture;
    const auto path = fixture.path("choices.runtime.toml");
    REQUIRE(launcher::load_active_versions(path).empty());
    REQUIRE_FALSE(launcher::active_version({}, version_identity()));
    launcher::reset_active_version(path, version_identity());
    REQUIRE_FALSE(fs::exists(path));

    const auto first = fixture.directory() / L"Not installed/工具 2025/tool.exe";
    const auto second = fixture.path("Not installed/Tool 2026/tool.exe");
    const auto other = fixture.path("Other/other.exe");
    launcher::set_active_version(path, version_identity(), first);
    launcher::set_active_version(path, version_identity("Other"), other);
    auto restored = launcher::load_active_versions(path);
    REQUIRE(restored.size() == 2);
    REQUIRE(launcher::active_version(restored, version_identity()) == first);
    REQUIRE(launcher::active_version(restored, version_identity("Other")) == other);
    REQUIRE_FALSE(fs::exists(first)); // A removed installation does not erase the choice.
    REQUIRE(read(path).find("\n[[active_version]]\n") != std::string::npos);

    launcher::set_active_version(path, version_identity(), second);
    restored = launcher::load_active_versions(path);
    REQUIRE(restored.size() == 2);
    REQUIRE(launcher::active_version(restored, version_identity()) == second);
    launcher::reset_active_version(path, version_identity());
    restored = launcher::load_active_versions(path);
    REQUIRE(restored.size() == 1);
    REQUIRE_FALSE(launcher::active_version(restored, version_identity()));
    REQUIRE(launcher::active_version(restored, version_identity("Other")) == other);
    launcher::reset_active_version(path, version_identity("Other"));
    REQUIRE(launcher::load_active_versions(path).empty());
    REQUIRE(read(path).find("\n[[active_version]]\n") == std::string::npos);
    no_staging(fixture.directory());
}

TEST_CASE("Active version changes and adjustment saves preserve each other's runtime data", "[settings][storage][active-version][presets]") {
    Fixture fixture;
    const auto path = fixture.path("choices.runtime.toml");
    write(path, "[adjustments]\ntext_scale=1.1\nicon_scale=1.2\nspacing_scale=0.5\n"
        "[dock]\nposition=0.3\n[appearance]\nthickness=2.5\n[[preset]]\nname='Normal'\n");
    const auto executable = fixture.path("Tool 2025/tool.exe");
    launcher::set_active_version(path, version_identity(), executable);
    auto active = launcher::load_runtime_adjustments(path);
    REQUIRE(active.text_scale == 1.1);
    REQUIRE(active.icon_scale == 1.2);
    REQUIRE(active.spacing_scale == 0.5);
    REQUIRE_FALSE(active.icon_spacing_px);
    REQUIRE(active.edge_position == 0.3);
    REQUIRE(active.thickness_dip == 2.5);
    auto presets = launcher::load_adjustment_presets(path);
    REQUIRE(presets.size() == 1);
    REQUIRE(presets[0].name == "Normal");
    REQUIRE_FALSE(presets[0].values);

    const launcher::RuntimeAdjustments snapshot{1.25, 1.5, 1, 0.7, 12U, 3.5};
    launcher::store_adjustment_preset(path, "Normal", snapshot);
    REQUIRE(launcher::active_version(launcher::load_active_versions(path), version_identity()) == executable);
    REQUIRE(launcher::load_runtime_adjustments(path).spacing_scale == 0.5);
    launcher::save_runtime_adjustments(path, {2, 0.75, 1, 0.8, 6U, 0});
    REQUIRE(launcher::active_version(launcher::load_active_versions(path), version_identity()) == executable);
    launcher::reset_active_version(path, version_identity());
    active = launcher::load_runtime_adjustments(path);
    REQUIRE(active.text_scale == 2);
    REQUIRE(active.icon_scale == 0.75);
    REQUIRE(active.icon_spacing_px == 6U);
    REQUIRE(active.edge_position == 0.8);
    REQUIRE(active.thickness_dip == 0);
    presets = launcher::load_adjustment_presets(path);
    REQUIRE(presets[0].values->text_scale == snapshot.text_scale);
    REQUIRE(presets[0].values->icon_scale == snapshot.icon_scale);
    REQUIRE(presets[0].values->icon_spacing_px == snapshot.icon_spacing_px);
    REQUIRE(presets[0].values->edge_position == snapshot.edge_position);
    REQUIRE(presets[0].values->thickness_dip == snapshot.thickness_dip);
    no_staging(fixture.directory());
}

TEST_CASE("Active versions read named TOML tables without an encoded item string", "[settings][storage][active-version]") {
    Fixture fixture;
    const auto path = fixture.path("choices.runtime.toml");
    write(path, active_version_source());
    const auto versions = launcher::load_active_versions(path);
    REQUIRE(versions.size() == 1);
    REQUIRE(versions.front().item == version_identity());
    REQUIRE(launcher::active_version(versions, version_identity()) == fs::path(L"C:\\Tool\\tool.exe"));
}

TEST_CASE("Active version records round trip complete typed identities with literal Unicode paths", "[settings][storage][active-version]") {
    Fixture fixture;
    const auto path = fixture.path("choices.runtime.toml");
    auto item = version_identity("工具 : | [x] ' \" \n");
    item.tab = "工作 : | [tools]";
    item.launch.handler = "custom:handler";
    item.launch.target = R"(C:\工具\%WINDIR%\$$literal\Tool-$version\tool.exe)";
    item.launch.arguments = "--label='工具: | []' --literal=$EXE %WINDIR%";
    item.launch.working_directory = R"(C:\工具\%WINDIR%\$$literal)";
    item.launch.show = launcher::LaunchShow::hidden;
    item.launch.include_beta = false;
    item.icon_explicit = true;
    item.occurrence = 7;
    const fs::path icon_path = L"C:\\工具\\%WINDIR%\\$$literal\\icons.dll";
    std::string expected_kind;
    SECTION("Named stock icon") { item.icon = launcher::StockIcon::shield; expected_kind = "stock = 'shield'"; }
    SECTION("Decoded file icon") { item.icon = launcher::FileIcon{icon_path}; expected_kind = "file = "; }
    SECTION("Shell icon") { item.icon = launcher::ShellItemIcon{icon_path}; expected_kind = "shell = "; }
    SECTION("Icon group index") { item.icon = launcher::IconGroup{icon_path, launcher::IconIndex{2}}; expected_kind = "index = 2"; }
    SECTION("Icon group resource id") { item.icon = launcher::IconGroup{icon_path, launcher::IconResourceId{2}}; expected_kind = "id = 2"; }
    SECTION("Named resource without language") {
        item.icon = launcher::PeResourceIcon{icon_path, std::string("工具:type|[]"), std::string("1:name|[]"), std::nullopt};
        expected_kind = "resource = ";
    }
    SECTION("Numeric resource and language zero") {
        item.icon = launcher::PeResourceIcon{icon_path, std::uint16_t{10}, std::uint16_t{1}, std::uint16_t{0}};
        item.launch.show = launcher::LaunchShow::minimized;
        expected_kind = "language = 0";
    }
    const auto executable = fixture.directory() / L"工具 %WINDIR% $$literal/tool.exe";
    launcher::set_active_version(path, item, executable);
    auto versions = launcher::load_active_versions(path);
    REQUIRE(versions.size() == 1);
    REQUIRE(versions.front().item == item);
    REQUIRE(launcher::active_version(versions, item) == executable);
    auto other = item;
    ++other.occurrence;
    launcher::set_active_version(path, other, fixture.path("other.exe"));
    versions = launcher::load_active_versions(path);
    REQUIRE(versions.size() == 2);
    REQUIRE(launcher::active_version(versions, item) == executable);
    REQUIRE(launcher::active_version(versions, other) == fixture.path("other.exe"));
    const auto source = read(path);
    REQUIRE(source.find("[active_version.item]") != source.npos);
    REQUIRE(source.find("[active_version.item.launch]") != source.npos);
    REQUIRE(source.find("[active_version.item.icon]") != source.npos);
    REQUIRE(source.find(expected_kind) != source.npos);
    REQUIRE(source.find("item = 'v1:") == source.npos);
    REQUIRE(source.find("environment_expanded") == source.npos);
    REQUIRE((source.find("show = 'hidden'") != source.npos || source.find("show = 'minimized'") != source.npos));
    no_staging(fixture.directory());
}

TEST_CASE("Active version records retain Adobe search roots without expanding literals", "[settings][storage][active-version][adobe-roots]") {
    Fixture fixture;
    const auto path = fixture.path("adobe-roots.runtime.toml");
    auto item = version_identity();
    item.launch.handler = "adobeCC";
    item.launch.target = "Photoshop";
    item.launch.root_paths = {L"C:\\Adobe", L"D:\\Adobe\\%WINDIR%\\$literal", L"portable"};
    const auto executable = fixture.path("Photoshop.exe");
    launcher::set_active_version(path, item, executable);
    const auto versions = launcher::load_active_versions(path);
    REQUIRE(versions.size() == 1);
    CHECK(versions.front().item == item);
    auto other = item;
    other.launch.root_paths = {L"E:\\Adobe"};
    CHECK_FALSE(launcher::active_version(versions, other));
}

TEST_CASE("Malformed active version entries cannot be loaded or overwritten", "[settings][storage][active-version]") {
    Fixture fixture;
    const auto path = fixture.path("choices.runtime.toml");
    const auto executable = fixture.path("Tool/tool.exe");
    const launcher::RuntimeAdjustments snapshot{1.25, 1.5, 1, 0.7, 12U, 3.5};
    const auto valid = active_version_source();
    const std::vector<std::string> invalid = {
        "active_version=1\n", "active_version=[1]\n", "[active_version]\nitem='Tool'\n",
        "[[active_version]]\n", "[[active_version]]\nitem=3\nexecutable='C:\\Tool\\tool.exe'\n",
        "[[active_version]]\nitem='v1:5:Tools1:x1:0'\nexecutable='C:\\Tool\\tool.exe'\n",
        changed_source(valid, "executable = 'C:\\Tool\\tool.exe'", "executable = 3"),
        changed_source(valid, "executable = 'C:\\Tool\\tool.exe'", "executable = ['C:\\Tool\\tool.exe']"),
        changed_source(valid, "executable = 'C:\\Tool\\tool.exe'", "executable = ''"),
        changed_source(valid, "executable = 'C:\\Tool\\tool.exe'", "executable = 'relative\\tool.exe'"),
        changed_source(valid, "executable = 'C:\\Tool\\tool.exe'", "executable = 'C:tool.exe'"),
        changed_source(valid, "executable = 'C:\\Tool\\tool.exe'", "executable = 'C:\\Tool\\'"),
        changed_source(valid, "executable = 'C:\\Tool\\tool.exe'", "executable = \"C:\\\\tool\\u0000.exe\""),
        changed_source(valid, "[active_version.item]", "unknown = true\n[active_version.item]"),
        changed_source(valid, "tab = 'Tools'", "tab = ''"),
        changed_source(valid, "label = 'Tool'", "label = \"a\\u0000b\""),
        changed_source(valid, "label_explicit = true", "label_explicit = 1"),
        changed_source(valid, "occurrence = 0", "occurrence = -1"),
        changed_source(valid, "occurrence = 0", "occurrence = 0.5"),
        changed_source(valid, "occurrence = 0", "occurrence = 0\nunknown = true"),
        changed_source(valid, "handler = 'shell'", "handler = ''"),
        changed_source(valid, "arguments = ''", "arguments = 1"),
        changed_source(valid, "show = 'normal'", "show = 'maximized'"),
        changed_source(valid, "include_beta = true", "include_beta = 'true'"),
        changed_source(valid, "include_beta = true", "include_beta = true\ntarget_environment_expanded = true"),
        changed_source(valid, "stock = 'application'", "stock = 'nonexistent'"),
        changed_source(valid, "stock = 'application'", "stock = 'application'\nfile = 'C:\\Icons\\tool.ico'"),
        changed_source(valid, "stock = 'application'", "file = 'C:\\Icons\\tool.ico'\nindex = -1"),
        changed_source(valid, "stock = 'application'", "file = 'C:\\Icons\\tool.ico'\nindex = 0\nid = 1"),
        changed_source(valid, "stock = 'application'", "file = 'C:\\Icons\\tool.ico'\nid = 65536"),
        changed_source(valid, "stock = 'application'", "file = 'C:\\Icons\\tool.ico'\nenvironment_expanded = true"),
        changed_source(valid, "stock = 'application'", "shell = ''"),
        changed_source(valid, "stock = 'application'", "resource = 'C:\\Icons\\tool.dll'\ntype = 10\nname = true"),
        changed_source(valid, "stock = 'application'", "resource = 'C:\\Icons\\tool.dll'\ntype = 10\nname = 1\nlanguage = 65536"),
        changed_source(valid, "stock = 'application'", "resource = 'C:\\Icons\\tool.dll'\ntype = ''\nname = 1"),
        valid + valid,
    };
    for (const auto& source : invalid) {
        CAPTURE(source);
        write(path, source);
        REQUIRE_THROWS_AS(launcher::load_active_versions(path), launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::load_runtime_adjustments(path), launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::load_adjustment_presets(path), launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::set_active_version(path, version_identity("Other"), executable), launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::reset_active_version(path, version_identity()), launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::save_runtime_adjustments(path, snapshot), launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::store_adjustment_preset(path, "Normal", snapshot), launcher::ConfigError);
        REQUIRE(read(path) == source);
    }
    no_staging(fixture.directory());
}

TEST_CASE("Invalid or failed active version updates preserve the runtime document", "[settings][storage][active-version]") {
    Fixture fixture;
    const auto path = fixture.path("choices.runtime.toml");
    const auto executable = fixture.path("Tool/tool.exe");
    const std::string original = "# My original settings\n[adjustments]\ntext_scale=1.1\n";
    write(path, original);
    REQUIRE_THROWS_AS(launcher::set_active_version(path, {}, executable), launcher::ConfigError);
    auto invalid = version_identity();
    invalid.tab = std::string("a\0b", 3);
    REQUIRE_THROWS_AS(launcher::set_active_version(path, invalid, executable), launcher::ConfigError);
    REQUIRE_THROWS_AS(launcher::set_active_version(path, version_identity(), "tool.exe"), launcher::ConfigError);
    REQUIRE_THROWS_AS(launcher::reset_active_version(path, invalid), launcher::ConfigError);
    {
        FileLock lock(path);
        REQUIRE(lock.valid());
        REQUIRE_THROWS_AS(launcher::set_active_version(path, version_identity(), executable), launcher::ConfigError);
    }
    REQUIRE(read(path) == original);
    launcher::set_active_version(path, version_identity(), executable);
    const auto selected = read(path);
    {
        FileLock lock(path);
        REQUIRE(lock.valid());
        REQUIRE_THROWS_AS(launcher::reset_active_version(path, version_identity()), launcher::ConfigError);
    }
    REQUIRE(read(path) == selected);
    no_staging(fixture.directory());
}

TEST_CASE("Version item identities follow launch bindings rather than grid positions", "[settings][active-version]") {
    launcher::Item tool;
    tool.launch = launcher::LaunchAction{"shell", R"(C:\Tools\Tool-$version\tool.exe)"};
    launcher::Item unrelated;
    unrelated.launch = launcher::LaunchAction{"shell", R"(C:\Other\other.exe)"};
    launcher::Configuration base;
    launcher::Tab tools;
    tools.name = "Tools";
    tools.items = {tool, unrelated};
    base.tabs.push_back(tools);
    const auto identity = launcher::version_item_identity(base, 0, 0);
    REQUIRE(identity == launcher::version_item_identity(base, 0, 0));

    auto moved = base;
    moved.tabs[0].items.insert(moved.tabs[0].items.begin(), unrelated);
    moved.tabs[0].short_name = "T";
    launcher::Tab other;
    other.name = "Other";
    other.items = {tool};
    moved.tabs.insert(moved.tabs.begin(), other);
    REQUIRE(launcher::version_item_identity(moved, 1, 1) == identity);
    std::swap(moved.tabs[1].items[0], moved.tabs[1].items[1]);
    REQUIRE(launcher::version_item_identity(moved, 1, 0) == identity);
    REQUIRE(launcher::version_item_identity(moved, 0, 0) != identity);
    moved.tabs[1].name = "Renamed";
    REQUIRE(launcher::version_item_identity(moved, 1, 0) != identity);

    auto duplicates = base;
    duplicates.tabs[0].items.push_back(tool);
    REQUIRE(launcher::version_item_identity(duplicates, 0, 2) != identity);
    duplicates.tabs.push_back(tools);
    REQUIRE(launcher::version_item_identity(duplicates, 1, 0) != identity);
    REQUIRE(launcher::version_item_identity(duplicates, 1, 0) != launcher::version_item_identity(duplicates, 0, 2));

    auto changed = base;
    SECTION("Arguments distinguish choices") { changed.tabs[0].items[0].launch->arguments = "--preview"; }
    SECTION("Working directories distinguish choices") { changed.tabs[0].items[0].launch->working_directory = R"(C:\Work)"; }
    SECTION("Window states distinguish choices") { changed.tabs[0].items[0].launch->show = launcher::LaunchShow::hidden; }
    SECTION("Beta filters distinguish choices") { changed.tabs[0].items[0].launch->include_beta = false; }
    SECTION("Handlers distinguish choices") { changed.tabs[0].items[0].launch->handler = "adobeCC"; }
    SECTION("Targets distinguish choices") { changed.tabs[0].items[0].launch->target += ".other"; }
    SECTION("Labels distinguish choices") { changed.tabs[0].items[0].label = "Legacy"; }
    SECTION("Explicit labels distinguish choices") { changed.tabs[0].items[0].label_explicit = true; }
    SECTION("Explicit icons distinguish choices") { changed.tabs[0].items[0].icon_explicit = true; }
    SECTION("Stock icons distinguish choices") { changed.tabs[0].items[0].icon_source = launcher::StockIcon::folder; }
    REQUIRE(launcher::version_item_identity(changed, 0, 0) != identity);
    REQUIRE_THROWS_AS(launcher::version_item_identity(base, 1, 0), launcher::ConfigError);
    REQUIRE_THROWS_AS(launcher::version_item_identity(base, 0, 2), launcher::ConfigError);
    changed.tabs[0].items[0].launch.reset();
    REQUIRE_THROWS_AS(launcher::version_item_identity(changed, 0, 0), launcher::ConfigError);
}

TEST_CASE("Active choices follow equal launch actions with distinct labels or icons when reordered", "[settings][storage][active-version]") {
    Fixture fixture;
    const auto path = fixture.path("choices.runtime.toml");
    launcher::Item first;
    first.launch = launcher::LaunchAction{"shell", R"(C:\Tools\Tool-$version\tool.exe)"};
    first.label = "Stable";
    first.label_explicit = true;
    launcher::Item second = first;
    SECTION("Labels distinguish equal launch actions") { second.label = "Legacy"; }
    SECTION("File icons distinguish equal launch actions") { second.icon_source = launcher::FileIcon{L"C:\\Icons\\legacy.ico"}; }
    SECTION("Shell icons distinguish equal launch actions") { second.icon_source = launcher::ShellItemIcon{L"C:\\Icons\\legacy.ico"}; }
    SECTION("Icon selectors distinguish equal launch actions") {
        first.icon_source = launcher::IconGroup{L"C:\\Icons\\icons.dll", launcher::IconIndex{1}};
        second.icon_source = launcher::IconGroup{L"C:\\Icons\\icons.dll", launcher::IconResourceId{1}};
    }
    SECTION("Resource identifiers distinguish equal launch actions") {
        first.icon_source = launcher::PeResourceIcon{L"C:\\Icons\\icons.dll", std::uint16_t(10), std::uint16_t(1)};
        second.icon_source = launcher::PeResourceIcon{L"C:\\Icons\\icons.dll", std::uint16_t(10), std::string("1")};
    }
    SECTION("Resource languages distinguish equal launch actions") {
        first.icon_source = launcher::PeResourceIcon{L"C:\\Icons\\icons.dll", std::uint16_t(10), std::uint16_t(1), std::nullopt};
        second.icon_source = launcher::PeResourceIcon{L"C:\\Icons\\icons.dll", std::uint16_t(10), std::uint16_t(1), std::uint16_t{0}};
    }
    launcher::Tab tab;
    tab.name = "Tools";
    tab.items = {first, second};
    launcher::Configuration configuration;
    configuration.tabs.push_back(tab);
    const auto first_identity = launcher::version_item_identity(configuration, 0, 0);
    const auto second_identity = launcher::version_item_identity(configuration, 0, 1);
    REQUIRE(first_identity != second_identity);
    const auto first_executable = fixture.path("Tool 2026/tool.exe");
    const auto second_executable = fixture.path("Tool 2025/tool.exe");
    launcher::set_active_version(path, first_identity, first_executable);
    launcher::set_active_version(path, second_identity, second_executable);
    std::swap(configuration.tabs[0].items[0], configuration.tabs[0].items[1]);
    const auto choices = launcher::load_active_versions(path);
    REQUIRE(launcher::active_version(choices, launcher::version_item_identity(configuration, 0, 0)) == second_executable);
    REQUIRE(launcher::active_version(choices, launcher::version_item_identity(configuration, 0, 1)) == first_executable);
}

TEST_CASE("Built-in EXE follows the executable in portable and installed configurations", "[config][paths][storage][exe]") {
    Fixture fixture;
    const auto config_dir = fixture.path("Settings");
    fs::create_directory(config_dir);
    const auto text = [](const fs::path& path) {
        const auto utf8 = path.u8string();
        return std::string(utf8.begin(), utf8.end());
    };
    const std::string source = R"([[tabs]]
name = 'General'
items = [
    { label = '$EXE stays literal', target = '$EXE\LauncherDrop.exe', icon = {file='$EXE\LauncherDrop.exe',index=0}, arguments = '$EXE\Read Me.txt' },
    { target = 'Documents\notes.txt', working_directory = 'Working folder' },
    { target = 'notepad.exe', arguments = '"$EXE\docs\Launcher.md"' },
]
)";
    for (const auto layout : {"Portable ZIP", "Program Files/Launcher", "AppData/Programs/Launcher"}) {
        CAPTURE(layout);
        const auto executable_dir = fixture.path(layout).lexically_normal();
        fs::create_directories(executable_dir);
        write(executable_dir / "LauncherDrop.exe", "icon lookup fixture");
        const auto config = (std::string_view(layout) == "Portable ZIP" ? executable_dir : config_dir) / "Launcher.toml";
        write(config, source);
        const auto loaded = launcher::load_configuration_files(config, config_dir, executable_dir);
        const auto& items = loaded.configuration.tabs.front().items;
        REQUIRE(items[0].label == "$EXE stays literal");
        REQUIRE(items[0].launch->target == text(executable_dir / "LauncherDrop.exe"));
        REQUIRE(items[0].launch->arguments == text(executable_dir / "Read Me.txt"));
        REQUIRE(std::get<launcher::IconGroup>(items[0].icon_source).path == executable_dir / "LauncherDrop.exe");
        REQUIRE(items[1].launch->target == R"(Documents\notes.txt)");
        REQUIRE(items[1].launch->working_directory == "Working folder");
        REQUIRE(items[2].launch->arguments == '"' + text(executable_dir / "docs" / "Launcher.md") + '"');
    }
}

TEST_CASE("Built-in EXE protects actual directory symbols and coexists with authored paths", "[config][paths][storage][exe]") {
    Fixture fixture;
    const auto executable_dir = fixture.directory() / L"工具 %WINDIR% $$literal $EXE";
    fs::create_directory(executable_dir);
    write(executable_dir / "helper.ico", "icon lookup fixture");
    const auto config = fixture.path("Launcher.toml");
    write(config, R"([paths]
Other = 'Relative folder'
[[tabs]]
name = 'General'
items = [
    { target = '$EXE\LauncherDrop.exe', icon = '$EXE\helper.ico', arguments = '$EXE\Read Me.txt' },
    { target = '$Other\notes.txt' },
    { target = '$$EXE\literal.txt' },
    { target = 'notepad.exe', arguments = '"$EXE\Read Me.txt" --literal %WINDIR% $$/' },
]
)");
    const auto loaded = launcher::load_configuration_files(config, {}, executable_dir);
    const auto& items = loaded.configuration.tabs.front().items;
    const auto utf8 = (executable_dir / "LauncherDrop.exe").u8string();
    REQUIRE(items[0].launch->target == std::string(utf8.begin(), utf8.end()));
    REQUIRE(std::get<launcher::FileIcon>(items[0].icon_source).path == executable_dir / "helper.ico");
    const auto argument = (executable_dir / "Read Me.txt").u8string();
    REQUIRE(items[0].launch->arguments == std::string(argument.begin(), argument.end()));
    REQUIRE(items[1].launch->target == R"(Relative folder\notes.txt)");
    REQUIRE(items[2].launch->target == R"($EXE\literal.txt)");
    REQUIRE(items[3].launch->arguments == '"' + std::string(argument.begin(), argument.end()) + R"(" --literal %WINDIR% $/)");
}

TEST_CASE("Built-in EXE discovers the running module when no directory is supplied", "[config][paths][storage][exe]") {
    std::wstring module(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    REQUIRE(length > 0);
    REQUIRE(length < module.size());
    module.resize(length);
    const auto expected = (fs::path(module).parent_path() / "LauncherDrop.exe").u8string();
    const std::string source = "[[tabs]]\nname='General'\nitems=[{target='$EXE\\LauncherDrop.exe'}]\n";
    const auto parsed = launcher::parse_configuration(source);
    REQUIRE(parsed.tabs.front().items.front().launch->target == std::string(expected.begin(), expected.end()));
    Fixture fixture;
    const auto config = fixture.path("Launcher.toml");
    write(config, source);
    const auto loaded = launcher::load_configuration(config);
    REQUIRE(loaded.tabs.front().items.front().launch->target == std::string(expected.begin(), expected.end()));
    REQUIRE_THROWS_AS(launcher::parse_configuration("[paths]\nEXE='elsewhere'\n" + source), launcher::ConfigError);
    REQUIRE_THROWS_AS(launcher::parse_configuration("[[tabs]]\nname='General'\nitems=[{target='$exe\\helper.exe'}]\n"), launcher::ConfigError);
}

TEST_CASE("Built-in SELF follows a versioned portable executable using the AppData configuration", "[config][paths][storage][self]") {
    Fixture fixture;
    const auto portable = fixture.directory() / L"工具 %WINDIR% $$literal $EXE";
    const auto user = fixture.path("AppData");
    fs::create_directory(portable);
    fs::create_directory(user);
    const auto executable = portable / "Launcher-R1.1.0.14.exe";
    write(executable, "versioned executable fixture");
    const auto config = user / "Launcher.toml";
    write(config, R"([[tabs]]
name = 'Launcher'
items = [
    { label = '$SELF stays literal', target = '$SELF', arguments = '"%LOCALAPPDATA%\Długosz\Launcher\Demos\demo_many_colors.toml"' },
    { target = 'notepad.exe', arguments = '"$SELF" --literal %WINDIR% $$/' },
    { target = '$$SELF' },
    { target = '$EXE\LauncherDrop.exe' },
]
)");
    const launcher::FileLookup files(executable, user);
    REQUIRE(files.portable());
    REQUIRE_FALSE(fs::exists(portable / "Launcher.exe"));
    REQUIRE_FALSE(fs::exists(portable / "Launcher.toml"));
    REQUIRE(files.require("Launcher.toml") == config);
    const auto loaded = launcher::load_configuration_files(files.require("Launcher.toml"), files);
    const auto& items = loaded.configuration.tabs.front().items;
    const auto utf8 = executable.u8string();
    const std::string expected(utf8.begin(), utf8.end());
    REQUIRE(items[0].label == "$SELF stays literal");
    REQUIRE(items[0].launch->target == expected);
    REQUIRE(items[0].launch->arguments == R"("%LOCALAPPDATA%\Długosz\Launcher\Demos\demo_many_colors.toml")");
    REQUIRE(items[1].launch->arguments == '"' + expected + R"(" --literal %WINDIR% $/)");
    REQUIRE(items[2].launch->target == "$SELF");
    const auto companion = (portable / "LauncherDrop.exe").u8string();
    REQUIRE(items[3].launch->target == std::string(companion.begin(), companion.end()));
}

TEST_CASE("Built-in SELF discovers the running module and cannot be redefined", "[config][paths][storage][self]") {
    std::wstring module(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    REQUIRE(length > 0);
    REQUIRE(length < module.size());
    module.resize(length);
    const auto utf8 = fs::path(module).u8string();
    const std::string expected(utf8.begin(), utf8.end());
    const std::string source = "[[tabs]]\nname='Launcher'\nitems=[{target='$SELF'}]\n";
    REQUIRE(launcher::parse_configuration(source).tabs.front().items.front().launch->target == expected);
    Fixture fixture;
    const auto config = fixture.path("Launcher.toml");
    write(config, source);
    REQUIRE(launcher::load_configuration(config).tabs.front().items.front().launch->target == expected);
    REQUIRE_THROWS_AS(launcher::parse_configuration("[paths]\nSELF='elsewhere'\n" + source), launcher::ConfigError);
    REQUIRE_THROWS_AS(launcher::parse_configuration("[[tabs]]\nname='Launcher'\nitems=[{target='$self'}]\n"), launcher::ConfigError);
}

TEST_CASE("Built-in USERDATA supplies an exact quoted demo filename to launch preparation", "[config][paths][storage][userdata]") {
    Fixture fixture;
    const auto portable = fixture.path("Portable ZIP");
    const auto user = fixture.directory() / L"AppData 用户 %WINDIR% $$literal $USERDATA";
    fs::create_directory(portable);
    fs::create_directories(user / "Demos");
    const auto executable = portable / "Launcher-R1.1.0.14.exe";
    write(executable, "versioned executable fixture");
    const auto demo = user / "Demos" / "demo_many_colors.toml";
    write(demo, "[[tabs]]\nname='Color demo'\n");
    const auto config = user / "Launcher.toml";
    write(config, R"([[tabs]]
name = 'Launcher'
items = [
    { target = '$SELF', arguments = '"$USERDATA\Demos\demo_many_colors.toml"' },
    { target = 'notepad.exe', arguments = '"$USERDATA\Demos\demo_many_colors.toml"' },
    { target = '$SELF', arguments = '--untouched %LOCALAPPDATA% %WINDIR% $$literal $/cost' },
    { target = '$USERDATA\Demos\demo_many_colors.toml' },
    { target = '$$USERDATA' },
]
)");
    const launcher::FileLookup files(executable, user);
    const auto loaded = launcher::load_configuration_files(files.require("Launcher.toml"), files);
    const auto& items = loaded.configuration.tabs.front().items;
    const auto utf8 = demo.u8string();
    const std::string expected(utf8.begin(), utf8.end());
    REQUIRE(items[0].launch->arguments == '"' + expected + '"');
    REQUIRE(items[1].launch->target == "notepad.exe");
    REQUIRE(items[1].launch->arguments == '"' + expected + '"');
    REQUIRE(items[2].launch->arguments == "--untouched %LOCALAPPDATA% %WINDIR% $literal %cost");
    REQUIRE(items[3].launch->target == expected);
    REQUIRE(items[4].launch->target == "$USERDATA");
    REQUIRE(fs::exists(demo));
    for (const auto item : {0, 1}) {
        const auto prepared = launcher::prepare_launch_action(*items[item].launch, config.parent_path());
        REQUIRE(prepared.result.success);
        REQUIRE(prepared.action.has_value());
        REQUIRE(prepared.action->arguments == L'"' + demo.wstring() + L'"');
        if (item == 0) { REQUIRE(prepared.action->target == executable.wstring()); }
        const auto command = L'"' + prepared.action->target + L"\" " + prepared.action->arguments;
        int count = 0;
        auto** arguments = CommandLineToArgvW(command.c_str(), &count);
        const std::unique_ptr<wchar_t*, decltype(&LocalFree)> owner(arguments, LocalFree);
        REQUIRE(arguments != nullptr);
        REQUIRE(count == 2);
        REQUIRE(fs::path(arguments[1]) == demo);
    }
    const auto untouched = launcher::prepare_launch_action(*items[2].launch, config.parent_path());
    REQUIRE(untouched.result.success);
    REQUIRE(untouched.action.has_value());
    REQUIRE(untouched.action->arguments == L"--untouched %LOCALAPPDATA% %WINDIR% $literal %cost");
}

TEST_CASE("Built-in USERDATA discovers the local Launcher profile and cannot be redefined", "[config][paths][storage][userdata]") {
    PWSTR local = nullptr;
    const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &local);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owner(local, CoTaskMemFree);
    REQUIRE(SUCCEEDED(result));
    REQUIRE(local != nullptr);
    const auto utf8 = (fs::path(local) / L"Długosz" / "Launcher" / "notes.txt").u8string();
    const std::string expected(utf8.begin(), utf8.end());
    const std::string source = "[[tabs]]\nname='Launcher'\nitems=[{target='$USERDATA\\notes.txt'}]\n";
    REQUIRE(launcher::parse_configuration(source).tabs.front().items.front().launch->target == expected);
    Fixture fixture;
    const auto config = fixture.path("Launcher.toml");
    write(config, source);
    REQUIRE(launcher::load_configuration(config).tabs.front().items.front().launch->target == expected);
    REQUIRE_THROWS_AS(launcher::parse_configuration("[paths]\nUSERDATA='elsewhere'\n" + source), launcher::ConfigError);
    REQUIRE_THROWS_AS(launcher::parse_configuration("[[tabs]]\nname='Launcher'\nitems=[{target='$userdata'}]\n"), launcher::ConfigError);
}

TEST_CASE("Selected color files search explicit configuration then portable EXE then AppData then resources", "[colors][storage][file-lookup]") {
    Fixture fixture;
    const auto config_dir = fixture.path("config");
    const auto user_dir = fixture.path("AppData");
    const auto portable_dir = fixture.path("Portable");
    fs::create_directory(config_dir);
    fs::create_directory(user_dir);
    fs::create_directory(portable_dir);
    const auto config = config_dir / "Launcher.toml";
    const std::string source = "colors='LiGhT'\n[[tabs]]\nname='General'\ncolor='orange'\n";
    write(config, source);
    const auto palette = [](std::string_view outline) {
        return "[colors.orange]\ntab=[{at=0,color='#FFF8BB'},{at=1,color='#FFF8BB'}]\n"
            "page=[{at=0,color='#FFF8BB'},{at=1,color='#FFD66B'}]\noutline='" + std::string(outline) + "'\n";
    };
    write(portable_dir / "colors-light.toml", palette("#112233"));
    write(user_dir / "colors-light.toml", palette("#223344"));
    write(config_dir / "colors-light.toml", palette("#334455"));
    auto loaded = launcher::load_configuration_files(config, user_dir, portable_dir);
    REQUIRE(loaded.configuration.color_sets == std::vector<std::string>{"light"});
    REQUIRE(loaded.color_sources.size() == 3);
    REQUIRE(loaded.color_sources[0].ends_with("!resource:basic_colors.toml"));
    REQUIRE(loaded.color_sources[1].ends_with("!resource:extra_colors.toml"));
    REQUIRE(fs::path(loaded.color_sources.back()) == config_dir / "colors-light.toml");
    REQUIRE(loaded.configuration.tabs.front().colors.outline == 0x334455);
    fs::remove(config_dir / "colors-light.toml");
    loaded = launcher::load_configuration_files(config, user_dir, portable_dir);
    REQUIRE(loaded.configuration.tabs.front().colors.outline == 0x112233);
    REQUIRE(fs::path(loaded.color_sources.back()) == portable_dir / "colors-light.toml");
    fs::remove(portable_dir / "colors-light.toml");
    loaded = launcher::load_configuration_files(config, user_dir, portable_dir);
    REQUIRE(loaded.configuration.tabs.front().colors.outline == 0x223344);
    REQUIRE(fs::path(loaded.color_sources.back()) == user_dir / "colors-light.toml");
    fs::remove(user_dir / "colors-light.toml");
    loaded = launcher::load_configuration_files(config, user_dir, portable_dir);
    REQUIRE(loaded.color_sources.back().ends_with("!resource:colors-light.toml"));
    REQUIRE(loaded.configuration.tabs.front().colors.outline == 0xb77a39);
    REQUIRE(loaded.configuration.tabs.front().colors.outline != 0x112233);

    // The settings directory and AppData may be the same directory.
    write(user_dir / "Launcher.toml", source);
    write(user_dir / "colors-light.toml", palette("#445566"));
    loaded = launcher::load_configuration_files(user_dir / "Launcher.toml", user_dir, portable_dir);
    REQUIRE(loaded.configuration.tabs.front().colors.outline == 0x445566);
    REQUIRE(loaded.color_sources.size() == 3);
    REQUIRE(fs::path(loaded.color_sources.back()) == user_dir / "colors-light.toml");
}

TEST_CASE("Existing invalid or unreadable color files cannot fall through to resources", "[colors][storage]") {
    Fixture fixture;
    const auto config_dir = fixture.path("config");
    const auto user_dir = fixture.path("AppData");
    fs::create_directory(config_dir);
    fs::create_directory(user_dir);
    const auto config = config_dir / "Launcher.toml";
    const auto selected = config_dir / "colors-light.toml";
    write(config, "colors='light'\n[[tabs]]\nname='General'\ncolor='orange'\n");
    write(user_dir / "colors-light.toml", "[colors.orange]\npartial=true\noutline='#223344'\n");
    write(selected, "broken TOML[");
    REQUIRE_THROWS_AS(launcher::load_configuration_files(config, user_dir, fixture.directory()), launcher::ConfigError);
    write(selected, "[colors.orange]\npartial=true\noutline='#334455'\n");
    {
        FileLock lock(selected);
        REQUIRE(lock.valid());
        REQUIRE_THROWS_AS(launcher::load_configuration_files(config, user_dir, fixture.directory()), launcher::ConfigError);
    }
    fs::remove(selected);
    fs::create_directory(selected);
    REQUIRE_THROWS_AS(launcher::load_configuration_files(config, user_dir, fixture.directory()), launcher::ConfigError);
    fs::remove(selected);
    write(config, "colors='not-installed'\n[[tabs]]\nname='General'\n");
    REQUIRE_THROWS_AS(launcher::load_configuration_files(config, user_dir, fixture.directory()), launcher::ConfigError);
}

TEST_CASE("Explicit icon files use the same roots and preserve their relative subdirectories", "[config][storage][icons][file-lookup]") {
    Fixture fixture;
    const auto config_dir = fixture.path("Settings");
    const auto executable_dir = fixture.path("Portable");
    const auto user_dir = fixture.path("AppData");
    for (const auto& root : {config_dir, executable_dir, user_dir}) {
        fs::create_directories(root / "Icons");
        write(root / "Icons/brand.svg", "icon lookup fixture");
        write(root / "Icons/catalog.dll", "resource lookup fixture");
    }
    const auto config = config_dir / "rightside.toml";
    const std::string source = R"([[tabs]]
name='Tools'
items=[
    {icon='Icons\brand.svg',target='Relative\tool.exe'},
    {icon={file='Icons\catalog.dll',index=1}},
    {icon={file='Icons\catalog.dll',type=10,id=2}},
    {icon={stock='folder'}},
]
)";
    write(config, source);
    const launcher::FileLookup files(executable_dir / "Launcher.exe", user_dir, config_dir, true);
    for (const auto& expected : {config_dir, executable_dir, user_dir}) {
        const auto loaded = launcher::load_configuration_files(config, files);
        const auto& items = loaded.configuration.tabs.front().items;
        REQUIRE(std::get<launcher::FileIcon>(items[0].icon_source).path == expected / "Icons/brand.svg");
        REQUIRE(std::get<launcher::IconGroup>(items[1].icon_source).path == expected / "Icons/catalog.dll");
        REQUIRE(std::get<launcher::PeResourceIcon>(items[2].icon_source).path == expected / "Icons/catalog.dll");
        REQUIRE(std::get<launcher::StockIcon>(items[3].icon_source) == launcher::StockIcon::folder);
        REQUIRE(items[0].launch->target == R"(Relative\tool.exe)");
        REQUIRE(loaded.icon_sources.size() == 2); // One log entry per resolved file.
        REQUIRE(loaded.icon_warnings.empty());
        fs::remove(expected / "Icons/brand.svg");
        fs::remove(expected / "Icons/catalog.dll");
    }
    // Parsing text validates syntax without requiring the author's files.
    const auto parsed = launcher::parse_configuration(source);
    REQUIRE(std::get<launcher::FileIcon>(parsed.tabs.front().items[0].icon_source).path == L"Icons\\brand.svg");
    const auto missing = launcher::load_configuration_files(config, files);
    REQUIRE(missing.icon_sources.empty());
    REQUIRE(missing.icon_warnings.size() == 2);
    REQUIRE(std::get<launcher::FileIcon>(missing.configuration.tabs.front().items[0].icon_source).path == config_dir / "Icons/brand.svg");
    REQUIRE(std::get<launcher::IconGroup>(missing.configuration.tabs.front().items[1].icon_source).path == config_dir / "Icons/catalog.dll");
    REQUIRE(std::get<launcher::PeResourceIcon>(missing.configuration.tabs.front().items[2].icon_source).path == config_dir / "Icons/catalog.dll");
    for (const auto& root : {config_dir, executable_dir, user_dir}) {
        const auto searched = (root / "Icons/brand.svg").make_preferred().u8string();
        REQUIRE(missing.icon_warnings.front().find(std::string(searched.begin(), searched.end())) != std::string::npos);
    }
    REQUIRE(missing.icon_warnings.front().find("(2)") != std::string::npos);
}

TEST_CASE("An explicitly named AppData configuration keeps portable precedence for its assets", "[config][storage][file-lookup]") {
    Fixture fixture;
    const auto executable_dir = fixture.path("Portable");
    const auto user_dir = fixture.path("AppData");
    fs::create_directory(executable_dir);
    fs::create_directory(user_dir);
    const auto config = user_dir / "rightside.toml";
    write(config, "colors='light'\n[[tabs]]\nname='Tools'\nitems=[{icon='brand.svg'}]\n");
    for (const auto& root : {executable_dir, user_dir}) { write(root / "brand.svg", "icon lookup fixture"); }
    write(executable_dir / "colors-light.toml", "[colors.salmon]\npartial=true\noutline='#112233'\n");
    write(user_dir / "colors-light.toml", "[colors.salmon]\npartial=true\noutline='#334455'\n");
    const launcher::FileLookup files(executable_dir / "Launcher.exe", user_dir, user_dir, true);
    REQUIRE(files.directories() == std::vector<fs::path>{executable_dir, user_dir});
    const auto loaded = launcher::load_configuration_files(config, files);
    REQUIRE(std::get<launcher::FileIcon>(loaded.configuration.tabs.front().items[0].icon_source).path == executable_dir / "brand.svg");
    REQUIRE(fs::path(loaded.color_sources.back()) == executable_dir / "colors-light.toml");
    REQUIRE(loaded.configuration.tabs.front().colors.outline == 0x112233);
}

TEST_CASE("Absolute icon files bypass the roots and unreadable first matches stop lookup", "[config][storage][icons][file-lookup]") {
    Fixture fixture;
    const auto config_dir = fixture.path("Settings");
    const auto executable_dir = fixture.path("Portable");
    fs::create_directory(config_dir);
    fs::create_directory(executable_dir);
    const auto config = config_dir / "rightside.toml";
    const auto exact = fixture.path("outside.svg");
    write(exact, "absolute icon fixture");
    const auto utf8 = exact.u8string();
    write(config, "[[tabs]]\nname='Tools'\nitems=[{icon='" + std::string(utf8.begin(), utf8.end()) + "'}]\n");
    const launcher::FileLookup files(executable_dir / "Launcher.exe", {}, config_dir, true);
    auto loaded = launcher::load_configuration_files(config, files);
    REQUIRE(std::get<launcher::FileIcon>(loaded.configuration.tabs.front().items[0].icon_source).path == exact);
    fs::remove(exact);
    loaded = launcher::load_configuration_files(config, files);
    REQUIRE(loaded.icon_sources.empty());
    REQUIRE(loaded.icon_warnings.size() == 1);
    REQUIRE(std::get<launcher::FileIcon>(loaded.configuration.tabs.front().items[0].icon_source).path == exact);
    REQUIRE(loaded.icon_warnings.front().find(std::string(utf8.begin(), utf8.end())) != std::string::npos);
    write(config, "[[tabs]]\nname='Tools'\nitems=[{icon='brand.svg'}]\n");
    const auto selected = config_dir / "brand.svg";
    write(selected, "first icon fixture");
    write(executable_dir / "brand.svg", "lower priority fixture");
    {
        FileLock lock(selected);
        REQUIRE(lock.valid());
        try {
            launcher::load_configuration_files(config, files);
            FAIL("Unreadable explicit icon must not fall through to another root");
        } catch (const launcher::ConfigError& error) {
            const auto path = selected.u8string();
            REQUIRE(std::string(error.what()).find(std::string(path.begin(), path.end())) != std::string::npos);
        }
    }
    fs::remove(selected);
    fs::create_directory(selected);
    REQUIRE_THROWS_AS(launcher::load_configuration_files(config, files), launcher::ConfigError);
}

TEST_CASE("Ordered selected color sets add then explicitly override names", "[colors][storage]") {
    Fixture fixture;
    const auto config = fixture.path("Launcher.toml");
    write(config, "colors=['custom','themed']\n[[tabs]]\nname='General'\ncolor='BeSpOkE'\n");
    write(fixture.path("colors-custom.toml"),
        "[colors.Bespoke]\ntab=[{at=0,color='#FFF8BB'},{at=1,color='#FFDD88'}]\n"
        "page=[{at=0,color='#FFDD88'},{at=1,color='#FFD66B'}]\noutline='#112233'\n");
    write(fixture.path("colors-themed.toml"), "[colors.bespoke]\npartial=true\noutline='#334455'\n");
    const auto loaded = launcher::load_configuration_files(config, fixture.directory(), fixture.directory());
    REQUIRE(loaded.configuration.color_sets == std::vector<std::string>{"custom", "themed"});
    REQUIRE(loaded.color_sources.size() == 4);
    REQUIRE(fs::path(loaded.color_sources[2]) == fixture.path("colors-custom.toml"));
    REQUIRE(fs::path(loaded.color_sources[3]) == fixture.path("colors-themed.toml"));
    REQUIRE(loaded.configuration.tabs.front().colors.outline == 0x334455);
    REQUIRE(loaded.configuration.tabs.front().colors.tab.gradient.back().rgb == 0xffdd88);
    write(config, "colors=['themed','custom']\n[[tabs]]\nname='General'\ncolor='bespoke'\n");
    REQUIRE_THROWS_AS(launcher::load_configuration_files(config, fixture.directory(), fixture.directory()), launcher::ConfigError);
}

TEST_CASE("Default color sets honor the same lookup hierarchy and plain colors TOML is ignored", "[colors][storage]") {
    Fixture fixture;
    const auto config = fixture.path("Launcher.toml");
    write(config, "[[tabs]]\nname='General'\ncolor='orange'\n");
    write(fixture.path("colors.toml"), "broken old-style colors file[");
    auto loaded = launcher::load_configuration_files(config, fixture.directory(), fixture.directory());
    REQUIRE(loaded.configuration.color_sets.empty());
    REQUIRE(loaded.color_sources.size() == 2);
    REQUIRE(loaded.configuration.tabs.front().colors.page.gradient.front().rgb == 0xef9843);
    write(fixture.path("basic_colors.toml"),
        "[colors.orange]\ntab=[{at=0,color='#FFF8BB'},{at=1,color='#FFDD88'}]\n"
        "page=[{at=0,color='#FFDD88'},{at=1,color='#FFD66B'}]\noutline='#112233'\n");
    loaded = launcher::load_configuration_files(config, fixture.directory(), fixture.directory());
    REQUIRE(fs::path(loaded.color_sources[0]) == fixture.path("basic_colors.toml"));
    REQUIRE(loaded.configuration.tabs.front().colors.outline == 0x112233);
    REQUIRE_FALSE(launcher::color_library.contains("salmon"));
    REQUIRE(launcher::color_library.contains("copper"));
    fs::remove(fixture.path("basic_colors.toml"));
    write(config, "colors=[]\n[[tabs]]\nname='General'\ncolor='salmon'\n");
    loaded = launcher::load_configuration_files(config, fixture.directory(), fixture.directory());
    REQUIRE(loaded.configuration.color_sets.empty());
    REQUIRE(loaded.color_sources.size() == 2);
    REQUIRE(launcher::color_library.contains("salmon"));
}

TEST_CASE("Selected set names do not alias the fixed basic and extra resource names", "[colors][storage][bundled]") {
    Fixture fixture;
    const auto config = fixture.path("Launcher.toml");
    write(config, "[[tabs]]\nname='General'\ncolor='orange'\n");
    const auto defaults = launcher::load_configuration_files(config, fixture.directory(), fixture.directory());
    REQUIRE(defaults.color_sources.size() == 2);
    REQUIRE(defaults.color_sources[0].ends_with("!resource:basic_colors.toml"));
    REQUIRE(defaults.color_sources[1].ends_with("!resource:extra_colors.toml"));
    REQUIRE(launcher::color_library.contains("orange"));
    REQUIRE(launcher::color_library.contains("copper"));
    for (const auto selection : {"'basic_colors'", "'extra_colors'", "['basic_colors']", "['extra_colors']",
            "['light','basic_colors']", "['extra_colors','dark']"}) {
        const auto source = std::string("colors=") + selection + "\n[[tabs]]\nname='General'\ncolor='orange'\n";
        INFO(source);
        REQUIRE_THROWS_AS(launcher::parse_configuration(source), launcher::ConfigError);
        write(config, source);
        REQUIRE_THROWS_AS(launcher::load_configuration_files(config, fixture.directory(), fixture.directory()), launcher::ConfigError);
    }
    // These remain ordinary selectable basenames when an actual prefixed file
    // exists; they never select the fixed resource of the same unqualified name.
    write(fixture.path("colors-basic_colors.toml"), "[colors.orange]\npartial=true\noutline='#112233'\n");
    write(config, "colors='basic_colors'\n[[tabs]]\nname='General'\ncolor='orange'\n");
    const auto selected = launcher::load_configuration_files(config, fixture.directory(), fixture.directory());
    REQUIRE(selected.color_sources.size() == 3);
    REQUIRE(fs::path(selected.color_sources.back()) == fixture.path("colors-basic_colors.toml"));
    REQUIRE(selected.configuration.tabs.front().colors.outline == 0x112233);
}
TEST_CASE("Color set selection rejects invalid names or list types", "[colors][storage]") {
    Fixture fixture;
    const auto config = fixture.path("Launcher.toml");
    for (const auto selection : {"''", "'../other'", "'x/y'", "'x\\y'", "'C:light'", "4",
            "\"x\\u0000y\"", "\"caf\\u00e9\"", "['light',4]", "['light',{}]", "[['light']]", "{}"}) {
        const auto invalid = std::string("colors=") + selection + "\n[[tabs]]\nname='General'\n";
        INFO(invalid);
        REQUIRE_THROWS_AS(launcher::parse_configuration(invalid), launcher::ConfigError);
        write(config, invalid);
        REQUIRE_THROWS_AS(launcher::load_configuration_files(config, fixture.directory(), fixture.directory()), launcher::ConfigError);
    }
}

TEST_CASE("Configuration file loaders ignore everything below STOP", "[config][storage][stop]") {
    Fixture fixture;
    const auto config = fixture.path("Launcher.toml");
    const std::string source = "colors='light'\n[[tabs]]\nname='Active'\n%STOP ===========examples below\n"
        "colors='nonexistent'\n[[tabs]]\nname='Example'\nThis isn't valid TOML [";
    write(config, source);
    write(fixture.path("colors-light.toml"), "[colors.salmon]\npartial=true\noutline='#112233'\n");
    REQUIRE(launcher::load_configuration(config).tabs.size() == 1);
    const auto loaded = launcher::load_configuration_files(config, fixture.directory(), fixture.directory());
    REQUIRE(loaded.configuration.tabs.size() == 1);
    REQUIRE(loaded.configuration.color_sets == std::vector<std::string>{"light"});
    REQUIRE(loaded.configuration.tabs.front().colors.outline == 0x112233);
    REQUIRE(fs::path(loaded.color_sources.back()) == fixture.path("colors-light.toml"));
    REQUIRE(read(config) == source);
}

TEST_CASE("Color and runtime TOML files share STOP preprocessing", "[colors][settings][storage][stop]") {
    Fixture fixture;
    const auto colors = fixture.path("colors-custom.toml");
    const std::string color_source = "[colors.salmon]\npartial=true\noutline='#112233'\n%STOP\nUnparsed palette examples [";
    write(colors, color_source);
    launcher::color_library.clear();
    launcher::load_colors("basic_colors.toml", fixture.directory(), fixture.directory());
    launcher::parse_colors(color_source);
    REQUIRE(launcher::color_library.at("salmon").outline == 0x112233);
    REQUIRE(launcher::load_colors("colors-custom.toml", fixture.directory(), fixture.directory()).ends_with("colors-custom.toml"));
    REQUIRE(launcher::color_library.at("salmon").outline == 0x112233);
    REQUIRE(read(colors) == color_source);

    const auto runtime = fixture.path("Launcher.runtime.toml");
    const std::string runtime_source = "[adjustments]\ntext_scale=1.5\n%STOP\nUnparsed runtime examples [";
    write(runtime, runtime_source);
    REQUIRE(launcher::load_runtime_adjustments(runtime).text_scale == 1.5);
    REQUIRE(read(runtime) == runtime_source);
}
TEST_CASE("Starter configuration keeps copyable item examples below STOP", "[config][stop][bundled]") {
    const auto path = fs::path(__FILE__).parent_path().parent_path() / "Launcher.toml";
    const auto source = read(path);
    const auto marker = source.find("%STOP ===========examples below");
    REQUIRE(marker != source.npos);
    const auto active = launcher::load_configuration(path);
    REQUIRE(active.tabs.size() == 3);
    REQUIRE(active.tabs[0].name == "Launcher");
    REQUIRE(active.tabs[1].name == "Windows");
    REQUIRE(active.tabs[2].name == "Examples");
    for (const auto& tab : active.tabs) {
        REQUIRE(tab.items.size() == 6);
        for (const auto& item : tab.items) {
            REQUIRE(item.kind == launcher::ItemKind::icon);
            REQUIRE(item.launch.has_value());
            REQUIRE_FALSE(item.launch->target.empty());
            REQUIRE_FALSE(item.label.empty());
        }
    }
    auto promoted = source;
    promoted.erase(marker, promoted.find('\n', marker) - marker);
    // Supply the optional macros referenced by examples without enabling them
    // in the starter's active configuration.
    promoted.insert(promoted.find("[dock]"), "[paths]\nTools='C:\\Tools'\nIcons='D:\\Launcher Icons'\n"
        "NotesArgument='\"D:\\Work Notes\\Checklist.txt\"'\n");
    const auto examples = launcher::parse_configuration(promoted);
    REQUIRE(examples.tabs.size() == active.tabs.size() + 1);
    REQUIRE(examples.tabs.back().items.size() >= 15);
}

TEST_CASE("Starter tools, HTML guides and quoted examples follow the shipped program directory", "[config][paths][bundled][starter]") {
    Fixture fixture;
    const auto repository = fs::path(__FILE__).parent_path().parent_path();
    const auto source = read(repository / "Launcher.toml");
    const auto text = [](const fs::path& path) {
        auto preferred = path;
        const auto utf8 = preferred.make_preferred().u8string();
        return std::string(utf8.begin(), utf8.end());
    };
    const std::vector<std::pair<fs::path, fs::path>> supplied_files = {
        {"examples/demo_many_colors.cmd", "examples/demo_many_colors.cmd"},
        {"examples/demo_many_colors.toml", "examples/demo_many_colors.toml"},
        {"examples/basic_colors.toml", "basic_colors.toml"},
    };
    const std::vector<std::string> windows_targets = {
        "notepad.exe", "calc.exe", "explorer.exe", "charmap.exe", "ms-settings:", "shell:RecycleBinFolder", "."
    };
    for (const auto layout : {"Portable ZIP", "Program Files/Launcher"}) {
        CAPTURE(layout);
        const auto program = fixture.path(layout).lexically_normal();
        const auto profile = fixture.path("Selected profile");
        fs::create_directories(program / "docs");
        fs::create_directories(program / "examples");
        fs::create_directories(profile);
        std::vector<std::string> guide_arguments;
        for (const auto& [destination, original] : supplied_files) {
            write(program / destination, read(repository / original));
            if (destination.extension() != ".cmd") { guide_arguments.push_back('"' + text(program / destination) + '"'); }
        }
        std::vector<std::string> html_targets;
        for (const auto name : {"Launcher", "Installer", "LauncherDrop", "SvgExtractor"}) {
            const auto path = program / "docs" / (std::string(name) + ".html");
            write(path, "<!doctype html><html><title>Guide path fixture</title></html>");
            html_targets.push_back(text(path));
        }
        auto program_targets = html_targets;
        const std::vector<std::string> other_targets = {
            text(program / "LauncherDrop.exe"), text(program / "LauncherSvgExtractor.exe"),
            text(program), text(program / "examples"), text(program / "examples/demo_many_colors.cmd")
        };
        program_targets.insert(program_targets.end(), other_targets.begin(), other_targets.end());
        write(program / "LauncherDrop.exe", "Unexecuted companion path fixture");
        write(program / "LauncherSvgExtractor.exe", "Unexecuted companion path fixture");
        const auto config = (std::string_view(layout) == "Portable ZIP" ? program : profile) / "Launcher.toml";
        write(config, source);
        const auto loaded = launcher::load_configuration_files(config, profile, program);
        std::size_t guide_count = 0;
        std::size_t html_count = 0;
        std::size_t demo_count = 0;
        for (const auto& tab : loaded.configuration.tabs) {
            for (const auto& item : tab.items) {
                REQUIRE(item.launch.has_value());
                const auto& action = *item.launch;
                if (!action.arguments.empty()) {
                    REQUIRE(std::ranges::find(guide_arguments, action.arguments) != guide_arguments.end());
                    if (action.target == text(program / "Launcher.exe")) {
                        REQUIRE(action.arguments == '"' + text(program / "examples/demo_many_colors.toml") + '"');
                        ++demo_count;
                    } else {
                        REQUIRE(action.target == "notepad.exe");
                        ++guide_count;
                    }
                } else {
                    REQUIRE((std::ranges::find(program_targets, action.target) != program_targets.end() ||
                        std::ranges::find(windows_targets, action.target) != windows_targets.end()));
                    if (std::ranges::find(html_targets, action.target) != html_targets.end()) {
                        ++html_count;
                    }
                }
            }
        }
        REQUIRE(guide_count == 2);
        REQUIRE(demo_count == 1);
        REQUIRE(html_count == 4);
        REQUIRE(read(config) == source);
    }
}
