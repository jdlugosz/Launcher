#pragma once

#include "launcher/config_storage.hpp"
#include "launcher/file_lookup.hpp"

#include <filesystem>
#include <system_error>

namespace launcher::drop {

// The helper uses the same roots, mode detection and first-use policy as Launcher.
template<typename UserDirectory>
std::filesystem::path default_configuration(const std::filesystem::path& executable_directory, UserDirectory user_directory,
    const std::filesystem::path& argument = {}, const std::function<bool(const std::filesystem::path&)>& offer_creation = {}) {
    const FileLookup files(executable_directory / L"LauncherDrop.exe", user_directory(), {}, !argument.empty());
    return resolve_configuration_path(argument, files, offer_creation);
}

} // namespace launcher::drop
