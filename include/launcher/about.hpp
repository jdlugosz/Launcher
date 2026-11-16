#pragma once

#include <windows.h>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace launcher {

struct BuildInfo {
    std::wstring version;
    std::wstring file_version;
    std::wstring built;
    std::wstring configuration;
    std::wstring commit;
    std::wstring git_version;
    std::wstring source_state;
    std::wstring release_status;
    std::wstring instrumentation;
    bool debug = false;
    bool prerelease = false;
    bool special = false;
};

BuildInfo read_build_info(const std::filesystem::path& executable);
const BuildInfo& current_build_info();
std::wstring build_details(const BuildInfo& info, bool include_full_commit = false);
INT_PTR show_about(HWND owner, const BuildInfo& info, const std::function<void(HWND)>& initialized = {},
    const std::function<bool(HWND)>& check_now = {}, std::wstring_view update_status = L"Last checked for updates: never",
    std::wstring_view installation_status = {});

} // namespace launcher
