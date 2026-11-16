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

#include "launcher/config_storage.hpp"
#include "launcher/config.hpp"
#include "launcher/file_lookup.hpp"
#include "launcher/resource_module.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace launcher {
namespace {

namespace fs = std::filesystem;

std::string path_text(const fs::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

[[noreturn]] void storage_error(std::string_view action,
                                const fs::path& path,
                                const std::error_code& error) {
    throw ConfigError(std::string(action) + " '" + path_text(path) + "': " +
                      error.message() + " (" + std::to_string(error.value()) + ")");
}

fs::path create_staging_directory(const fs::path& user_directory) {
    static std::atomic<std::uint64_t> sequence = 0;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        const auto name = ".launcher-seed-" + std::to_string(GetCurrentProcessId()) + "-" +
                          std::to_string(GetTickCount64()) + "-" +
                          std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
        const auto path = user_directory / name;
        std::error_code error;
        if (fs::create_directory(path, error)) {
            return path;
        }
        if (error && error != std::errc::file_exists) {
            storage_error("Cannot create configuration staging directory", path, error);
        }
    }
    throw ConfigError("Cannot allocate a unique configuration staging directory in '" +
                      path_text(user_directory) + "'");
}

class OwnedStagingDirectory {
public:
    explicit OwnedStagingDirectory(fs::path directory) : directory_(std::move(directory)) {}
    OwnedStagingDirectory(const OwnedStagingDirectory&) = delete;
    OwnedStagingDirectory& operator=(const OwnedStagingDirectory&) = delete;

    ~OwnedStagingDirectory() {
        // This directory was exclusively created by this invocation. Remove
        // only its known staging file, never the published user configuration.
        std::error_code ignored;
        fs::remove(directory_ / "Launcher.toml", ignored);
        fs::remove(directory_, ignored);
    }

    fs::path configuration_path() const { return directory_ / "Launcher.toml"; }

private:
    fs::path directory_;
};

} // namespace

namespace {
fs::path seed_file(const fs::path& configuration_path, const std::function<std::string()>& contents,
    const FileLookup& lookup, bool configuration = true, const std::function<void()>& before_publish = {}) {
    if (lookup.find(configuration_path, FileFallback::exact_file)) {
        return configuration_path;
    }
    const auto user_directory = configuration_path.parent_path();
    std::error_code error;
    fs::create_directories(user_directory, error);
    if (error) {
        storage_error("Cannot create user configuration directory", user_directory, error);
    }
    if (!fs::is_directory(user_directory, error)) {
        if (error) {
            storage_error("Cannot access user configuration directory", user_directory, error);
        }
        throw ConfigError("User configuration directory '" + path_text(user_directory) +
                          "' must be a directory");
    }

    // Another first launch may have finished while the directory was created.
    if (lookup.find(configuration_path, FileFallback::exact_file)) {
        return configuration_path;
    }

    const OwnedStagingDirectory staging(create_staging_directory(user_directory));
    const auto staging_path = staging.configuration_path();
    // A new ordinary file is editable even if the source template is read-only.
    // Neither the bundled source nor an existing configuration is modified.
    lookup.write_text(staging_path, contents());
    if (before_publish) { before_publish(); }
    if (configuration) {
        const auto loaded = load_configuration_files(staging_path, lookup);
        (void)layout_configuration(loaded.configuration);
    }

    // Staging beneath the destination keeps publication on the same volume.
    // No replacement is allowed. A concurrent winner becomes the user's
    // configuration without being read.
    if (MoveFileExW(staging_path.c_str(), configuration_path.c_str(), 0)) {
        return configuration_path;
    }
    const std::error_code publish_error(static_cast<int>(GetLastError()), std::system_category());
    if (lookup.find(configuration_path, FileFallback::exact_file)) {
        return configuration_path;
    }
    storage_error("Cannot publish user configuration", configuration_path, publish_error);
}

std::string starter_resource(const char* name) {
    ResourceModule module(nullptr);
    return std::string(module.FetchText(std::uint16_t{10}, std::string(name)));
}
} // namespace

fs::path ensure_user_configuration(const fs::path& user_directory,
    const fs::path& bundled_default_path, const FileLookup& lookup) {
    if (user_directory.empty()) { throw ConfigError("User configuration directory must not be empty"); }
    const auto destination = user_directory / L"Launcher.toml";
    if (lookup.find(destination, FileFallback::exact_file)) { return destination; }
    auto template_path = bundled_default_path;
    if (!lookup.portable() && bundled_default_path.filename() == L"Launcher.default.toml") {
        const auto installed = bundled_default_path.parent_path() / L"Launcher.installed.default.toml";
        if (lookup.find(installed, FileFallback::exact_file)) { template_path = installed; }
    }
    const auto files = lookup.executable().empty() ? FileLookup(bundled_default_path.parent_path() / L"Launcher.exe", user_directory) : lookup;
    return seed_file(destination, [&] { return files.read_text(template_path, FileFallback::exact_file).text; }, files, true, [&] {
        if (template_path != bundled_default_path) {
            (void)resolve_color_demo_configuration(bundled_default_path.parent_path() / L"examples" / L"demo_many_colors.toml", files);
        }
    });
}

std::filesystem::path resolve_configuration_path(const fs::path& argument, const FileLookup& files,
    const std::function<bool(const fs::path&)>& offer_creation) {
    if (files.explicitly_named() && argument.empty()) { throw ConfigError("An explicitly named configuration must not be empty"); }
    if (!argument.empty()) { return files.require(argument); }
    if (const auto found = files.find(L"Launcher.toml")) { return *found; }
    const auto directory = files.portable() ? files.executable().parent_path() : files.user_directory();
    if (directory.empty()) { throw ConfigError("No directory is available for the default configuration"); }
    if (!files.portable() && !offer_creation) {
        throw ConfigError("Launcher configuration was not created; choose an existing file with --config or create Launcher.toml in the Launcher settings folder");
    }
    if (!files.portable() && !offer_creation(directory / L"Launcher.toml")) { return {}; }
    return ensure_user_configuration(directory, files.executable().parent_path() / L"Launcher.default.toml", files);
}

std::filesystem::path resolve_color_demo_configuration(const fs::path& bundled_demo_path, const FileLookup& files) {
    if (files.portable()) { return files.require(bundled_demo_path, FileFallback::exact_file); }
    if (files.user_directory().empty()) { throw ConfigError("No user directory is available for the color demo"); }
    const auto user = files.user_directory();
    const auto destination = user / L"Demos" / L"demo_many_colors.toml";
    const auto legacy = user / L"Demos" / L"ManyColors" / L"Launcher.toml";
    const bool migrating = !files.find(destination, FileFallback::exact_file) && files.find(legacy, FileFallback::exact_file).has_value();
    const auto result = seed_file(destination, [&] {
        return migrating ? files.read_text(legacy, FileFallback::exact_file).text : starter_resource("DEMO_CONFIGURATION");
    }, files, !migrating, [&] {
        if (migrating) {
            const auto runtime = files.runtime_path(legacy);
            if (files.find(runtime, FileFallback::exact_file)) {
                (void)seed_file(files.runtime_path(destination), [&] { return files.read_text(runtime, FileFallback::exact_file).text; }, files, false);
            }
        }
    });
    (void)seed_file(user / L"Icons" / L"many-colors.svg", [] { return starter_resource("DEMO_ICON"); }, files, false);
    return result;
}

} // namespace launcher
