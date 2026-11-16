#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#ifdef LAUNCHER_USE_JMD_MODULES
import jmd.about;
#endif
#include "launcher/seenthemall.hh"
#endif

#include "launcher/about.hpp"
#include "launcher_resource.h"

#include <cstddef>
#include <cstring>
#include <format>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace launcher {
namespace {

std::wstring query_text(const std::vector<std::byte>& bytes, std::wstring_view field)
{
    const auto query = L"\\StringFileInfo\\040904b0\\" + std::wstring(field);
    void* value = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(bytes.data(), query.c_str(), &value, &length) || !value || length == 0) { return {}; }
    const auto* text = static_cast<const wchar_t*>(value);
    std::size_t used = 0;
    while (used < length && text[used] != L'\0') { ++used; }
    return {text, used};
}

#if !defined(LAUNCHER_USE_JMD_MODULES) || defined(__INTELLISENSE__)
bool copy_details(HWND owner, const std::wstring& text)
{
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) { return false; }
    void* buffer = GlobalLock(memory);
    if (!buffer) { GlobalFree(memory); return false; }
    std::memcpy(buffer, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!OpenClipboard(owner)) { GlobalFree(memory); return false; }
    const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    CloseClipboard();
    if (!copied) { GlobalFree(memory); }
    return copied;
}

struct AboutContext {
    const BuildInfo& info;
    const std::function<void(HWND)>& initialized;
    const std::function<bool(HWND)>& check_now;
    std::wstring subtitle;
    std::wstring update_status;
    std::wstring details;
    HWND dialog = nullptr;
};

LRESULT CALLBACK about_owner(HWND owner, UINT message, WPARAM word, LPARAM parameter, UINT_PTR id, DWORD_PTR data)
{
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(owner, about_owner, id);
    }
    if (message == WM_SETCURSOR && LOWORD(parameter) == static_cast<WORD>(HTERROR)) {
        switch (HIWORD(parameter)) {
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_NCLBUTTONDOWN:
        case WM_NCRBUTTONDOWN:
        case WM_NCMBUTTONDOWN:
        case WM_NCXBUTTONDOWN: {
            // Windows routes clicks on a disabled modal owner here before its
            // default beep. Raise About or the prompt currently blocking it.
            const auto dialog = reinterpret_cast<const AboutContext*>(data)->dialog;
            if (dialog && IsWindowVisible(dialog)) {
                auto target = GetWindow(dialog, GW_ENABLEDPOPUP);
                if (!target || !IsWindowVisible(target)) { target = dialog; }
                if (!SetForegroundWindow(target)) {
                    SetWindowPos(target, HWND_TOP, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                }
                return TRUE;
            }
            break;
        }
        }
    }
    return DefSubclassProc(owner, message, word, parameter);
}

