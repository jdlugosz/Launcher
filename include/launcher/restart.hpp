#pragma once

#include "launcher/launch.hpp"

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>

namespace launcher {

struct RestartHandles final {
    std::uintptr_t parent = 0;
    std::uintptr_t ready = 0;
    std::uintptr_t cancel = 0;
    std::uintptr_t error = 0;
};

struct UpdateStartupHandles final {
    std::uintptr_t ready = 0;
    std::uintptr_t error = 0;
};

// Success means the replacement initialized while hidden and is waiting for
// this process to exit. Run off the UI thread; cancellation retains the parent.
// An optional cancellation handle transfers to the caller on success. Signal
// and close it to abandon a ready replacement, or just close it when committing.
// Arguments contain the application's synthesized persistent options, without
// argv[0] or private handoff switches. Their interpretation belongs to the caller.
LaunchResult restart_launcher(const std::filesystem::path& executable, std::wstring_view arguments,
    std::stop_token stop = {}, std::uintptr_t* cancellation_handle = nullptr);

// The child calls this after hidden initialization and before opening the shared
// log or showing its window. Both child helpers consume the inherited handles.
LaunchResult complete_restart_handoff(RestartHandles handles);
void report_restart_failure(RestartHandles handles, std::wstring_view message) noexcept;

// Post-update startup uses the same event and diagnostic pipe as Restart,
// without a previous process to wait for. Each helper consumes both handles.
LaunchResult complete_update_startup(UpdateStartupHandles handles);
void report_update_startup_failure(UpdateStartupHandles handles, bool settings_error, std::wstring_view message,
    std::wstring_view settings_file = {}, std::uint32_t line = 0, std::uint32_t column = 0) noexcept;

// Adds argv[0] and fresh synchronization handles to the supplied argument text.
std::wstring restart_command_line(const std::filesystem::path& executable, std::wstring_view arguments, RestartHandles handles);

} // namespace launcher
