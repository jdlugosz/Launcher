#pragma once

#include "launcher/config.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace launcher {

struct LaunchResult {
    bool success = false;
    std::uint32_t error_code = 0;
    std::wstring message;
};

// Resolution is separate from dispatch so all item actions use the same target.
// An empty working_directory preserves a shortcut's own "Start in" setting.
struct NativeLaunchAction {
    std::wstring target;
    std::wstring arguments;
    std::wstring working_directory;
    LaunchShow show = LaunchShow::normal;
    bool shell_namespace = false;
};

struct PreparedLaunch {
    std::optional<NativeLaunchAction> action;
    LaunchResult result;
};

// Shell filesystem targets containing Win32 wildcards become an absolute
// version pattern beside the configuration. Invalid targets return nullopt.
std::optional<std::filesystem::path> version_target_pattern(const LaunchAction& action, const std::filesystem::path& configuration_directory);

// The action already contains its final configuration text. Preparation resolves
// filesystem locations and executable lookup without further substitutions.
PreparedLaunch prepare_launch_action(const LaunchAction& action,
    const std::filesystem::path& configuration_directory);

// Pure lexical preparation for an already observed catalog file. The caller
// supplies a concrete absolute Shell target with its environment expanded and
// an absolute configuration directory. No existence, version, registry, Shell
// or COM queries occur; arguments/show and shortcut Start in rules are retained.
PreparedLaunch prepare_exact_target_action(const LaunchAction& action,
    const std::filesystem::path& configuration_directory);

// Reads a resolved EXE's FileDescription without loading its image. Other files
// and unavailable metadata use the filename; virtual/protocol targets stay literal.
std::wstring launch_display_label(const NativeLaunchAction& action);

// Uses the prepared, cached filesystem target without another executable scan.
bool can_run_as_administrator(const NativeLaunchAction& action) noexcept;

// Selects an existing filesystem target in its parent, preserving a shortcut
// itself. Virtual folders open their container. Call on a COM STA worker.
LaunchResult reveal_launch_target(const NativeLaunchAction& action,
    std::uintptr_t owner_window = 0);

// Shows Properties for the exact filesystem item, including a shortcut itself.
// Call on a worker: provider parsing and property handlers may block. Configured
// arguments, working directory and show mode are not passed to the handler;
// an unavailable Properties verb never falls back to opening the program.
LaunchResult show_target_properties(const NativeLaunchAction& action,
    std::uintptr_t owner_window = 0);

// Starts activation and returns without waiting for the launched program to
// exit. The caller reports failures; Windows retains security/elevation prompts.
// Dispatch an existing snapshot without another path or registry search.
LaunchResult launch_prepared_action(const NativeLaunchAction& action,
    std::uintptr_t owner_window = 0, std::string_view verb = {});

// Convenience entry point for callers that do not already own a snapshot.
LaunchResult launch_action(const LaunchAction& action,
    const std::filesystem::path& configuration_directory,
    std::uintptr_t owner_window = 0, std::string_view verb = {});

} // namespace launcher