INT_PTR CALLBACK about_dialog(HWND dialog, UINT message, WPARAM word, LPARAM parameter)
{
    if (message == WM_INITDIALOG) {
        auto& context = *reinterpret_cast<AboutContext*>(parameter);
        context.dialog = dialog;
        SetWindowLongPtrW(dialog, DWLP_USER, parameter);
        const auto title = L"Launcher " + context.info.version;
        SetDlgItemTextW(dialog, IDC_ABOUT_VERSION, title.c_str());
        SetDlgItemTextW(dialog, IDC_ABOUT_STATUS, context.subtitle.c_str());
        SetDlgItemTextW(dialog, IDC_ABOUT_DETAILS, context.details.c_str());
        SetDlgItemTextW(dialog, IDC_ABOUT_UPDATE_STATUS, context.update_status.c_str());
        EnableWindow(GetDlgItem(dialog, IDC_ABOUT_CHECK_NOW), static_cast<bool>(context.check_now));
        // Center on the owner's monitor; a dock may sit at the edge of it.
        MONITORINFO monitor{sizeof(monitor)};
        RECT bounds{};
        if (GetWindowRect(dialog, &bounds) && GetMonitorInfoW(MonitorFromWindow(GetParent(dialog), MONITOR_DEFAULTTONEAREST), &monitor)) {
            SetWindowPos(dialog, nullptr, monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - (bounds.right - bounds.left)) / 2,
                monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - (bounds.bottom - bounds.top)) / 2,
                0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        if (context.initialized) { context.initialized(dialog); }
        SetFocus(GetDlgItem(dialog, IDOK));
        return FALSE;
    }
    if (message == WM_NCDESTROY) {
        if (auto* context = reinterpret_cast<AboutContext*>(GetWindowLongPtrW(dialog, DWLP_USER))) {
            context->dialog = nullptr;
        }
    }
    if (message == WM_COMMAND) {
        const auto command = LOWORD(word);
        if (command == IDOK || command == IDCANCEL) { EndDialog(dialog, command); return TRUE; }
        if (command == IDC_ABOUT_CHECK_NOW) {
            const auto* context = reinterpret_cast<const AboutContext*>(GetWindowLongPtrW(dialog, DWLP_USER));
            if (context && context->check_now && !context->check_now(dialog)) {
                MessageBoxW(dialog, L"Could not start an update check.", L"Launcher updates", MB_OK | MB_ICONINFORMATION);
            }
            return TRUE;
        }
        if (command == IDC_ABOUT_COPY) {
            const auto* context = reinterpret_cast<const AboutContext*>(GetWindowLongPtrW(dialog, DWLP_USER));
            if (context && !copy_details(dialog, L"Launcher " + context->info.version + L"\r\n" +
                    context->info.release_status + L"\r\n\r\n" + build_details(context->info, true))) {
                MessageBoxW(dialog, L"The clipboard is busy. Please try copying again.", L"Launcher", MB_OK | MB_ICONINFORMATION);
            }
            return TRUE;
        }
    }
    return FALSE;
}
#endif

} // namespace

BuildInfo read_build_info(const std::filesystem::path& executable)
{
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(executable.c_str(), &ignored);
    if (size == 0) { throw std::runtime_error("Cannot read the executable version resource."); }
    std::vector<std::byte> bytes(size);
    void* value = nullptr;
    UINT length = 0;
    if (!GetFileVersionInfoW(executable.c_str(), 0, size, bytes.data()) ||
            !VerQueryValueW(bytes.data(), L"\\", &value, &length) || !value || length < sizeof(VS_FIXEDFILEINFO)) {
        throw std::runtime_error("Cannot read the executable version resource.");
    }
    const auto& fixed = *std::start_lifetime_as<VS_FIXEDFILEINFO>(value);
    if (fixed.dwSignature != VS_FFI_SIGNATURE) { throw std::runtime_error("Invalid executable version resource."); }
    BuildInfo info;
    info.version = query_text(bytes, L"ProductVersion");
    if (info.version.empty()) {
        info.version = std::format(L"{}.{}.{}.{}", HIWORD(fixed.dwProductVersionMS), LOWORD(fixed.dwProductVersionMS),
            HIWORD(fixed.dwProductVersionLS), LOWORD(fixed.dwProductVersionLS));
    }
    info.file_version = std::format(L"{}.{}.{}.{}", HIWORD(fixed.dwFileVersionMS), LOWORD(fixed.dwFileVersionMS),
        HIWORD(fixed.dwFileVersionLS), LOWORD(fixed.dwFileVersionLS));
    info.built = query_text(bytes, L"BuildDateTime");
    info.configuration = query_text(bytes, L"BuildConfiguration");
    info.commit = query_text(bytes, L"CommitId");
    info.git_version = query_text(bytes, L"GitVersion");
    info.source_state = query_text(bytes, L"SourceState");
    info.release_status = query_text(bytes, L"ReleaseStatus");
    info.instrumentation = query_text(bytes, L"SpecialBuild");
    const auto flags = fixed.dwFileFlags & fixed.dwFileFlagsMask;
    info.debug = (flags & VS_FF_DEBUG) != 0;
    info.prerelease = (flags & VS_FF_PRERELEASE) != 0;
    info.special = (flags & VS_FF_SPECIALBUILD) != 0;
    return info;
}

const BuildInfo& current_build_info()
{
    static const auto info = [] {
        std::wstring path(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size()) { throw std::runtime_error("Cannot locate the running executable."); }
        path.resize(length);
        return read_build_info(path);
    }();
    return info;
}

