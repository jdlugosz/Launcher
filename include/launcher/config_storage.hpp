#pragma once

#include "launcher/config.hpp"
#include "launcher/file_lookup.hpp"

#include <filesystem>
#include <functional>

namespace launcher {
class FileLookup;

// Returns user_directory/Launcher.toml. Existing files are never overwritten or
// validated here; their normal load errors must reach the caller unchanged.
// A newly seeded bundled default is validated before atomic publication.
// Throws ConfigError for storage or bundled-default validation failures.
std::filesystem::path ensure_user_configuration(
    const std::filesystem::path& user_directory,
    const std::filesystem::path& bundled_default_path, const FileLookup& files = FileLookup({}, {}));

// Explicit files must exist. A missing implicit portable default is seeded
// beside the executable. Installed first use requires the supplied UI choice;
// declining returns an empty path; omitting the callback reports the missing
// configuration. Both leave the profile untouched.
std::filesystem::path resolve_configuration_path(const std::filesystem::path& argument, const FileLookup& files,
    const std::function<bool(const std::filesystem::path&)>& offer_creation = {});

// Portable demos use the exact bundled file. Installed demos seed a separate
// Demos/demo_many_colors.toml in the user's directory, preserving existing
// edits and sibling runtime settings. The caller identifies the bundled source.
std::filesystem::path resolve_color_demo_configuration(const std::filesystem::path& bundled_demo_path, const FileLookup& files);

} // namespace launcher
