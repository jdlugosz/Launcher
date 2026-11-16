#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/about.hpp"
#include "launcher_resource.h"
#include "launcher_version.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::filesystem::path application()
{
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path).parent_path() / L"Launcher.exe";
}

std::wstring control_text(HWND dialog, int control)
{
    std::array<wchar_t, 2048> text{};
    const auto length = GetDlgItemTextW(dialog, control, text.data(), static_cast<int>(text.size()));
    return {text.data(), static_cast<std::size_t>(length)};
}

bool is_above(HWND upper, HWND lower)
{
    for (HWND window = GetWindow(lower, GW_HWNDPREV); window; window = GetWindow(window, GW_HWNDPREV)) {
        if (window == upper) { return true; }
    }
    return false;
}

} // namespace

TEST_CASE("Executable resource identifies this development build")
{
    const auto info = launcher::read_build_info(application());
    CHECK(info.version == LAUNCHER_PRODUCT_VERSION_WSTRING);
    constexpr std::array file_version{LAUNCHER_VERSION_RESOURCE};
    CHECK(info.file_version == std::format(L"{}.{}.{}.{}", file_version[0], file_version[1], file_version[2], file_version[3]));
    CHECK(info.prerelease == static_cast<bool>(LAUNCHER_PRERELEASE));
    CHECK_FALSE(info.release_status.empty());
    CHECK(info.configuration == LAUNCHER_TEST_CONFIGURATION);
#ifdef _DEBUG
    CHECK(info.debug);
#else
    CHECK_FALSE(info.debug);
#endif
    CHECK(info.special == !info.instrumentation.empty());
    REQUIRE(info.built.size() == 23);
    CHECK(info.built.substr(19) == L" UTC");
    CHECK_FALSE(info.git_version.empty());
    if (info.source_state == L"Unavailable") {
        CHECK(info.commit == L"Unavailable (Git metadata absent)");
    } else {
        REQUIRE(info.commit.size() == 40);
        CHECK(std::ranges::all_of(info.commit, [](wchar_t character) {
            return (character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f');
        }));
        CHECK((info.source_state == L"Clean" || info.source_state == L"Modified working tree"));
        CHECK(info.git_version.find(info.commit.substr(0, 8)) != std::wstring::npos);
        const bool marked_modified = info.git_version.ends_with(L'+') || info.git_version.ends_with(L"+)");
        CHECK(marked_modified == (info.source_state == L"Modified working tree"));
    }
    CHECK_THROWS(launcher::read_build_info(application().parent_path() / L"absent-executable.exe"));
}

TEST_CASE("About details distinguish debug and special instrumentation")
{
    auto info = launcher::read_build_info(application());
    info.configuration = L"Debug";
    info.debug = true;
    info.special = true;
    info.instrumentation = L"Code coverage, AddressSanitizer";
    info.source_state = L"Modified working tree";
    const auto details = launcher::build_details(info);
    CHECK(details.find(L"Build: Debug") != std::wstring::npos);
    CHECK(details.find(L"File version: " + info.file_version) != std::wstring::npos);
    CHECK(details.find(L"Instrumentation: Code coverage, AddressSanitizer") != std::wstring::npos);
    CHECK(details.find(L"Git: " + info.git_version) != std::wstring::npos);
    CHECK(details.find(L"Commit: ") == std::wstring::npos);
    CHECK(launcher::build_details(info, true).find(L"Commit: " + info.commit) != std::wstring::npos);
    CHECK(details.find(L"Modified working tree") != std::wstring::npos);
}

TEST_CASE("Native About dialog displays resource details and closes")
{
    constexpr std::wstring_view update_status = L"Last checked for updates: 4 days ago";
    constexpr std::wstring_view installation_status = L"Portable";
    const auto info = launcher::read_build_info(application());
    const HWND owner = CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"About test owner", WS_OVERLAPPED,
        0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    REQUIRE(owner != nullptr);
    bool initialized = false;
    bool content_matches = false;
    bool owner_disabled = false;
    bool topmost_suspended = false;
    bool check_available = false;
    HWND about_dialog = nullptr;
    HWND check_owner = nullptr;
    int checks = 0;
    const auto result = launcher::show_about(owner, info, [&](HWND dialog) {
        initialized = true;
        about_dialog = dialog;
        owner_disabled = !IsWindowEnabled(owner);
        topmost_suspended = ((GetWindowLongPtrW(owner, GWL_EXSTYLE) | GetWindowLongPtrW(dialog, GWL_EXSTYLE)) & WS_EX_TOPMOST) == 0;
        content_matches = control_text(dialog, IDC_ABOUT_VERSION) == L"Launcher " + info.version &&
            control_text(dialog, IDC_ABOUT_STATUS) == info.release_status + L"\r\nPortable" &&
            control_text(dialog, IDC_ABOUT_UPDATE_STATUS) == update_status &&
            control_text(dialog, IDC_ABOUT_DETAILS) == launcher::build_details(info) &&
            (GetWindowLongW(GetDlgItem(dialog, IDC_ABOUT_DETAILS), GWL_STYLE) & ES_READONLY) != 0;
        const auto check_button = GetDlgItem(dialog, IDC_ABOUT_CHECK_NOW);
        check_available = check_button && IsWindowEnabled(check_button) &&
            control_text(dialog, IDC_ABOUT_CHECK_NOW) == L"Check now";
        if (check_available) { PostMessageW(check_button, BM_CLICK, 0, 0); }
        // The optional hold supports a visual review of this same real dialog.
        const UINT delay = GetEnvironmentVariableW(L"LAUNCHER_ABOUT_VISUAL_REVIEW", nullptr, 0) ? 5000 : 1;
        SetTimer(dialog, 1, delay, [](HWND window, UINT, UINT_PTR timer, DWORD) {
            KillTimer(window, timer);
            PostMessageW(window, WM_COMMAND, IDOK, 0);
        });
    }, [&](HWND dialog) {
        ++checks;
        check_owner = dialog;
        return true;
    }, update_status, installation_status);
    CHECK(IsWindowEnabled(owner));
    CHECK((GetWindowLongPtrW(owner, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
    DestroyWindow(owner);
    CHECK(initialized);
    CHECK(owner_disabled);
    CHECK(topmost_suspended);
    CHECK(content_matches);
    CHECK(check_available);
    CHECK(check_owner == about_dialog);
    CHECK(checks == 1);
    CHECK(result == IDOK);
}

TEST_CASE("Clicking the disabled dock raises its About dialog without making it topmost")
{
    const auto info = launcher::read_build_info(application());
    const auto desktop_name = std::format(L"Launcher-About-QA-{}", GetCurrentProcessId());
    const HDESK desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    REQUIRE(desktop);

    struct Probe {
        HWND owner = nullptr;
        bool occluded = false;
        bool hover_kept_order = false;
        bool click_handled = false;
        bool raised = false;
        bool stayed_modal = false;
        bool stayed_non_topmost = false;
        bool owner_restored = false;
    } probe;
    INT_PTR result = -1;
    std::jthread worker([&] {
        if (!SetThreadDesktop(desktop)) { return; }
        const auto module = GetModuleHandleW(nullptr);
        probe.owner = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"STATIC", L"About test dock", WS_POPUP | WS_VISIBLE,
            20, 20, 100, 100, nullptr, nullptr, module, nullptr);
        if (!probe.owner) { return; }
        result = launcher::show_about(probe.owner, info, [&](HWND dialog) {
            // Wait for DialogBox to show the actual resource dialog before covering it.
            if (!SetPropW(dialog, L"Launcher.AboutClickTest", &probe) ||
                !SetTimer(dialog, 1, 1, [](HWND window, UINT, UINT_PTR timer, DWORD) {
                    KillTimer(window, timer);
                    auto& state = *static_cast<Probe*>(RemovePropW(window, L"Launcher.AboutClickTest"));
                    RECT bounds{};
                    const HWND occluder = GetWindowRect(window, &bounds) ?
                        CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"About test occluder", WS_POPUP,
                            bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
                    if (occluder) {
                        SetWindowPos(occluder, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                        state.occluded = IsWindowVisible(window) && IsWindowVisible(occluder) && is_above(occluder, window);
                        const auto owner = reinterpret_cast<WPARAM>(state.owner);
                        SendMessageW(state.owner, WM_SETCURSOR, owner, MAKELPARAM(HTERROR, WM_MOUSEMOVE));
                        state.hover_kept_order = is_above(occluder, window);
                        state.click_handled = SendMessageW(state.owner, WM_SETCURSOR, owner, MAKELPARAM(HTERROR, WM_LBUTTONDOWN)) == TRUE;
                        state.raised = is_above(window, occluder);
                        state.stayed_modal = !IsWindowEnabled(state.owner) && GetWindow(window, GW_OWNER) == state.owner;
                        state.stayed_non_topmost =
                            ((GetWindowLongPtrW(state.owner, GWL_EXSTYLE) | GetWindowLongPtrW(window, GWL_EXSTYLE)) & WS_EX_TOPMOST) == 0;
                        DestroyWindow(occluder);
                    }
                    PostMessageW(window, WM_COMMAND, IDOK, 0);
                })) {
                PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
            }
        });
        probe.owner_restored = IsWindowEnabled(probe.owner) && (GetWindowLongPtrW(probe.owner, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        DestroyWindow(probe.owner);
    });
    worker.join();
    CHECK(CloseDesktop(desktop));
    CHECK(probe.occluded);
    CHECK(probe.hover_kept_order);
    CHECK(probe.click_handled);
    CHECK(probe.raised);
    CHECK(probe.stayed_modal);
    CHECK(probe.stayed_non_topmost);
    CHECK(probe.owner_restored);
    CHECK(result == IDOK);
}