std::wstring build_details(const BuildInfo& info, bool include_full_commit)
{
    auto configuration = info.configuration.empty() ? L"Unknown" : info.configuration;
    if (info.debug && configuration.find(L"Debug") == std::wstring::npos) { configuration += L" (Debug)"; }
    auto details = L"Build: " + configuration + L" · x64\r\n";
    details += L"File version: " + info.file_version + L"\r\n";
    if (info.special || !info.instrumentation.empty()) {
        details += L"Instrumentation: " + (info.instrumentation.empty() ? L"Special build" : info.instrumentation) + L"\r\n";
    }
    details += L"Built: " + (info.built.empty() ? L"Unavailable" : info.built) + L"\r\n";
    details += L"Git: " + (info.git_version.empty() ? L"Unavailable" : info.git_version) + L"\r\n";
    if (include_full_commit) {
        details += L"Commit: " + (info.commit.empty() ? L"Unavailable" : info.commit) + L"\r\n";
    }
    details += L"Source: " + (info.source_state.empty() ? L"Unavailable" : info.source_state);
    return details;
}

INT_PTR show_about(HWND owner, const BuildInfo& info, const std::function<void(HWND)>& initialized,
    const std::function<bool(HWND)>& check_now, std::wstring_view update_status, std::wstring_view installation_status)
{
    auto subtitle = info.release_status;
    if (!installation_status.empty()) {
        if (!subtitle.empty()) { subtitle += L"\r\n"; }
        subtitle += installation_status;
    }
#if defined(LAUNCHER_USE_JMD_MODULES) && !defined(__INTELLISENSE__)
    const jmd::AboutTemplate layout{
        .resource = MAKEINTRESOURCEW(IDD_ABOUT),
        .heading = IDC_ABOUT_VERSION,
        .subtitle = IDC_ABOUT_STATUS,
        .details = IDC_ABOUT_DETAILS,
        .update_status = IDC_ABOUT_UPDATE_STATUS,
        .copy = IDC_ABOUT_COPY,
        .check_now = IDC_ABOUT_CHECK_NOW,
        .center_on_owner_monitor = true,
    };
    jmd::AboutContent content;
    content.product_name = L"Launcher";
    content.heading = content.product_name + L" " + info.version;
    content.subtitle = std::move(subtitle);
    content.details = build_details(info);
    content.update_status = update_status;
    if (initialized) { content.initialized = std::cref(initialized); }
    if (check_now) { content.check_now = std::cref(check_now); }
    content.copy_text = [&] { return content.heading + L"\r\n" + info.release_status + L"\r\n\r\n" + build_details(info, true); };
    return jmd::show_about(owner, GetModuleHandleW(nullptr), layout, content);
#else
    AboutContext context{info, initialized, check_now, std::move(subtitle), std::wstring(update_status), build_details(info)};
    // An owned About dialog inherits the dock's topmost state. Let other apps
    // and Windows security prompts appear above it while the dialog is open.
    constexpr UINT topmost_flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    const bool restore_topmost = (GetWindowLongPtrW(owner, GWL_EXSTYLE) & WS_EX_TOPMOST) &&
        SetWindowPos(owner, HWND_NOTOPMOST, 0, 0, 0, 0, topmost_flags);
    const auto subclass_id = reinterpret_cast<UINT_PTR>(&context);
    const bool subclassed = owner && SetWindowSubclass(owner, about_owner, subclass_id, reinterpret_cast<DWORD_PTR>(&context));
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ABOUT), owner, about_dialog,
        reinterpret_cast<LPARAM>(&context));
    const auto error = result == -1 ? GetLastError() : ERROR_SUCCESS;
    if (subclassed && IsWindow(owner)) { RemoveWindowSubclass(owner, about_owner, subclass_id); }
    if (restore_topmost && IsWindow(owner)) {
        SetWindowPos(owner, HWND_TOPMOST, 0, 0, 0, 0, topmost_flags);
    }
    if (result == -1) { SetLastError(error); }
    return result;
#endif
}

} // namespace launcher
