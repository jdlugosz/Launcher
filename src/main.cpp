#include "launcher/windows_pch.hpp"
#include <lmcons.h>
#include <mmsystem.h>

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/config.hpp"
#include "launcher/hover.hpp"
#include "launcher/config_storage.hpp"
#include "launcher/file_lookup.hpp"
#include "launcher/adjustments_dialog.hpp"
#include "launcher/about.hpp"
#include "about_review_status.hpp"
#include "default_drawer.hpp"
#include "updater_integration.hpp"
#include "launcher/desktop_icons.hpp"
#include "launcher/filesystem_error_log.hpp"
#include "launcher/filesystem_error_notice.hpp"
#include "launcher/app_notification.hpp"
#include "launcher/log.hpp"

#ifndef LAUNCHER_VERSION_TEST_HOOKS
#define LAUNCHER_VERSION_TEST_HOOKS 0
#endif
#include "launcher/icon_images.hpp"
#include "launcher/menu_icons.hpp"
#include "launcher/item_presentation.hpp"
#include "launcher/item_version_catalog.hpp"
#include "launcher/pending_overlay.hpp"
#include "launcher/popup_menu.hpp"
#include "launcher/svg_images.hpp"
#include "launcher/png.hpp"
#include "launcher/dock.hpp"
#include "launcher/launch.hpp"
#include "launcher/adobe_cc.hpp"
#include "launcher/version_discovery.hpp"
#include "launcher/version_discovery_runtime.hpp"
#include "launcher/restart.hpp"
#include "win32/window_text_reply.hpp"
#include "launcher/settings.hpp"
#include "launcher/shell_context_menu.hpp"
#if LAUNCHER_VERSION_TEST_HOOKS
#include "launcher/version_scenarios.hpp"
#endif
#include "launcher_version.h"
#include "launcher_resource.h"
#include "adjustments_resource.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
using launcher::Log;
constexpr wchar_t kWindowClass[] = L"Launcher.DrawingPrototype.Window";
constexpr UINT kQuitCommand = 1;
constexpr UINT kEditConfigurationCommand = 2;
constexpr UINT kConfigurationFolderCommand = 3;
constexpr UINT kAdjustmentsCommand = 4;
constexpr UINT kRestartCommand = 5;
constexpr UINT kHideCommand = 6;
constexpr UINT kAboutCommand = 7;
constexpr UINT kSnapshotCommand = 8;
constexpr int kSnapshotHotkey = 1;
constexpr UINT kIconOpenCommand = 10;
constexpr UINT kIconAdministratorCommand = 11;
constexpr UINT kIconRevealCommand = 12;
constexpr UINT kIconLatestCommand = 13;
constexpr UINT kIconAlternateCommand = 14;
constexpr UINT kIconConfiguredFirst = 100;
constexpr UINT kIconVersionFirst = 0x800;
constexpr UINT kIconShellFirst = 0x3000;
constexpr UINT kIconShellLast = 0x7fff;
constexpr UINT kVersionOpenCommand = 0x9000;
constexpr UINT kVersionAdministratorCommand = 0x9001;
constexpr UINT kVersionRevealCommand = 0x9002;
constexpr UINT kVersionPropertiesCommand = 0x9003;
constexpr UINT kVersionSetDefaultCommand = 0x9004;
constexpr UINT kVersionConfiguredFirst = 0x9100;
constexpr UINT kVersionShellFirst = 0xa000;
constexpr UINT kVersionShellLast = 0xefff;
constexpr wchar_t kInstalledVersionsHeading[] = L"Installed Versions";
constexpr UINT kFullscreenMessage = WM_APP + 1;
constexpr UINT kReconfigureMessage = WM_APP + 2;
constexpr UINT kDesktopSwitchMessage = WM_APP + 4;
constexpr UINT kUpdaterRetryChangedMessage = WM_APP + 5;
constexpr UINT kUpdateStartupReadyMessage = WM_APP + 6;
constexpr UINT_PTR kDiagnosticTimer = 50;
constexpr UINT_PTR kGapPointerTimer = 51;
constexpr UINT_PTR kRestartCompletionTimer = 54;
constexpr UINT_PTR kMotionTimer = 55;
constexpr UINT_PTR kUpdaterTimer = 56;
constexpr std::uint64_t kLaunchRevealGraceMs = 2000;
constexpr std::array<DWORD, 5> kFullscreenObjectEvents{
    EVENT_OBJECT_DESTROY, EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE,
    EVENT_OBJECT_REORDER, EVENT_OBJECT_LOCATIONCHANGE};
constexpr float kTabVerticalPaddingEm = 0.2F;
constexpr float kTabExtraBaselinePaddingEm = 0.2F;
constexpr float kTabActiveRiseEm = 0.15F;

// The live prism uses the same premultiplied WIC drawing as PNG export. Keep
// its transfer bitmap between paints, including the soft exterior shadows.
class LayeredFrame final {
public:
    LayeredFrame() = default;
    LayeredFrame(const LayeredFrame&) = delete;
    LayeredFrame& operator=(const LayeredFrame&) = delete;
    ~LayeredFrame() { reset(); }

    HRESULT present(HWND window, IWICBitmap* image, const WICRect* crop = nullptr)
    {
        UINT width = 0;
        UINT height = 0;
        HRESULT result = image->GetSize(&width, &height);
        if (FAILED(result)) { return result; }
        if (crop) {
            width = static_cast<UINT>(crop->Width);
            height = static_cast<UINT>(crop->Height);
        }
        if (width > LONG_MAX || height > LONG_MAX ||
                static_cast<std::uint64_t>(width) * height * 4 > UINT_MAX) { return E_OUTOFMEMORY; }
        if (!dc_ || width != width_ || height != height_) {
            reset();
            dc_ = CreateCompatibleDC(nullptr);
            if (!dc_) { return HRESULT_FROM_WIN32(GetLastError()); }
            BITMAPINFO information{};
            information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            information.bmiHeader.biWidth = static_cast<LONG>(width);
            information.bmiHeader.biHeight = -static_cast<LONG>(height);
            information.bmiHeader.biPlanes = 1;
            information.bmiHeader.biBitCount = 32;
            information.bmiHeader.biCompression = BI_RGB;
            void* storage = nullptr;
            bitmap_ = CreateDIBSection(dc_, &information, DIB_RGB_COLORS, &storage, nullptr, 0);
            if (!bitmap_) { reset(); return E_OUTOFMEMORY; }
            original_ = SelectObject(dc_, bitmap_);
            if (!original_ || original_ == HGDI_ERROR) { reset(); return E_FAIL; }
            bits_ = static_cast<BYTE*>(storage);
            width_ = width;
            height_ = height;
        }
        result = image->CopyPixels(crop, width_ * 4, width_ * height_ * 4, bits_);
        if (FAILED(result)) { return result; }

        // Use the actual nonzero alpha silhouette for the native region as well
        // as Windows' layered hit testing. A shadow keeps its partial opacity;
        // a transparent gap cannot become an opaque rectangle or catch clicks.
        struct Region final {
            HRGN value = CreateRectRgn(0, 0, 0, 0);
            ~Region() { if (value) { DeleteObject(value); } }
        } region, band, current;
        if (!region.value || !band.value || !current.value) { return E_OUTOFMEMORY; }
        using Run = std::pair<LONG, LONG>;
        std::vector<Run> previous;
        std::vector<Run> runs;
        LONG first_row = 0;
        for (LONG y = 0; y <= static_cast<LONG>(height_); ++y) {
            runs.clear();
            if (y < static_cast<LONG>(height_)) {
                for (LONG x = 0; x < static_cast<LONG>(width_);) {
                    const auto alpha = [&](LONG column) {
                        return bits_[(static_cast<std::size_t>(y) * width_ + column) * 4 + 3];
                    };
                    while (x < static_cast<LONG>(width_) && alpha(x) == 0) { ++x; }
                    const LONG first = x;
                    while (x < static_cast<LONG>(width_) && alpha(x) != 0) { ++x; }
                    if (first != x) { runs.emplace_back(first, x); }
                }
            }
            if (runs == previous) { continue; }
            for (const auto& [left, right] : previous) {
                if (!SetRectRgn(band.value, left, first_row, right, y) ||
                        CombineRgn(region.value, region.value, band.value, RGN_OR) == ERROR) { return E_FAIL; }
            }
            previous.swap(runs);
            first_row = y;
        }
        if (GetWindowRgn(window, current.value) == ERROR || !EqualRgn(current.value, region.value)) {
            if (!SetWindowRgn(window, region.value, FALSE)) { return HRESULT_FROM_WIN32(GetLastError()); }
            region.value = nullptr; // Windows owns the applied region.
        }
        POINT origin{};
        SIZE size{static_cast<LONG>(width_), static_cast<LONG>(height_)};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        return UpdateLayeredWindow(window, nullptr, nullptr, &size, dc_, &origin, 0, &blend, ULW_ALPHA)
            ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

private:
    void reset() noexcept
    {
        if (dc_ && original_ && original_ != HGDI_ERROR) { SelectObject(dc_, original_); }
        if (bitmap_) { DeleteObject(bitmap_); }
        if (dc_) { DeleteDC(dc_); }
        dc_ = nullptr;
        bitmap_ = nullptr;
        original_ = nullptr;
        bits_ = nullptr;
        width_ = height_ = 0;
    }

    HDC dc_{};
    HBITMAP bitmap_{};
    HGDIOBJ original_{};
    BYTE* bits_{};
    UINT width_ = 0;
    UINT height_ = 0;
};

struct SurfaceCache final {
    std::vector<D2D1_POINT_2F> outline;
    ComPtr<ID2D1LinearGradientBrush> brush;
    std::vector<launcher::ColorStop> stops;
};

std::wstring utf16(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const int length = static_cast<int>(text.size());
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), length, nullptr, 0);
    if (needed == 0) {
        throw std::runtime_error("Text is not valid UTF-8.");
    }
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length,
            result.data(), needed) == 0) {
        throw std::runtime_error("Cannot convert UTF-8 text.");
    }
    return result;
}

std::string utf8(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    const int length = static_cast<int>(text.size());
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text.data(), length, nullptr, 0, nullptr, nullptr);
    if (needed == 0) {
        return "[invalid Unicode]";
    }
    std::string result(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length,
        result.data(), needed, nullptr, nullptr);
    return result;
}

std::filesystem::path executable_path()
{
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
        throw std::runtime_error("Cannot determine the launcher executable path.");
    }
    return std::filesystem::path(std::wstring(buffer.data(), length));
}

launcher::NativeLaunchAction filesystem_log_action(const std::filesystem::path& journal)
{
    std::array<wchar_t, MAX_PATH> directory{};
    const auto length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    if (length == 0 || length >= directory.size()) { throw std::runtime_error("Cannot find the Windows text editor."); }
    launcher::NativeLaunchAction action;
    action.target = (std::filesystem::path(directory.data()) / L"notepad.exe").native();
    action.arguments = L"\"" + journal.native() + L"\"";
    return action;
}

std::filesystem::path write_adobe_cc_report(std::string_view text, std::filesystem::path destination = {})
{
    if (destination.empty()) {
        const auto temporary = std::filesystem::temp_directory_path();
        std::array<wchar_t, MAX_PATH> name{};
        if (!GetTempFileNameW(temporary.c_str(), L"LAC", 0, name.data())) {
            throw std::system_error(GetLastError(), std::system_category(), "Cannot create the AdobeCC report");
        }
        destination = name.data();
    }
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output << "\xEF\xBB\xBF" << text;
    output.close();
    return destination;
}

int activate_filesystem_notification()
{
    // COM launches this small mode after the dock has exited. Do not load any
    // configuration or create a second dock just to open the retained journal.
    launcher::AppNotification notification;
    const auto registered = notification.register_server(executable_path(), LAUNCHER_VERSION_TEST_HOOKS != 0);
    if (FAILED(registered)) { return 1; }
    const HANDLE activation = notification.activation_event();
    const auto deadline = GetTickCount64() + 30000;
    for (;;) {
        if (const auto journal = notification.take_activation()) {
            return launcher::launch_prepared_action(filesystem_log_action(*journal)).success ? 0 : 1;
        }
        const auto now = GetTickCount64();
        if (now >= deadline) { return 1; }
        const auto ready = MsgWaitForMultipleObjectsEx(1, &activation, static_cast<DWORD>(deadline - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (ready == WAIT_FAILED || ready == WAIT_TIMEOUT) { return 1; }
        MSG message{};
        for (unsigned int count = 0; count != 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
            if (message.message == WM_QUIT) { return 1; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

struct CoTaskMemoryCloser final {
    void operator()(wchar_t* memory) const noexcept { CoTaskMemFree(memory); }
};

const std::filesystem::path& user_data_directory()
{
    static const std::filesystem::path directory = [] {
        PWSTR folder = nullptr;
        const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &folder);
        const std::unique_ptr<wchar_t, CoTaskMemoryCloser> owned_folder(folder);
        if (FAILED(result) || folder == nullptr) {
            throw std::runtime_error("Cannot find the current user's AppData folder.");
        }
        return std::filesystem::path(folder) / L"D\u0142ugosz" / L"Launcher";
    }();
    return directory;
}

std::wstring installation_status(launcher::InstallationScope scope)
{
    using launcher::InstallationScope;
    switch (scope) {
    case InstallationScope::portable: return L"Portable";
    case InstallationScope::all_users: return L"Installed for all users";
    case InstallationScope::current_user: {
        wchar_t username[UNLEN + 1];
        DWORD length = UNLEN + 1;
        if (GetUserNameW(username, &length) && length > 1) {
            return L"Installed for " + std::wstring(username, length - 1);
        }
        return L"Installed for the current user";
    }
    case InstallationScope::unspecified: return {};
    }
    return {};
}

struct FilesystemDiagnostics final {
    FilesystemDiagnostics(std::filesystem::path path, const launcher::FileLookup& files) : journal(std::move(path), 256 * 1024, files) {}
    launcher::FilesystemErrorLog journal;
    std::atomic<bool> write_failed = false;
};

class ComApartment final {
public:
    ComApartment() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComApartment()
    {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }
    HRESULT result() const noexcept { return result_; }
private:
    HRESULT result_;
};

struct HandleCloser final {
    void operator()(void* handle) const noexcept
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};
using ProcessHandle = std::unique_ptr<void, HandleCloser>;

struct OwnedWindow final {
    HWND window{};
    DWORD process_id{};
    ProcessHandle process;
};

struct StopContext final {
    std::wstring executable;
    std::vector<OwnedWindow> windows;
    bool inspection_failed = false;
};

BOOL CALLBACK find_owned_window(HWND window, LPARAM parameter)
{
    auto& context = *reinterpret_cast<StopContext*>(parameter);
    std::array<wchar_t, 128> name{};
    if (GetClassNameW(window, name.data(), static_cast<int>(name.size())) == 0 ||
        wcscmp(name.data(), kWindowClass) != 0) {
        return TRUE;
    }
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id == 0 || process_id == GetCurrentProcessId()) {
        return TRUE;
    }
    ProcessHandle process(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, process_id));
    if (!process) {
        context.inspection_failed = true;
        return TRUE;
    }
    std::array<wchar_t, 32768> path{};
    DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &length)) {
        context.inspection_failed = true;
        return TRUE;
    }
    if (_wcsicmp(context.executable.c_str(), path.data()) == 0) {
        context.windows.push_back(OwnedWindow{window, process_id, std::move(process)});
    }
    return TRUE;
}

int stop_owned_instances(const std::filesystem::path& executable, Log& log)
{
    StopContext context{executable.wstring(), {}, false};
    if (!EnumWindows(find_owned_window, reinterpret_cast<LPARAM>(&context)) ||
        context.inspection_failed) {
        log.write("Stop failed: cannot safely inspect all matching launcher windows.");
        return 2;
    }
    bool success = true;
    for (const auto& owned : context.windows) {
        DWORD_PTR ignored = 0;
        if (SendMessageTimeoutW(owned.window, WM_CLOSE, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored) == 0) {
            if (WaitForSingleObject(owned.process.get(), 0) != WAIT_OBJECT_0) {
                log.write("Stop failed: launcher did not accept graceful close.");
                success = false;
                continue;
            }
        }
        if (WaitForSingleObject(owned.process.get(), 5000) != WAIT_OBJECT_0) {
            log.write("Stop failed: launcher process did not exit within five seconds.");
            success = false;
        }
    }
    log.write(success ? "Graceful stop complete." : "Graceful stop incomplete.");
    return success ? 0 : 2;
}

D2D1_COLOR_F color_rgb(std::uint32_t color)
{
    const auto component = [](std::uint32_t value) {
        return static_cast<float>(value) / 255.0F;
    };
    return D2D1::ColorF(component((color >> 16) & 0xffU),
        component((color >> 8) & 0xffU), component(color & 0xffU));
}

bool contains(D2D1_RECT_F rectangle, D2D1_POINT_2F point) noexcept
{
    return point.x >= rectangle.left && point.x < rectangle.right &&
        point.y >= rectangle.top && point.y < rectangle.bottom;
}

int pixels(float dip, UINT dpi) noexcept
{
    return static_cast<int>(dip * static_cast<float>(dpi) / 96.0F + 0.5F);
}

struct TabVisual final {
    std::wstring label;
    const launcher::ColorScheme* colors{};
    D2D1_RECT_F bounds{}; // Oriented full-view DIPs, including the long stem.
    D2D1_RECT_F logical_bounds{}; // Tab-relative layout DIPs: parallel, depth.
    // logical_bounds is the short head/label box; bounds includes the stem.
    std::size_t row{};
};

struct Monitor final {
    HMONITOR handle{};
    MONITORINFOEXW information{};
};

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter)
{
    auto& monitors = *reinterpret_cast<std::vector<Monitor>*>(parameter);
    Monitor entry;
    entry.handle = monitor;
    entry.information.cbSize = sizeof(entry.information);
    if (!GetMonitorInfoW(monitor, &entry.information)) {
        return FALSE;
    }
    try {
        monitors.push_back(entry);
        return TRUE;
    } catch (...) {
        return FALSE;
    }
}

launcher::PixelRect pixel_rect(RECT rectangle) noexcept
{
    return {rectangle.left, rectangle.top, rectangle.right, rectangle.bottom};
}

std::string rect_text(RECT rectangle)
{
    return std::to_string(rectangle.left) + "," + std::to_string(rectangle.top) + "," +
        std::to_string(rectangle.right) + "," + std::to_string(rectangle.bottom);
}

const char* edge_name(launcher::Edge edge) noexcept
{
    switch (edge) {
    case launcher::Edge::top: return "top";
    case launcher::Edge::bottom: return "bottom";
    case launcher::Edge::left: return "left";
    case launcher::Edge::right: return "right";
    }
    return "unknown";
}

struct IconVisual final {
    launcher::IconSource source;
    std::wstring label;
    D2D1_RECT_F bounds{};
    ComPtr<IWICBitmapSource> image;
    ComPtr<ID2D1Bitmap> bitmap;
    std::size_t item_index = 0;
    std::wstring tooltip;
    launcher::PreparedLaunch launch;
    std::shared_ptr<const std::vector<launcher::TargetVersion>> catalog;
    launcher::ItemVersionCatalog versions;
    std::optional<std::filesystem::path> pinned_version;
    bool can_admin = false;
    bool can_reveal = false;
    std::uint64_t presentation_generation = 0;
};

using ItemBinding = std::pair<std::size_t, std::size_t>;

struct ItemPresentationJob final {
    std::uint64_t generation = 0;
    std::uint64_t visit = 0;
    launcher::ItemPresentationRequest request;
    std::shared_ptr<const std::vector<launcher::TargetVersion>> catalog;
    launcher::ItemVersionCatalog versions;
    std::optional<std::filesystem::path> pinned_version;
    std::optional<launcher::ItemPresentation> result;
    std::atomic<bool> ready = false;
    std::uint64_t bytes = 0;
    std::string error;
};

struct ItemPresentationRecord final {
    std::uint64_t generation = 0;
    std::shared_ptr<ItemPresentationJob> desired;
    std::shared_ptr<ItemPresentationJob> running;
    std::shared_ptr<ItemPresentationJob> adopted;
};

struct LazyShellMenu final {
    HWND owner = nullptr;
    std::wstring target;
    HMENU menu = nullptr;
    bool extended = false;
    bool initialized = false;
    std::unique_ptr<launcher::ShellContextMenu> shell;
    UINT first_command = kIconShellFirst;
    UINT last_command = kIconShellLast;
};

struct VersionMenuPage final {
    HMENU menu = nullptr;
    std::size_t first = 0;
    std::size_t count = 0;
    bool initialized = false;
};

struct OwnedPopupMenu final {
    HMENU menu = CreatePopupMenu();
    ~OwnedPopupMenu() { if (menu) { DestroyMenu(menu); } }
};

struct VersionMenuChoice final {
    std::size_t row = 0;
    UINT command = 0;
    POINT screen{};
    std::shared_ptr<OwnedPopupMenu> menu;
    std::shared_ptr<LazyShellMenu> shell;
};

struct VersionMenuSession final {
    launcher::Item item;
    ItemBinding source{};
    std::vector<launcher::TargetVersion> versions;
    std::optional<launcher::LaunchAction> normal_action;
    HMENU root = nullptr;
    std::vector<VersionMenuPage> pages;
    std::optional<std::pair<HMENU, UINT>> focused_row;
    std::optional<VersionMenuChoice> choice;
    bool tracking_child = false;
    bool limited = false;
    bool latest_known = false;
    bool saved_default = false;
    std::optional<std::filesystem::path> latest_version;
    HMENU child_menu = nullptr;
};

struct ShellMenuFrame final {
    std::vector<LazyShellMenu*>& frames;
    explicit ShellMenuFrame(std::vector<LazyShellMenu*>& stack, LazyShellMenu* frame) : frames(stack) { frames.push_back(frame); }
    ~ShellMenuFrame() { frames.pop_back(); }
};

std::string icon_description(const launcher::IconSource& source)
{
    if (const auto* stock = std::get_if<launcher::StockIcon>(&source)) {
        return "stock '" + std::string(launcher::stock_icon_names.at(static_cast<std::size_t>(*stock))) + "'";
    }
    return std::visit([](const auto& value) -> std::string {
        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, launcher::StockIcon>) { return {}; }
        else {
            auto description = "file '" + utf8(value.path.wstring()) + "'";
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, launcher::IconGroup>) {
                if (const auto* index = std::get_if<launcher::IconIndex>(&value.selector)) {
                    description += " index=" + std::to_string(index->value);
                } else { description += " id=" + std::to_string(std::get<launcher::IconResourceId>(value.selector).value); }
            } else if constexpr (std::is_same_v<std::decay_t<decltype(value)>, launcher::PeResourceIcon>) {
                const auto identifier = [](const launcher::ResourceIdentifier& key) {
                    if (const auto* id = std::get_if<std::uint16_t>(&key)) { return std::to_string(*id); }
                    return "'" + std::get<std::string>(key) + "'";
                };
                description += " type=" + identifier(value.type) + " name/id=" + identifier(value.name);
                if (value.language) { description += " language=" + std::to_string(*value.language); }
            }
            return description;
        }
    }, source);
}

struct LaunchCompletion final {
    std::atomic<bool> ready = false;
    launcher::LaunchResult result;
    std::wstring label;
};

struct RestartCompletion final {
    std::atomic<bool> ready = false;
    launcher::LaunchResult result;
    HANDLE cancel = nullptr;
    bool committed = false;
    ~RestartCompletion() {
        if (cancel) {
            if (!committed) { SetEvent(cancel); }
            CloseHandle(cancel);
        }
    }
};

struct PendingVersionLaunch final {
    launcher::Item item;
    std::wstring label;
    std::string verb;
    bool reveal = false;
    launcher::VersionSearchKey search;
    std::shared_ptr<const launcher::VersionDiscoveryAttempt> previous_attempt;
    std::optional<launcher::LaunchAction> resolved;
    bool terminal = false;
    std::pair<std::size_t, std::size_t> source{};
    std::optional<launcher::LaunchResult> failure;
    std::optional<std::filesystem::path> pinned_version;
};

// Used only by --smoke-test. Inspect the face chosen by actual text layout,
// rather than merely trusting the requested format's automatic-axis flag.
class FontAxisInspector final : public IDWriteTextRenderer {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) override
    {
        if (value == nullptr) { return E_POINTER; }
        *value = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWriteTextRenderer) ||
                id == __uuidof(IDWritePixelSnapping)) {
            *value = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = static_cast<ULONG>(InterlockedDecrement(&references_));
        if (remaining == 0) { delete this; }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override
    {
        if (disabled == nullptr) { return E_POINTER; }
        *disabled = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) override
    {
        if (transform == nullptr) { return E_POINTER; }
        *transform = {1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* scale) override
    {
        if (scale == nullptr) { return E_POINTER; }
        *scale = 1.0F;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT, FLOAT, DWRITE_MEASURING_MODE,
        const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override
    {
        if (run == nullptr || run->fontFace == nullptr) { return E_POINTER; }
        if (examined_) { return S_OK; }
        examined_ = true;
        ComPtr<IDWriteFontFace5> face;
        if (FAILED(run->fontFace->QueryInterface(IID_PPV_ARGS(face.GetAddressOf())))) {
            return S_OK; // Optional on older Windows 10.
        }
        std::array<DWRITE_FONT_AXIS_VALUE, 32> axes{};
        const UINT32 count = face->GetFontAxisValueCount();
        if (count == 0 || count > axes.size()) { return S_OK; }
        HRESULT result = face->GetFontAxisValues(axes.data(), count);
        if (FAILED(result)) { return result; }
        for (UINT32 index = 0; index < count; ++index) {
            if (axes[index].axisTag == DWRITE_FONT_AXIS_TAG_OPTICAL_SIZE) {
                optical_size = axes[index].value;
            }
            if (axes[index].axisTag == DWRITE_FONT_AXIS_TAG_WEIGHT) {
                weight = axes[index].value;
            }
        }
        ComPtr<IDWriteFontResource> resource;
        result = face->GetFontResource(resource.GetAddressOf());
        if (FAILED(result)) { return result; }
        std::array<DWRITE_FONT_AXIS_RANGE, 32> ranges{};
        const UINT32 range_count = resource->GetFontAxisCount();
        if (range_count > ranges.size()) { return S_OK; }
        result = resource->GetFontAxisRanges(ranges.data(), range_count);
        if (FAILED(result)) { return result; }
        for (UINT32 index = 0; index < range_count; ++index) {
            if (ranges[index].axisTag == DWRITE_FONT_AXIS_TAG_OPTICAL_SIZE) {
                optical_range = ranges[index];
            }
            if (ranges[index].axisTag == DWRITE_FONT_AXIS_TAG_WEIGHT) {
                weight_range = ranges[index];
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT,
        const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT,
        const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT,
        IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }

    std::optional<float> optical_size;
    std::optional<float> weight;
    std::optional<DWRITE_FONT_AXIS_RANGE> optical_range;
    std::optional<DWRITE_FONT_AXIS_RANGE> weight_range;

private:
    virtual ~FontAxisInspector() = default;
    LONG references_ = 1;
    bool examined_ = false;
};

class Window final {
public:
    Window(HINSTANCE instance, launcher::Configuration configuration,
        std::filesystem::path config_path, launcher::FileLookup files, Log& log, launcher::NativeVersionDiscoveryExecutor& background
#if LAUNCHER_VERSION_TEST_HOOKS
        , std::shared_ptr<launcher::VersionScenarios> scenario = {}, std::filesystem::path scenario_path = {}
#endif
        )
        : instance_(instance), configuration_(std::move(configuration)),
          layout_(launcher::layout_configuration(configuration_)),
          peek_(configuration_.dock.auto_hide),
          config_path_(std::move(config_path)),
          config_directory_(config_path_.parent_path()),
          files_(std::move(files)),
          runtime_path_(files_.runtime_path(config_path_)), log_(log), background_(background)
    {
#if LAUNCHER_VERSION_TEST_HOOKS
        scenario_ = std::move(scenario);
        scenario_path_ = std::move(scenario_path);
#endif
        auto patterns = std::make_shared<std::map<launcher::VersionSearchKey, std::filesystem::path>>();
        for (const auto& tab : configuration_.tabs) {
            for (const auto& item : tab.items) {
                if (const auto key = version_search(item)) {
                    patterns->try_emplace(*key, key->kind == launcher::VersionSearchKind::target_pattern
                        ? *launcher::version_target_pattern(*item.launch, config_directory_) : std::filesystem::path{});
                }
                if (item.shift_launch && item.shift_launch->handler == "adobeCC") {
                    patterns->try_emplace(launcher::adobe_cc_search_key(*item.shift_launch, config_directory_), std::filesystem::path{});
                }
            }
        }
        for (const auto& [key, pattern] : *patterns) { version_keys_.push_back(key); }
        launcher::VersionDiscoveryService::Scanner scan = [patterns](const auto& key) {
            if (key.kind == launcher::VersionSearchKind::adobe_catalog) {
                return launcher::scan_adobe_cc_catalog(key);
            }
            return launcher::scan_target_versions_detailed(patterns->at(key));
        };
#if LAUNCHER_VERSION_TEST_HOOKS
        if (scenario_) { scan = scenario_->scanner(); }
#endif
        filesystem_diagnostics_ = std::make_shared<FilesystemDiagnostics>(log_.filesystem_error_path(), files_);
        versions_ = std::make_unique<launcher::VersionDiscoveryService>([scan = std::move(scan), errors = filesystem_diagnostics_](const auto& key) {
            auto result = scan(key);
            // Write before publication, independently of UI adoption or whether
            // the binding still exists. Captures own the journal during shutdown.
            if (!errors->journal.append(result.diagnostics)) { errors->write_failed.store(true, std::memory_order_relaxed); }
            return result;
        }, background_);
        versions_->set_searches(version_keys_);
    }

    ~Window()
    {
        background_.stop_admission();
        cancel_version_intents();
        versions_->deactivate();
        restart_worker_.request_stop();
        if (restart_worker_.joinable()) { restart_worker_.join(); }
        remove_hooks();
        destroy_icon_tooltips();
        if (window_ != nullptr && IsWindow(window_)) {
            DestroyWindow(window_);
        }
        if (large_window_icon_ != nullptr) { DestroyIcon(large_window_icon_); }
        if (small_window_icon_ != nullptr) { DestroyIcon(small_window_icon_); }
    }

    HRESULT initialize(bool smoke_test, bool test_ui, bool restart_pending, bool report_runtime_errors, bool monitor_startup)
    {
        smoke_mode_ = smoke_test;
        test_ui_mode_ = test_ui;
        restart_pending_ = restart_pending;
        diagnostics_enabled_ = test_ui && !smoke_test;
        initial_foreground_ = GetForegroundWindow();
        desktop_icons_ = launcher::sample_desktop_icon_size();
        log_.write("Desktop icon baseline: pixels=", std::to_string(desktop_icons_.size_px),
            "; desktop-dpi=", std::to_string(desktop_icons_.desktop_dpi),
            "; nominal-dip=", std::to_string(desktop_icons_.size_dip),
            "; source=", (desktop_icons_.shell_sample ? "shell" : "fallback"), ".");
        std::optional<std::string> runtime_error;
        if (!smoke_mode_) {
            try {
                const auto runtime = launcher::load_runtime_adjustments(runtime_path_, files_);
                auto versions = launcher::load_active_versions(runtime_path_, files_);
                // Validate composition before accepting stored adjustments.
                (void)launcher::resolve_effective_settings(configuration_, runtime,
                    desktop_icons_.size_dip, dpi_);
                runtime_ = runtime;
                active_versions_ = std::move(versions);
                log_.write("Runtime settings: ", utf8(runtime_path_.wstring()), ".");
            } catch (const std::exception& error) {
                log_.write("Runtime settings could not be fully applied; file preserved: ", error.what());
                runtime_ = {};
                active_versions_.clear();
                runtime_error = error.what();
            }
        }
        HRESULT result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            d2d_factory_.GetAddressOf());
        if (FAILED(result)) {
            return result;
        }
        result = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(write_factory_.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        result = CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_factory_.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        icon_images_.initialize(wic_factory_.Get());
        menu_icons_.initialize(wic_factory_.Get());
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = dispatch;
        window_class.hInstance = instance_;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // LoadIcon's shared class default is owned by Windows. Per-window,
        // DPI-sized icons below are separate owned handles.
        window_class.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_LAUNCHER));
        window_class.lpszClassName = kWindowClass;
        if (RegisterClassExW(&window_class) == 0) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        monitor_ = select_monitor(true);
        const DWORD extended_style = WS_EX_NOACTIVATE | WS_EX_TOPMOST |
            (test_ui ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW) |
            WS_EX_LAYERED;
        window_ = CreateWindowExW(extended_style, kWindowClass,
            L"Launcher " LAUNCHER_VERSION_WSTRING, WS_POPUP,
            monitor_.information.rcWork.left + 1, monitor_.information.rcWork.top + 1,
            1, 1, nullptr, nullptr, instance_, this);
        if (window_ == nullptr) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        dpi_ = GetDpiForWindow(window_);
        update_window_icons();
        bool notices_enabled = !smoke_mode_ && !test_ui_mode_;
#if LAUNCHER_VERSION_TEST_HOOKS
        if (scenario_) { notices_enabled = !smoke_mode_ && scenario_->notifications_enabled(); }
#endif
        if (notices_enabled) {
            filesystem_notification_ = std::make_unique<launcher::AppNotification>(background_.completion_event());
            const auto registration = filesystem_notification_->register_server(executable_path(), LAUNCHER_VERSION_TEST_HOOKS != 0);
            if (FAILED(registration)) { log_.result("Filesystem notification registration failed", registration); }
        }
        try {
            effective_ = launcher::resolve_effective_settings(configuration_, runtime_,
                desktop_icons_.size_dip, dpi_);
        } catch (const std::exception& error) {
            if (smoke_mode_) { throw; }
            log_.write("Runtime adjustments ignored at display DPI; file preserved: ", error.what());
            runtime_ = {};
            active_versions_.clear();
            runtime_error = error.what();
            effective_ = launcher::resolve_effective_settings(configuration_, runtime_,
                desktop_icons_.size_dip, dpi_);
        }
        // Convert any legacy spacing factor only after the selected display's
        // actual DPI is known. Future previews/saves use this absolute gap.
        runtime_.icon_spacing_px = effective_.icon_spacing_px;
        runtime_.thickness_dip = effective_.thickness_dip;
        runtime_.spacing_scale = 1.0;
        create_text_format();
        calculate_layout();
        // A saved adjustment may outgrow a changed display; keep it accessible
        // using the same fit policy as preview and live display reconfiguration.
        update_placement(false);
        load_icons();
        create_icon_tooltips();
        if (!smoke_mode_) {
            install_hooks();
            peek_.set_suppressed(fullscreen_active());
        }
        apply_state(true);
        if (restart_pending_ || monitor_startup) {
            result = render();
            if (result == D2DERR_RECREATE_TARGET) { result = render(); }
            if (FAILED(result)) { return result; }
        }
        if (diagnostics_enabled_) {
            diagnostics_started_ = GetTickCount64();
            SetTimer(window_, kDiagnosticTimer, 5000, nullptr);
        }
        if (!smoke_mode_ && !test_ui_mode_ && !files_.portable()) {
            SetTimer(window_, kUpdaterTimer, 1000, nullptr);
        }
        log_.write("Drawing initialized; dpi=", std::to_string(dpi_),
            ", grid=", std::to_string(layout_.columns), "x",
            std::to_string(layout_.rows), ", icon-count=",
            std::to_string(icons_.size()), ".");
        configure_restart_arguments();
        if (runtime_error && report_runtime_errors) {
            pending_runtime_error_ = L"Could not load saved runtime settings.\n\n" + runtime_path_.wstring() +
                L"\n\n" + utf16(*runtime_error) + L"\n\nLauncher will use the settings from its configuration. The runtime file was left unchanged.";
        }
        if (!restart_pending_ && !monitor_startup) { show_pending_runtime_error(); }
        return S_OK;
    }

    void show_pending_runtime_error()
    {
        if (pending_runtime_error_.empty() || window_ == nullptr) { return; }
        const auto message = std::exchange(pending_runtime_error_, {});
        MessageBoxW(window_, message.c_str(), L"Launcher runtime settings", MB_OK | MB_ICONERROR);
    }

    void reveal_after_restart()
    {
        restart_pending_ = false;
        sync_version_activation();
        admit_item_presentations();
        if (peek_.state() != launcher::DockState::suppressed) { ShowWindow(window_, SW_SHOWNOACTIVATE); }
        InvalidateRect(window_, nullptr, FALSE);
        sync_icon_tooltips();
        show_pending_runtime_error();
    }

    void reveal_from_launch()
    {
        if (display_layout_failed_) { return; }
        restore_from_taskbar();
        peek_.pointer_changed(pointer_inside());
        peek_.reveal();
        // The shortcut may be far from the dock. Give the user time to reach it
        // before normal auto-hide resumes.
        launch_reveal_until_ = GetTickCount64() + kLaunchRevealGraceMs;
        cancel_timer();
        apply_state();
        sync_timer();
    }

    void collect_background()
    {
        background_.reset_completion();
        if (filesystem_notification_) {
            if (const auto journal = filesystem_notification_->take_activation()) { open_filesystem_error_log(*journal); }
        }
        finish_version_discovery();
        finish_launch();
        finish_restart();
        background_.reap_exited();
    }

    void adopt_background()
    {
        // A completion may have been collected while an interaction held its
        // presentation. Reconsider it when ordinary message dispatch returns.
        adopt_version_results();
        collect_item_presentations();
        adopt_item_presentations();
        finish_launch();
        if (display_restart_requested_ && !smoke_mode_ && !restart_pending_ &&
                !version_adoption_held() && !launch_dialog_active_) {
            display_restart_requested_ = false;
            log_.write("Display resolution changed; restarting to reload desktop icon size and layout.");
            try {
                start_restart();
            } catch (const std::exception& error) {
                log_.write("Display restart could not start: ", error.what());
                const auto text = utf16(error.what());
                MessageBoxW(window_, text.c_str(), L"Cannot restart Launcher", MB_OK | MB_ICONERROR);
            }
        }
    }

    void enable_snapshot_hotkey()
    {
        // Called only after a restart's parent has exited and released its key.
        snapshot_hotkey_registered_ = RegisterHotKey(window_, kSnapshotHotkey,
            MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F11) != FALSE;
        if (snapshot_hotkey_registered_) {
            log_.write("Window snapshot shortcut registered: Ctrl+Alt+Shift+F11.");
        } else {
            log_.result("Window snapshot shortcut unavailable; Shift context-menu capture remains available",
                HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    HRESULT render_preview(const std::filesystem::path& path, std::size_t page)
    {
        if (std::filesystem::exists(path)) { return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS); }
        if (page >= tabs_.size() || !select_tab(page)) { return E_INVALIDARG; }
        peek_ = launcher::PeekController(true);
        peek_.set_menu_open(true);
        apply_state(true);
        if (!settle_version_discovery()) { return E_FAIL; }
        discard_device_resources();
        // Export the full silhouette, including walls and exterior shadows.
        const int padding = prism_frame_padding_px();
        render_offset_px_ = {-padding, -padding};
        const UINT width = static_cast<UINT>(std::max(1, pixels(width_dip_ * fit_scale_, dpi_)) + 2 * padding);
        const UINT height = static_cast<UINT>(std::max(1, pixels(height_dip_ * fit_scale_, dpi_)) + 2 * padding);
        HRESULT result = wic_factory_->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnLoad, preview_bitmap_.GetAddressOf());
        if (SUCCEEDED(result)) { result = render(); }
        if (SUCCEEDED(result)) { result = launcher::write_png(wic_factory_.Get(), preview_bitmap_.Get(), path, dpi_, files_); }
        log_.result("Drawing preview export", result);
        return result;
    }

    HRESULT smoke_test()
    {
        const auto saved_controller = peek_;
        peek_ = launcher::PeekController(true);
        peek_.set_menu_open(true);
        apply_state(true);
        if (!settle_version_discovery()) { return E_FAIL; }
        if (!verify_native_state(launcher::DockState::expanded)) {
            return E_FAIL;
        }
        HRESULT result = render();
        if (result == D2DERR_RECREATE_TARGET) {
            result = render();
        }
        if (SUCCEEDED(result) && std::any_of(icons_.begin(), icons_.end(),
                [](const IconVisual& icon) { return !icon.image || !icon.bitmap; })) {
            log_.write("Smoke test failed: an icon and its fallback could not be drawn.");
            result = E_FAIL;
        }
        if (SUCCEEDED(result)) {
            result = verify_font_axes();
        }
        if (SUCCEEDED(result)) { result = verify_window_icons(); }
        if (SUCCEEDED(result)) { result = verify_surface_cache(); }
        if (SUCCEEDED(result)) { result = verify_snapshot_copy(); }
        if (SUCCEEDED(result)) { result = verify_thickness_dialog(); }
        if (SUCCEEDED(result)) {
            result = verify_tab_metrics();
        }
        if (SUCCEEDED(result)) {
            result = verify_grid_spacing();
        }
        if (SUCCEEDED(result)) {
            result = verify_grid_screen_order();
        }
        if (SUCCEEDED(result)) {
            result = verify_icon_tooltips(true);
        }
        if (SUCCEEDED(result)) { result = verify_hover(); }
        if (SUCCEEDED(result)) { result = verify_installed_versions(); }
        if (SUCCEEDED(result)) { result = verify_version_discovery(); }
        if (SUCCEEDED(result)) { result = verify_item_presentations(); }
        if (SUCCEEDED(result)) { result = verify_pending_overlay(); }
        if (SUCCEEDED(result)) { result = verify_version_menu_pagination(); }
        if (SUCCEEDED(result)) {
            result = verify_tab_selection();
        }
        if (SUCCEEDED(result)) {
            result = verify_tab_gap_hover();
        }
        if (SUCCEEDED(result)) {
            peek_.set_menu_open(false);
            peek_.pointer_changed(false);
            peek_.timer_elapsed();
            apply_state();
            if (!verify_native_state(launcher::DockState::collapsed)) {
                result = E_FAIL;
            }
            if (SUCCEEDED(result)) { result = verify_icon_tooltips(false); }
            if (SUCCEEDED(result)) { result = verify_hover(false); }
        }
        if (SUCCEEDED(result)) {
            result = render();
            peek_.set_suppressed(true);
            apply_state();
            if (!verify_native_state(launcher::DockState::suppressed)) {
                result = E_FAIL;
            }
            if (SUCCEEDED(result)) { result = verify_icon_tooltips(false); }
        }
        if (SUCCEEDED(result)) {
            peek_.set_suppressed(false);
            apply_state();
            if (!verify_native_state(launcher::DockState::collapsed) ||
                GetForegroundWindow() == window_) {
                result = E_FAIL;
            }
        }
        if (SUCCEEDED(result)) { result = verify_dock_motion(); }
        if (SUCCEEDED(result)) { result = verify_drawing_recovery(); }
        if (SUCCEEDED(result)) { result = verify_taskbar_hide(); }
        peek_ = saved_controller;
        apply_state(true);
        if (GetForegroundWindow() != initial_foreground_) {
            log_.write("Smoke test: foreground changed externally during testing.");
        }
        if (SUCCEEDED(result)) {
            log_.write("Smoke test passed: graphics/icons/font axes/tab metrics/pixel spacing/icon label tooltips/tab selection, native expanded/collapsed clips, "
                "selected monitor/DPI, suppression/restoration, drawing recovery, taskbar Hide/restore, no launcher activation.");
        } else {
            log_.result("Smoke test draw failed", result);
        }
        return result;
    }

#if LAUNCHER_VERSION_TEST_HOOKS
    void scenario_control(const launcher::VersionScenarioCommand& command)
    {
        if (!scenario_) { return; }
        switch (command.kind) {
        case launcher::VersionScenarioCommandKind::close:
            SendMessageW(window_, WM_CLOSE, 0, 0);
            break;
        case launcher::VersionScenarioCommandKind::restart:
            start_restart();
            break;
        case launcher::VersionScenarioCommandKind::select_page:
            if (!select_tab(command.page)) { scenario_->checkpoint("page-index-rejected"); }
            break;
        case launcher::VersionScenarioCommandKind::checkpoint:
            scenario_->checkpoint("ui-checkpoint");
            break;
        case launcher::VersionScenarioCommandKind::activate_item:
            if (command.page < icons_.size()) { launch_icon(command.page); }
            else { scenario_->checkpoint("icon-index-rejected"); }
            break;
        case launcher::VersionScenarioCommandKind::open_icon_menu:
            if (command.page < icons_.size()) {
                const auto rectangle = icon_client_px_rect(icons_[command.page]);
                POINT screen{(rectangle.left + rectangle.right) / 2, (rectangle.top + rectangle.bottom) / 2};
                ClientToScreen(window_, &screen);
                icon_context_menu(command.page, screen);
            } else { scenario_->checkpoint("menu-index-rejected"); }
            break;
        case launcher::VersionScenarioCommandKind::fatal_error:
            break; // The outer message pump throws after receiving this command.
        }
    }
#endif

private:
    void scenario_checkpoint(std::string_view stage)
    {
#if LAUNCHER_VERSION_TEST_HOOKS
        if (scenario_) { scenario_->checkpoint(stage); }
#else
        static_cast<void>(stage);
#endif
    }

    HRESULT verify_dock_motion()
    {
        const auto original = peek_;
        peek_ = launcher::PeekController(true);
        apply_state(true);
        peek_.set_menu_open(true);
        cache_motion_frame();
        motion_.reset(false);
        motion_.start(true, 400, 1000);
        motion_active_ = true;
        applied_state_ = launcher::DockState::expanded;
        bool valid = true;
        const auto sample = [&](std::uint64_t now) {
            const auto expected_canvas = launcher::dock_canvas(placement_, motion_.value(now));
            const auto expected = launcher::dock_viewport(expected_canvas,
                pixel_rect(monitor_.information.rcWork), prism_frame_padding_px());
            advance_motion(now);
            RECT actual{};
            if (!GetWindowRect(window_, &actual) || pixel_rect(actual) != expected) { return false; }
            if (!motion_frame_) { return true; }
            HRGN region = CreateRectRgn(0, 0, 0, 0);
            if (!region) { return false; }
            bool matches = GetWindowRgn(window_, region) != ERROR;
            // Compare transferred alpha and the native silhouette at samples
            // throughout the clipped window, rather than just its rectangle.
            for (LONG y = 0; matches && y < motion_crop_.Height; y += 7) {
                for (LONG x = 0; matches && x < motion_crop_.Width; x += 7) {
                    WICRect source{motion_crop_.X + x, motion_crop_.Y + y, 1, 1};
                    std::array<BYTE, 4> pixel{};
                    matches = SUCCEEDED(motion_frame_->CopyPixels(&source, 4, 4, pixel.data())) &&
                        (PtInRegion(region, x, y) != FALSE) == (pixel[3] != 0);
                }
            }
            DeleteObject(region);
            return matches;
        };
        for (const auto now : {1000ULL, 1100ULL, 1200ULL}) { valid &= sample(now); }
        valid &= SUCCEEDED(verify_snapshot_copy());
        const double before = motion_.value(1200);
        peek_ = launcher::PeekController(true);
        applied_state_ = launcher::DockState::collapsed;
        motion_.start(false, 800, 1200);
        valid &= motion_.value(1200) == before;
        for (const auto now : {1200ULL, 1400ULL, 1600ULL}) { valid &= sample(now); }
        valid &= !motion_active_ && !motion_frame_ && verify_native_state(launcher::DockState::collapsed);
        peek_.set_menu_open(true);
        cache_motion_frame();
        motion_.start(true, 400, 2000);
        motion_active_ = true;
        applied_state_ = launcher::DockState::expanded;
        valid &= sample(2200);
        interaction_hold_ = true;
        apply_state();
        valid &= !motion_active_ && !motion_frame_ && verify_native_state(launcher::DockState::expanded);
        interaction_hold_ = false;
        motion_.reset(false);
        cache_motion_frame();
        motion_.start(true, 400, 3000);
        motion_active_ = true;
        valid &= sample(3200);
        peek_.set_suppressed(true);
        apply_state();
        valid &= !motion_active_ && !motion_frame_ && verify_native_state(launcher::DockState::suppressed);
        peek_ = original;
        apply_state(true);
        log_.write(valid ? "Smoke dock motion passed: clipped intermediate frames, cached alpha/native region, continuous reversal, dialog/menu settling, suppression cancellation."
            : "Smoke dock motion failed: intermediate placement, alpha region, reversal, or suppression.");
        return valid ? S_OK : E_FAIL;
    }

    HRESULT verify_prism_frame()
    {
        if (!window_bitmap_) { return E_FAIL; }
        if ((GetWindowLongPtrW(window_, GWL_EXSTYLE) & WS_EX_LAYERED) == 0) { return E_FAIL; }
        // Probe a straight visible page wall, away from tabs, corners and their
        // shadows. Its opaque pixels must stop at the snapped extrusion width.
        const auto page = D2D1::RectF(panel_bounds_.left * fit_scale_, panel_bounds_.top * fit_scale_,
            panel_bounds_.right * fit_scale_, panel_bounds_.bottom * fit_scale_);
        const LONG edge = vertical() ? pixels(page.bottom, dpi_) - render_offset_px_.y
            : pixels(page.right, dpi_) - render_offset_px_.x;
        const LONG middle = vertical() ? pixels((page.left + page.right) * 0.5F, dpi_) - render_offset_px_.x
            : pixels((page.top + page.bottom) * 0.5F, dpi_) - render_offset_px_.y;
        const LONG thickness = sheet_thickness_pixels();
        UINT width = 0;
        UINT height = 0;
        HRESULT result = window_bitmap_->GetSize(&width, &height);
        if (FAILED(result)) { return result; }
        const LONG end = static_cast<LONG>(vertical() ? height : width);
        // A dock filling the monitor can legitimately meet the work-area clip.
        if (edge < 0 || middle < 0 || edge + thickness >= end ||
                middle >= static_cast<LONG>(vertical() ? width : height)) { return S_OK; }
        HRGN region = CreateRectRgn(0, 0, 0, 0);
        if (!region) { return E_OUTOFMEMORY; }
        if (GetWindowRgn(window_, region) == ERROR) { DeleteObject(region); return E_FAIL; }
        for (LONG offset = 0; offset <= thickness; ++offset) {
            const LONG x = vertical() ? middle : edge + offset;
            const LONG y = vertical() ? edge + offset : middle;
            WICRect sample{x, y, 1, 1};
            std::array<BYTE, 4> pixel{};
            result = window_bitmap_->CopyPixels(&sample, 4, 4, pixel.data());
            const bool wall = offset < thickness;
            if (FAILED(result) || pixel[3] != (wall ? 255 : 0) ||
                    (PtInRegion(region, x, y) != FALSE) != wall ||
                    (wall && tab_at(POINT{x, y}) != active_tab_)) {
                log_.write("Smoke prism frame failed: offset=", std::to_string(offset),
                    "; thickness-px=", std::to_string(thickness), "; alpha=", std::to_string(pixel[3]),
                    "; native-region=", std::to_string(PtInRegion(region, x, y) != FALSE),
                    "; hit=", (tab_at(POINT{x, y}) ? std::to_string(*tab_at(POINT{x, y})) : "none"), ".");
                DeleteObject(region);
                return E_FAIL;
            }
        }
        DeleteObject(region);
        return S_OK;
    }

    HRESULT verify_thickness_dialog()
    {
        const auto original = runtime_;
        unsigned commits = 0;
        bool saw_fraction = false;
        bool saw_tenth_step = false;
        bool saw_zero = false;
        bool saw_reset = false;
        bool saw_preset_position = false;
        bool stored_preset = false;
        unsigned preset_loads = 0;
        std::vector<launcher::AdjustmentPreset> presets{
            {"Empty", std::nullopt}, {"Normal", launcher::RuntimeAdjustments{1.5, 1.25, 1, 0.15, 7U, 1.6}}};
        bool failed = false;
        HWND test_dialog = nullptr;
        launcher::AdjustmentCallbacks callbacks;
        callbacks.preview = [&](const launcher::RuntimeAdjustments& candidate, std::wstring& error) {
            if (!rebuild_view(candidate, active_tab_, error)) { failed = true; return false; }
            if (FAILED(verify_prism_frame()) || FAILED(verify_tab_metrics())) {
                failed = true;
                error = L"The preview crowds or clips the tab glyphs.";
                return false;
            }
            const double value = candidate.thickness_dip.value_or(-1);
            const double physical = sheet_thickness_dip() * fit_scale_ * static_cast<float>(dpi_) / 96.0F;
            if (std::abs(physical - std::round(value * static_cast<double>(dpi_) / 96.0)) > 0.001) {
                failed = true;
            }
            saw_fraction |= value == 1.6 && candidate.icon_scale == 1.25;
            saw_tenth_step |= value == 1.7;
            saw_zero |= value == 0;
            saw_reset |= value == configuration_.appearance.thickness_dip && candidate.icon_scale == 1;
            saw_preset_position |= candidate.edge_position == 0.15;
            return true;
        };
        callbacks.commit = [&](const launcher::RuntimeAdjustments& candidate, std::wstring&) {
            if (candidate.thickness_dip != 1.6 || candidate.icon_scale != 1.25) { failed = true; }
            ++commits;
            return true; // Exercise Apply without writing any fixture or personal settings.
        };
        callbacks.describe = [this, &failed, &test_dialog] {
            const auto description = adjustment_description();
            const auto expected = L"drawn: " + std::to_wstring(sheet_thickness_pixels()) + L" px at " +
                std::to_wstring(std::lround(static_cast<double>(dpi_) * 100.0 / 96.0)) + L"%";
            if (description.find(expected) == std::wstring::npos) { failed = true; }
            if (test_dialog) {
                const HWND label = GetDlgItem(test_dialog, IDC_EFFECTIVE_SIZES);
                RECT available{};
                GetClientRect(label, &available);
                RECT measured{0, 0, available.right, 0};
                const HDC dc = GetDC(label);
                if (!dc) { failed = true; return description; }
                const auto font = reinterpret_cast<HFONT>(SendMessageW(label, WM_GETFONT, 0, 0));
                const auto previous = SelectObject(dc, font);
                DrawTextW(dc, description.c_str(), static_cast<int>(description.size()), &measured,
                    DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                SelectObject(dc, previous);
                ReleaseDC(label, dc);
                if (measured.bottom > available.bottom) { failed = true; }
                int minimum = -1;
                int maximum = -1;
                SendDlgItemMessageW(test_dialog, IDC_THICKNESS_SPIN, UDM_GETRANGE32,
                    reinterpret_cast<WPARAM>(&minimum), reinterpret_cast<LPARAM>(&maximum));
                if (minimum != 0 || maximum != 160) { failed = true; }
            }
            return description;
        };
        callbacks.default_icon_spacing_px = [this] { return configuration_.grid.icon_spacing_px.value_or(8); };
        callbacks.default_thickness_dip = [this] { return configuration_.appearance.thickness_dip; };
        callbacks.load_presets = [&] { ++preset_loads; return presets; };
        callbacks.store_preset = [&](std::string_view name, const launcher::RuntimeAdjustments& candidate, std::wstring&) {
            if (name != "Empty" || candidate.text_scale != 1.5 || candidate.icon_scale != 1.25 ||
                candidate.icon_spacing_px != 7U || candidate.thickness_dip != 0.6 || candidate.edge_position != 0.15) {
                failed = true;
            }
            presets[0].values = candidate;
            stored_preset = true;
            return true;
        };
        callbacks.dialog_window = [&](HWND dialog) {
            test_dialog = dialog;
            if (!dialog) { return; }
            if (!SetTimer(dialog, 1, 1, [](HWND window, UINT, UINT_PTR timer, DWORD) {
                    KillTimer(window, timer);
                    SetDlgItemTextW(window, IDC_THICKNESS, L"1.6");
                    NMUPDOWN step{};
                    step.hdr.hwndFrom = GetDlgItem(window, IDC_THICKNESS_SPIN);
                    step.hdr.idFrom = IDC_THICKNESS_SPIN;
                    step.hdr.code = UDN_DELTAPOS;
                    step.iDelta = 1;
                    SendMessageW(window, WM_NOTIFY, IDC_THICKNESS_SPIN, reinterpret_cast<LPARAM>(&step));
                    step.iDelta = -1;
                    SendMessageW(window, WM_NOTIFY, IDC_THICKNESS_SPIN, reinterpret_cast<LPARAM>(&step));
                    SetDlgItemTextW(window, IDC_ICON_SCALE, L"125");
                    SendMessageW(window, WM_COMMAND, IDC_APPLY_ADJUSTMENTS, 0);
                    SetDlgItemTextW(window, IDC_THICKNESS, L"0");
                    SendMessageW(window, WM_COMMAND, IDC_RESET_SIZES, 0);
                    SetDlgItemTextW(window, IDC_THICKNESS, L"0.4");
                    // An empty Recall remains in the picker; Escape dismisses only that picker.
                    SendMessageW(window, WM_COMMAND, IDC_RECALL_PRESET, 0);
                    SendDlgItemMessageW(window, IDC_PRESET_LIST, LB_SETCURSEL, 0, 0);
                    SendMessageW(window, WM_COMMAND, IDOK, 0);
                    SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
                    SendMessageW(window, WM_COMMAND, IDC_RECALL_PRESET, 0);
                    SendDlgItemMessageW(window, IDC_PRESET_LIST, LB_SETCURSEL, 1, 0);
                    SendMessageW(window, WM_COMMAND, IDOK, 0);
                    SetDlgItemTextW(window, IDC_THICKNESS, L"0.6");
                    SendMessageW(window, WM_COMMAND, IDC_STORE_PRESET, 0);
                    SendDlgItemMessageW(window, IDC_PRESET_LIST, LB_SETCURSEL, 0, 0);
                    SendMessageW(window, WM_COMMAND, IDOK, 0);
                    SendMessageW(window, WM_COMMAND, IDC_RECALL_PRESET, 0);
                    SendDlgItemMessageW(window, IDC_PRESET_LIST, LB_SETCURSEL, 0, 0);
                    SendMessageW(window, WM_COMMAND, IDOK, 0);
                    SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
                })) {
                failed = true;
                PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
            }
        };
        const auto result = launcher::show_adjustments_dialog(instance_, window_, original, callbacks);
        const bool restored_apply = runtime_.thickness_dip == 1.6 && runtime_.icon_scale == 1.25 &&
            runtime_.edge_position == original.edge_position && runtime_.text_scale == original.text_scale;
        std::wstring error;
        const bool restored_original = rebuild_view(original, active_tab_, error);
        if (result != IDCANCEL || failed || commits != 1 || !saw_fraction || !saw_tenth_step || !saw_zero || !saw_reset ||
                !restored_apply || !restored_original || !saw_preset_position || !stored_preset || preset_loads != 4) {
            log_.write("Smoke thickness/presets dialog failed: preview, Reset, Apply, Cancel, Store, Recall, or position rollback.");
            return E_FAIL;
        }
        log_.write("Smoke thickness/presets dialog passed: fractional arrows/preview, pixel snapping, Reset/Apply/Cancel, empty slots, picker Escape/Enter, Store without Apply, Recall with position, and Cancel rollback.");
        return S_OK;
    }

    HRESULT verify_surface_cache()
    {
        const auto builds = surface_builds_;
        const HRESULT result = render();
        if (FAILED(result)) { return result; }
        if (surface_builds_ != builds) {
            log_.write("Smoke surface cache failed: an unchanged repaint regenerated a surface.");
            return E_FAIL;
        }
        log_.write("Smoke surface cache passed: surfaces=", std::to_string(surfaces_.size()),
            "; unchanged repaint reused all surfaces; initial-generation-ms=", std::to_string(surface_build_elapsed_ms_), ".");
        return S_OK;
    }

    void update_window_icons()
    {
        const auto load = [this](int width_metric, int height_metric) {
            return static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_LAUNCHER), IMAGE_ICON,
                GetSystemMetricsForDpi(width_metric, dpi_), GetSystemMetricsForDpi(height_metric, dpi_),
                LR_DEFAULTCOLOR));
        };
        // LR_SHARED caches by resource name and can return the wrong size for
        // a later request. Load two owned handles to retain the optical variants.
        const HICON large_icon = load(SM_CXICON, SM_CYICON);
        const HICON small_icon = load(SM_CXSMICON, SM_CYSMICON);
        if (large_icon == nullptr || small_icon == nullptr) {
            if (large_icon != nullptr) { DestroyIcon(large_icon); }
            if (small_icon != nullptr) { DestroyIcon(small_icon); }
            throw std::runtime_error("Cannot load the Launcher window icons.");
        }
        SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large_icon));
        SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
        const HICON old_large = std::exchange(large_window_icon_, large_icon);
        const HICON old_small = std::exchange(small_window_icon_, small_icon);
        if (old_large != nullptr) { DestroyIcon(old_large); }
        if (old_small != nullptr) { DestroyIcon(old_small); }
    }

    HRESULT verify_window_icons()
    {
        const auto module = GetModuleHandleW(nullptr);
        const auto ding = FindResourceW(module, MAKEINTRESOURCEW(IDW_NO_ACTION_DING), L"WAVE");
        if (!ding || SizeofResource(module, ding) <= 44) {
            log_.write("Smoke application resources failed: embedded no-action ding is missing.");
            return E_FAIL;
        }
        for (const bool use_small : {true, false}) {
            const auto icon = reinterpret_cast<HICON>(SendMessageW(window_, WM_GETICON,
                use_small ? ICON_SMALL : ICON_BIG, 0));
            ICONINFO information{};
            if (icon == nullptr || !GetIconInfo(icon, &information)) { return E_FAIL; }
            BITMAP bitmap{};
            const bool valid = information.hbmColor != nullptr &&
                GetObjectW(information.hbmColor, static_cast<int>(sizeof(bitmap)), &bitmap) == static_cast<int>(sizeof(bitmap)) &&
                bitmap.bmWidth == GetSystemMetricsForDpi(use_small ? SM_CXSMICON : SM_CXICON, dpi_) &&
                bitmap.bmHeight == GetSystemMetricsForDpi(use_small ? SM_CYSMICON : SM_CYICON, dpi_);
            if (information.hbmColor != nullptr) { DeleteObject(information.hbmColor); }
            if (information.hbmMask != nullptr) { DeleteObject(information.hbmMask); }
            if (!valid) {
                log_.write("Smoke application icon failed: resource dimensions differ from the window DPI metrics.");
                return E_FAIL;
            }
        }
        log_.write("Smoke application icons passed: embedded small/large icons match the current window DPI.");
        return S_OK;
    }

    struct Diagnostics final {
        std::uint64_t paints = 0;
        std::uint64_t paint_failures = 0;
        std::uint64_t mouse_moves = 0;
        std::uint64_t mouse_leaves = 0;
        std::uint64_t timers = 0;
        std::uint64_t timer_starts = 0;
        std::uint64_t sizes = 0;
        std::uint64_t dpi_changes = 0;
        std::uint64_t reconfigurations = 0;
        std::uint64_t state_applications = 0;
        std::uint64_t winevents = 0;
        std::uint64_t foreground_events = 0;
        std::uint64_t location_events = 0;
        std::uint64_t show_events = 0;
        std::uint64_t hide_events = 0;
        std::uint64_t destroy_events = 0;
        std::uint64_t reorder_events = 0;
        std::uint64_t fullscreen_filtered = 0;
        std::uint64_t fullscreen_coalesced = 0;
        std::uint64_t fullscreen_messages = 0;
        std::uint64_t fullscreen_scans = 0;
        std::uint64_t windows_visited = 0;
    };

    void report_diagnostics(bool closing = false) noexcept
    {
        if (!diagnostics_enabled_ ||
            (closing ? diagnostics_final_reported_ : diagnostics_reported_)) {
            return;
        }
        if (closing) {
            diagnostics_final_reported_ = true;
        } else {
            diagnostics_reported_ = true;
        }
        KillTimer(window_, kDiagnosticTimer);
        try {
            log_.write("UI diagnostic after-ms=", std::to_string(GetTickCount64() - diagnostics_started_),
                "; final=", (closing ? "yes" : "no"),
                "; paints=", std::to_string(diagnostics_.paints),
                "; paint-failures=", std::to_string(diagnostics_.paint_failures),
                "; mouse-moves=", std::to_string(diagnostics_.mouse_moves),
                "; mouse-leaves=", std::to_string(diagnostics_.mouse_leaves),
                "; timers=", std::to_string(diagnostics_.timers),
                "; timer-starts=", std::to_string(diagnostics_.timer_starts),
                "; sizes=", std::to_string(diagnostics_.sizes),
                "; dpi=", std::to_string(diagnostics_.dpi_changes),
                "; reconfigure=", std::to_string(diagnostics_.reconfigurations),
                "; state-applications=", std::to_string(diagnostics_.state_applications),
                "; winevents=", std::to_string(diagnostics_.winevents),
                "; foreground=", std::to_string(diagnostics_.foreground_events),
                "; location=", std::to_string(diagnostics_.location_events),
                "; show=", std::to_string(diagnostics_.show_events),
                "; hide=", std::to_string(diagnostics_.hide_events),
                "; destroy=", std::to_string(diagnostics_.destroy_events),
                "; reorder=", std::to_string(diagnostics_.reorder_events),
                "; fullscreen-filtered=", std::to_string(diagnostics_.fullscreen_filtered),
                "; fullscreen-coalesced=", std::to_string(diagnostics_.fullscreen_coalesced),
                "; fullscreen-messages=", std::to_string(diagnostics_.fullscreen_messages),
                "; fullscreen-scans=", std::to_string(diagnostics_.fullscreen_scans),
                "; windows-visited=", std::to_string(diagnostics_.windows_visited), ".");
        } catch (...) {
        }
    }

    bool verify_native_state(launcher::DockState expected)
    {
        if (peek_.state() != expected) {
            log_.write("Smoke test failed: unexpected dock controller state.");
            return false;
        }
        if (expected == launcher::DockState::suppressed) {
            return !IsWindowVisible(window_);
        }
        RECT actual{};
        if (!GetWindowRect(window_, &actual)) {
            return false;
        }
        const auto desired = native_destination(expected);
        if (pixel_rect(actual) != desired ||
            MonitorFromWindow(window_, MONITOR_DEFAULTTONULL) != monitor_.handle ||
            GetDpiForWindow(window_) != dpi_ ||
            GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(window_)) !=
                DPI_AWARENESS_PER_MONITOR_AWARE) {
            log_.write("Smoke test failed: native position, monitor, DPI, or awareness mismatch.");
            return false;
        }
        HRGN region = CreateRectRgn(0, 0, 0, 0);
        if (region == nullptr) {
            return false;
        }
        RECT clipped{};
        const int kind = GetWindowRgn(window_, region);
        const int box_kind = GetRgnBox(region, &clipped);
        DeleteObject(region);
        if (kind == ERROR || kind == NULLREGION || box_kind == ERROR || box_kind == NULLREGION) {
            return false;
        }
        OffsetRect(&clipped, actual.left, actual.top);
        const RECT work = monitor_.information.rcWork;
        const bool inside = clipped.left >= work.left && clipped.top >= work.top &&
            clipped.right <= work.right && clipped.bottom <= work.bottom;
        const bool compact = expected != launcher::DockState::collapsed ||
            (clipped.left >= desired.left && clipped.top >= desired.top &&
                clipped.right <= desired.right && clipped.bottom <= desired.bottom);
        if (!inside || !compact) {
            log_.write("Smoke test failed: visible dock region leaks outside selected work area.");
        }
        return inside && compact;
    }

    bool vertical() const noexcept
    {
        return configuration_.dock.edge == launcher::Edge::left ||
            configuration_.dock.edge == launcher::Edge::right;
    }

    bool tabs_above_page() const noexcept
    {
        return configuration_.dock.edge == launcher::Edge::bottom;
    }

    bool reverse_tab_text() const noexcept
    {
        if (!vertical() || configuration_.appearance.tab_text_direction == launcher::TabTextDirection::automatic) { return false; }
        const bool top_down = configuration_.appearance.tab_text_direction == launcher::TabTextDirection::top_to_bottom;
        return top_down != (configuration_.dock.edge == launcher::Edge::right);
    }

    D2D1::Matrix3x2F tab_text_transform(D2D1_RECT_F head) const noexcept
    {
        return reverse_tab_text() ? D2D1::Matrix3x2F::Rotation(180.0F,
            D2D1::Point2F((head.left + head.right) * 0.5F, (head.top + head.bottom) * 0.5F))
            : D2D1::Matrix3x2F::Identity();
    }

    D2D1_POINT_2F prism_view_direction() const noexcept
    {
        const bool reverse = configuration_.appearance.reverse_thickness;
        return D2D1::Point2F(reverse && vertical() ? -1.0F : 1.0F,
            reverse && !vertical() ? -1.0F : 1.0F);
    }

    D2D1_POINT_2F view_to_canvas_vector(float x, float y) const noexcept
    {
        const auto orientation = canvas_to_view_transform();
        return D2D1::Point2F(x * orientation._11 + y * orientation._12,
            x * orientation._21 + y * orientation._22);
    }

    // Coordinate spaces are distinct from units:
    // - Page grid: configured (column, row) indices; flow is defined here.
    // - Layout DIPs: parallel to the dock edge, depth from tabs into the page.
    // - Canvas DIPs: upright drawing axes used by tab shapes and text.
    // - View DIPs: oriented full view, before fitting and collapsed clipping.
    // - Client pixels: native window-local coordinates after the viewport.
    // - Screen pixels: desktop coordinates, converted with ClientToScreen.
    D2D1_POINT_2F layout_to_canvas_dip_point(float parallel, float depth) const noexcept
    {
        const float total_depth = panel_depth_dip_ + tab_depth_dip_;
        return tabs_above_page()
            ? D2D1::Point2F(parallel, depth)
            : D2D1::Point2F(parallel, total_depth - depth);
    }

    D2D1::Matrix3x2F canvas_to_view_transform() const noexcept
    {
        const float total_depth = panel_depth_dip_ + tab_depth_dip_;
        switch (configuration_.dock.edge) {
        case launcher::Edge::right:
            return D2D1::Matrix3x2F(0.0F, 1.0F, -1.0F, 0.0F, total_depth, 0.0F);
        case launcher::Edge::left:
            return D2D1::Matrix3x2F(0.0F, -1.0F, 1.0F, 0.0F, 0.0F, parallel_span_dip_);
        case launcher::Edge::top:
        case launcher::Edge::bottom:
            return D2D1::Matrix3x2F::Identity();
        }
        return D2D1::Matrix3x2F::Identity();
    }

    D2D1_POINT_2F layout_to_view_dip_point(float parallel, float depth) const noexcept
    {
        return canvas_to_view_transform().TransformPoint(layout_to_canvas_dip_point(parallel, depth));
    }

    D2D1_RECT_F layout_to_canvas_dip_rect(D2D1_RECT_F logical) const noexcept
    {
        const auto first = layout_to_canvas_dip_point(logical.left, logical.top);
        const auto last = layout_to_canvas_dip_point(logical.right, logical.bottom);
        return D2D1::RectF(std::min(first.x, last.x), std::min(first.y, last.y),
            std::max(first.x, last.x), std::max(first.y, last.y));
    }

    D2D1_RECT_F layout_to_view_dip_rect(D2D1_RECT_F logical) const noexcept
    {
        const auto first = layout_to_view_dip_point(logical.left, logical.top);
        const auto last = layout_to_view_dip_point(logical.right, logical.bottom);
        return D2D1::RectF(std::min(first.x, last.x), std::min(first.y, last.y),
            std::max(first.x, last.x), std::max(first.y, last.y));
    }

    std::array<D2D1_POINT_2F, 6> tab_canvas_dip_points(const TabVisual& tab) const noexcept
    {
        const auto& bounds = tab.logical_bounds;
        const float slope = std::min(tab_slope_dip_, (bounds.right - bounds.left) * 0.5F);
        return {{layout_to_canvas_dip_point(bounds.left + slope, bounds.top),
            layout_to_canvas_dip_point(bounds.right - slope, bounds.top),
            layout_to_canvas_dip_point(bounds.right, bounds.bottom),
            layout_to_canvas_dip_point(bounds.right, tab_depth_dip_),
            layout_to_canvas_dip_point(bounds.left, tab_depth_dip_),
            layout_to_canvas_dip_point(bounds.left, bounds.bottom)}};
    }

    D2D1_RECT_F grid_cell_view_dip_bounds(std::size_t page_column, std::size_t page_row) const noexcept
    {
        // A page row appears as a view column on vertical edges. Reflect the
        // Left grid along both layout axes: row zero is the leftmost view column,
        // and its cells fill downward. Tab/text orientation stays independent.
        const bool reflect_left_grid = configuration_.dock.edge == launcher::Edge::left;
        const auto layout_column = reflect_left_grid
            ? layout_.columns - 1 - page_column : page_column;
        const auto layout_row = reflect_left_grid
            ? layout_.rows - 1 - page_row : page_row;
        const float step = drawn_icon_size_dip_ + icon_gap_dip_;
        const float left = icon_gap_dip_ + static_cast<float>(layout_column) * step;
        const float top = tab_depth_dip_ + icon_gap_dip_ + static_cast<float>(layout_row) * step;
        return layout_to_view_dip_rect(D2D1::RectF(left, top,
            left + drawn_icon_size_dip_, top + drawn_icon_size_dip_));
    }

    HRESULT verify_grid_spacing()
    {
        const double to_pixels = static_cast<double>(dpi_) * fit_scale_ / 96.0;
        const double expected_gap = effective_.icon_spacing_px;
        const auto cell = [&](std::size_t column, std::size_t row) {
            return grid_cell_view_dip_bounds(column, row);
        };
        const auto grid = layout_to_view_dip_rect(D2D1::RectF(icon_gap_dip_, tab_depth_dip_ + icon_gap_dip_,
            parallel_span_dip_ - icon_gap_dip_,
            tab_depth_dip_ + panel_depth_dip_ - icon_gap_dip_));
        double maximum_error = 0.0;
        const auto check_gap = [&](double gap) {
            maximum_error = std::max(maximum_error, std::abs(gap * to_pixels - expected_gap));
        };
        check_gap(grid.left - panel_bounds_.left);
        check_gap(panel_bounds_.right - grid.right);
        check_gap(grid.top - panel_bounds_.top);
        check_gap(panel_bounds_.bottom - grid.bottom);
        const auto first = cell(0, 0);
        if (layout_.columns > 1) {
            const auto next = cell(1, 0);
            check_gap(vertical() ? std::max(next.top - first.bottom, first.top - next.bottom)
                : std::max(next.left - first.right, first.left - next.right));
        }
        if (layout_.rows > 1) {
            const auto next = cell(0, 1);
            check_gap(vertical() ? std::max(next.left - first.right, first.left - next.right)
                : std::max(next.top - first.bottom, first.top - next.bottom));
        }
        const auto check_bounds = [&](D2D1_RECT_F bounds) {
            for (const float value : {bounds.left, bounds.top, bounds.right, bounds.bottom}) {
                const double actual = value * to_pixels;
                maximum_error = std::max(maximum_error, std::abs(actual - std::round(actual)));
            }
        };
        check_bounds(panel_bounds_);
        std::size_t icon_index = 0;
        if (!configuration_.tabs.empty()) {
            for (const auto& placement : layout_.tabs[active_tab_].placements) {
                if (configuration_.tabs[active_tab_].items.at(placement.item_index).kind != launcher::ItemKind::icon) {
                    continue;
                }
                if (icon_index >= icons_.size()) { return E_FAIL; }
                const auto expected = cell(placement.column, placement.row);
                const auto actual = icons_[icon_index++].bounds;
                const std::array<double, 4> differences{{actual.left - expected.left,
                    actual.top - expected.top, actual.right - expected.right,
                    actual.bottom - expected.bottom}};
                for (const auto difference : differences) {
                    maximum_error = std::max(maximum_error, std::abs(difference * to_pixels));
                }
                check_bounds(actual);
            }
        }
        log_.write("Smoke pixel spacing: gap-px=", std::to_string(effective_.icon_spacing_px),
            "; dpi=", std::to_string(dpi_), "; fit-scale=", std::to_string(fit_scale_),
            "; maximum-error-px=", std::to_string(maximum_error), ".");
        if (maximum_error > 0.01 || icon_index != icons_.size()) {
            log_.write("Smoke pixel spacing failed: cell/edge gaps or native pixel alignment differ.");
            return E_FAIL;
        }
        return S_OK;
    }

    HRESULT verify_grid_screen_order()
    {
        if (!vertical()) { return S_OK; }
        const double to_pixels = static_cast<double>(dpi_) * fit_scale_ / 96.0;
        const float step = drawn_icon_size_dip_ + icon_gap_dip_;
        const auto& items = configuration_.tabs[active_tab_].items;
        std::vector<std::optional<std::pair<std::size_t, float>>> previous(layout_.rows);
        std::size_t count = 0;
        for (const auto& placement : layout_.tabs[active_tab_].placements) {
            // Spacers occupy their grid slot but have no hit target or tooltip.
            if (items.at(placement.item_index).kind != launcher::ItemKind::icon) { continue; }
            const auto icon = std::find_if(icons_.begin(), icons_.end(), [&](const IconVisual& entry) {
                return entry.item_index == placement.item_index;
            });
            if (icon == icons_.end() || placement.row >= previous.size()) { return E_FAIL; }
            const float center_x = (icon->bounds.left + icon->bounds.right) * 0.5F;
            const float center_y = (icon->bounds.top + icon->bounds.bottom) * 0.5F;
            // This expected position uses screen geometry directly. It does not
            // reuse the production rotation or mirrored-cell mapping helper.
            const float expected_y = panel_bounds_.top + icon_gap_dip_ + drawn_icon_size_dip_ * 0.5F +
                static_cast<float>(placement.column) * step;
            if (std::abs(static_cast<double>(center_y - expected_y) * to_pixels) > 0.01) {
                log_.write("Smoke vertical grid order failed: logical column did not occupy its top-down screen slot.");
                return E_FAIL;
            }
            const float expected_x = panel_bounds_.left + icon_gap_dip_ + drawn_icon_size_dip_ * 0.5F +
                static_cast<float>(placement.row) * step;
            if (std::abs(static_cast<double>(center_x - expected_x) * to_pixels) > 0.01) {
                log_.write("Smoke vertical grid order failed: logical row did not occupy its left-to-right screen slot.");
                return E_FAIL;
            }
            auto& preceding = previous[placement.row];
            if (preceding && placement.column > preceding->first && center_y <= preceding->second) {
                log_.write("Smoke vertical grid order failed: consecutive row icons run upward.");
                return E_FAIL;
            }
            preceding = std::pair{placement.column, center_y};
            if (peek_.state() == launcher::DockState::expanded) {
                const POINT center{pixels(center_x * fit_scale_, dpi_) - render_offset_px_.x,
                    pixels(center_y * fit_scale_, dpi_) - render_offset_px_.y};
                const auto hit = icon_at(center);
                if (!hit || icons_[*hit].item_index != placement.item_index) {
                    log_.write("Smoke vertical grid order failed: native icon center selected a different item.");
                    return E_FAIL;
                }
            }
            ++count;
        }
        if (count != icons_.size()) { return E_FAIL; }
        log_.write("Smoke vertical grid order: edge=", edge_name(configuration_.dock.edge),
            "; icons=", std::to_string(count), "; top-down, left-to-right slots, partial-row anchoring and item hit targets verified.");
        return S_OK;
    }

    HRESULT verify_font_axes()
    {
        ComPtr<IDWriteTextFormat3> format;
        if (automatic_optical_size_ && (FAILED(text_format_.As(&format)) ||
                (format->GetAutomaticFontAxes() & DWRITE_AUTOMATIC_FONT_AXES_OPTICAL_SIZE) == 0)) {
            log_.write("Smoke font axes failed: automatic optical size was not retained.");
            return E_FAIL;
        }
        ComPtr<IDWriteTextLayout> text;
        HRESULT result = write_factory_->CreateTextLayout(L"General", 7,
            text_format_.Get(), 4096.0F, 4096.0F, text.GetAddressOf());
        if (FAILED(result)) { return result; }
        if (automatic_optical_size_) {
            ComPtr<IDWriteTextLayout4> axis_layout;
            if (FAILED(text.As(&axis_layout)) ||
                    (axis_layout->GetAutomaticFontAxes() & DWRITE_AUTOMATIC_FONT_AXES_OPTICAL_SIZE) == 0) {
                log_.write("Smoke font axes failed: automatic optical size did not reach text layout.");
                return E_FAIL;
            }
        }
        ComPtr<FontAxisInspector> inspector;
        inspector.Attach(new FontAxisInspector());
        result = text->Draw(nullptr, inspector.Get(), 0.0F, 0.0F);
        if (FAILED(result)) { return result; }
        const float points = effective_.text_size_dip * 0.75F;
        log_.write("Smoke font axes: family=", utf8(resolved_font_family_),
            "; point-size=", std::to_string(points), "; automatic-optical-size=",
            (automatic_optical_size_ ? "enabled" : "unavailable"),
            "; actual-opsz=", (inspector->optical_size
                ? std::to_string(*inspector->optical_size) : "none"),
            "; actual-wght=", (inspector->weight ? std::to_string(*inspector->weight) : "none"), ".");
        if (automatic_optical_size_ && typographic_font_family_ &&
                _wcsicmp(resolved_font_family_.c_str(), L"Segoe UI Variable") == 0) {
            if (!inspector->optical_size || !inspector->optical_range ||
                    !inspector->weight || !inspector->weight_range) {
                log_.write("Smoke font axes failed: Segoe UI Variable did not expose its actual axes.");
                return E_FAIL;
            }
            const float expected_optical = std::clamp(points,
                inspector->optical_range->minValue, inspector->optical_range->maxValue);
            const float expected_weight = std::clamp(static_cast<float>(effective_.font_weight),
                inspector->weight_range->minValue, inspector->weight_range->maxValue);
            if (std::abs(*inspector->optical_size - expected_optical) > 0.01F ||
                    std::abs(*inspector->weight - expected_weight) > 0.01F) {
                log_.write("Smoke font axes failed: actual optical size or weight differs from the request.");
                return E_FAIL;
            }
        }
        return S_OK;
    }

    float tab_extra_baseline_padding_dip() const noexcept
    {
        if (configuration_.dock.edge == launcher::Edge::left) {
            // Keep the extra room at one physical pixel after DPI and fitting.
            return 96.0F / (static_cast<float>(dpi_) * fit_scale_);
        }
        return tabs_above_page()
            ? kTabExtraBaselinePaddingEm * effective_.text_size_dip : 0.0F;
    }

    D2D1_RECT_F tab_label_bounds(D2D1_RECT_F bounds) const noexcept
    {
        const float parallel_padding = std::min(tab_label_padding_dip_,
            (bounds.right - bounds.left) * 0.25F);
        return D2D1::RectF(bounds.left + parallel_padding, bounds.top,
            bounds.right - parallel_padding, bounds.bottom - tab_extra_baseline_padding_dip());
    }

    float sheet_thickness_dip() const noexcept
    {
        // Thickness follows monitor DPI only. Undo the common fit transform so
        // a change in icon/panel size cannot change the final physical width.
        const double density = static_cast<double>(dpi_) / 96.0;
        return static_cast<float>(sheet_thickness_pixels() / (density * fit_scale_));
    }

    long sheet_thickness_pixels() const noexcept
    {
        return std::lround(effective_.thickness_dip * static_cast<double>(dpi_) / 96.0);
    }

    int prism_frame_padding_px() const noexcept
    {
        return static_cast<int>(sheet_thickness_pixels()) * 4;
    }

    launcher::PixelRect native_destination(launcher::DockState state) const noexcept
    {
        return launcher::dock_viewport(state == launcher::DockState::collapsed ? placement_.collapsed : placement_.expanded,
            pixel_rect(monitor_.information.rcWork), prism_frame_padding_px());
    }

    float prism_outer_wall_dip() const noexcept
    {
        // Reserve the wall's projection toward the outer tab rows, including
        // the reversed view. Text direction is independent of this geometry.
        const auto orientation = canvas_to_view_transform();
        const auto direction = prism_view_direction();
        const float outward = tabs_above_page() ? -1.0F : 1.0F;
        return sheet_thickness_dip() * std::max(0.0F, outward *
            (direction.x * orientation._21 + direction.y * orientation._22));
    }

    HRESULT verify_tab_metrics()
    {
        if (tabs_.empty()) { return E_FAIL; }
        float line_height = effective_.text_size_dip;
        for (const auto& tab : tabs_) {
            ComPtr<IDWriteTextLayout> text;
            HRESULT result = write_factory_->CreateTextLayout(tab.label.c_str(),
                static_cast<UINT32>(tab.label.size()), text_format_.Get(),
                4096.0F, 4096.0F, text.GetAddressOf());
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(result)) { result = text->GetMetrics(&metrics); }
            if (FAILED(result)) { return result; }
            line_height = std::max(line_height, metrics.height);
        }
        const float baseline_inset = tab_extra_baseline_padding_dip();
        const float padding = kTabVerticalPaddingEm * effective_.text_size_dip + baseline_inset;
        const float nominal_depth = line_height + padding;
        const float active_rise = kTabActiveRiseEm * effective_.text_size_dip;
        const double to_pixels = static_cast<double>(dpi_) * fit_scale_ / 96.0;
        // Only the panel-facing row gains the fractional pixel needed to
        // align the strip boundary. Every label must retain its line box and
        // font-relative breathing room through DPI changes and grid fitting.
        const float snap_allowance = 1.0F / static_cast<float>(to_pixels);
        float minimum_cap_gap = std::numeric_limits<float>::infinity();
        float minimum_descender_gap = std::numeric_limits<float>::infinity();
        for (std::size_t index = 0; index < tabs_.size(); ++index) {
            const auto& tab = tabs_[index];
            const float depth = tab.logical_bounds.bottom - tab.logical_bounds.top;
            const float expected_depth = nominal_depth + (index == active_tab_ ? active_rise : 0.0F);
            if (depth + 0.001F < expected_depth ||
                    depth > expected_depth + snap_allowance + 0.001F || tab.logical_bounds.top < -0.001F) {
                log_.write("Smoke tab metrics failed: line height and font-relative padding do not fit the tab.");
                return E_FAIL;
            }
            const auto head = layout_to_canvas_dip_rect(tab.logical_bounds);
            const auto label = tab_label_bounds(head);
            ComPtr<IDWriteTextLayout> bounded_text;
            HRESULT text_result = write_factory_->CreateTextLayout(tab.label.c_str(), static_cast<UINT32>(tab.label.size()),
                text_format_.Get(), label.right - label.left, label.bottom - label.top, bounded_text.GetAddressOf());
            DWRITE_LINE_METRICS line{};
            UINT32 line_count = 0;
            if (SUCCEEDED(text_result)) { text_result = bounded_text->GetLineMetrics(&line, 1, &line_count); }
            if (FAILED(text_result) || line_count != 1) { return FAILED(text_result) ? text_result : E_FAIL; }
            ComPtr<IDWriteTextLayout> natural_text;
            text_result = write_factory_->CreateTextLayout(tab.label.c_str(), static_cast<UINT32>(tab.label.size()),
                text_format_.Get(), 100000.0F, 4096.0F, natural_text.GetAddressOf());
            DWRITE_TEXT_METRICS natural_metrics{};
            if (SUCCEEDED(text_result)) { text_result = natural_text->GetMetrics(&natural_metrics); }
            if (FAILED(text_result)) { return text_result; }
            const bool needs_ellipsis = natural_metrics.widthIncludingTrailingWhitespace > label.right - label.left + 0.01F;
            if (needs_ellipsis && !line.isTrimmed) {
                log_.write("Smoke tab metrics failed: an overlong label did not acquire an ellipsis.");
                return E_FAIL;
            }
            if (!needs_ellipsis && line.isTrimmed) {
                log_.write("Smoke tab metrics failed: a fitting label was unnecessarily truncated.");
                return E_FAIL;
            }
            if (line.isTrimmed) { log_.write("Smoke tab label: ellipsis applied to ", utf8(tab.label), "."); }
            // Check the actual glyph ink, including descenders and the
            // ellipsis, against the face left visible by later rows.
            DWRITE_OVERHANG_METRICS ink{};
            text_result = bounded_text->GetOverhangMetrics(&ink);
            if (FAILED(text_result)) { return text_result; }
            const float border = sheet_thickness_dip() > 0.0F ? snap_allowance * 0.5F : 0.0F;
            float visible_top = head.top + border;
            float visible_bottom = head.bottom - border;
            for (const auto& front : tabs_) {
                if (front.row <= tab.row) { continue; }
                const auto front_head = layout_to_canvas_dip_rect(front.logical_bounds);
                if (tabs_above_page()) {
                    visible_bottom = std::min(visible_bottom, front_head.top - prism_outer_wall_dip() - border);
                } else {
                    visible_top = std::max(visible_top, front_head.bottom + prism_outer_wall_dip() + border);
                }
            }
            const float ink_top = label.top - ink.top;
            const float ink_bottom = label.bottom + ink.bottom;
            const float cap_gap = (reverse_tab_text() ? head.top + head.bottom - ink_bottom : ink_top) - visible_top;
            const float descender_gap = visible_bottom - (reverse_tab_text() ? head.top + head.bottom - ink_top : ink_bottom);
            minimum_cap_gap = std::min(minimum_cap_gap, cap_gap);
            minimum_descender_gap = std::min(minimum_descender_gap, descender_gap);
            if (cap_gap < -0.01F || descender_gap < -0.01F) {
                log_.write("Smoke tab ink failed: label=", utf8(tab.label),
                    "; cap-gap-px=", std::to_string(cap_gap * to_pixels),
                    "; descender-gap-px=", std::to_string(descender_gap * to_pixels), ".");
                return E_FAIL;
            }
            const float label_height = label.bottom - label.top;
            const float centered_line_padding = (label_height - line_height) * 0.5F;
            const float bottom_padding = head.bottom - label.bottom + centered_line_padding;
            const float base_padding = kTabVerticalPaddingEm * effective_.text_size_dip;
            if (label_height + 0.001F < line_height + base_padding ||
                    bottom_padding + 0.001F < base_padding * 0.5F + baseline_inset) {
                log_.write("Smoke tab metrics failed: the label line box lacks internal baseline padding.");
                return E_FAIL;
            }
            if (tabs_above_page() || configuration_.dock.edge == launcher::Edge::left) {
                const auto physical_head = layout_to_view_dip_rect(tab.logical_bounds);
                const auto orientation = tab_text_transform(head) * canvas_to_view_transform();
                const auto label_first = orientation.TransformPoint(D2D1::Point2F(label.left, label.top));
                const auto label_last = orientation.TransformPoint(D2D1::Point2F(label.right, label.bottom));
                const float physical_inset = configuration_.dock.edge == launcher::Edge::left
                    ? (reverse_tab_text() ? std::min(label_first.x, label_last.x) - physical_head.left
                        : physical_head.right - std::max(label_first.x, label_last.x))
                    : physical_head.bottom - std::max(label_first.y, label_last.y);
                if (std::abs(physical_inset - baseline_inset) > 0.001F) {
                    log_.write("Smoke tab metrics failed: rotated baseline padding is on the wrong side.");
                    return E_FAIL;
                }
            }
        }
        const auto& first = tabs_.front().logical_bounds;
        log_.write("Smoke tab ink: minimum cap-gap-px=", std::to_string(minimum_cap_gap * to_pixels),
            "; minimum descender-gap-px=", std::to_string(minimum_descender_gap * to_pixels), ".");
        log_.write("Smoke tab metrics: point-size=",
            std::to_string(effective_.text_size_dip * 0.75F),
            "; em-dip=", std::to_string(effective_.text_size_dip),
            "; line-height-dip=", std::to_string(line_height),
            "; vertical-padding-dip=", std::to_string(padding),
            "; baseline-inset-dip=", std::to_string(baseline_inset),
            "; active-rise-dip=", std::to_string(active_rise),
            "; nominal-row-px=", std::to_string(nominal_depth * to_pixels),
            "; first-tab-px=", std::to_string((first.bottom - first.top) * to_pixels),
            "; dpi=", std::to_string(dpi_),
            "; fit-scale=", std::to_string(fit_scale_), ".");
        return S_OK;
    }

    void create_text_format()
    {
        ComPtr<IDWriteFontCollection> classic_collection;
        HRESULT result = write_factory_->GetSystemFontCollection(classic_collection.GetAddressOf());
        if (FAILED(result)) {
            throw std::runtime_error("Cannot read the installed font families.");
        }
        ComPtr<IDWriteFactory6> modern_factory;
        ComPtr<IDWriteFontCollection2> typographic_collection;
        if (SUCCEEDED(write_factory_.As(&modern_factory))) {
            result = modern_factory->GetSystemFontCollection(FALSE,
                DWRITE_FONT_FAMILY_MODEL_TYPOGRAPHIC, typographic_collection.GetAddressOf());
            if (FAILED(result)) {
                log_.result("Typographic font collection unavailable; classic fallback", result);
                typographic_collection.Reset();
            }
        }
        std::vector<std::wstring> candidates{utf16(effective_.font_family)};
        candidates.emplace_back(L"Segoe UI");
        std::wstring selected;
        ComPtr<IDWriteFontCollection> selected_collection;
        bool typographic = false;
        for (const auto& family : candidates) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (typographic_collection) {
                result = typographic_collection->FindFamilyName(family.c_str(), &index, &exists);
                if (SUCCEEDED(result) && exists) {
                    selected = family;
                    selected_collection = typographic_collection;
                    typographic = true;
                    break;
                }
            }
            exists = FALSE;
            result = classic_collection->FindFamilyName(family.c_str(), &index, &exists);
            if (SUCCEEDED(result) && exists) {
                selected = family;
                selected_collection = classic_collection;
                break;
            }
        }
        if (selected.empty()) {
            throw launcher::ConfigError("Neither the configured font nor Segoe UI is installed.");
        }
        ComPtr<IDWriteTextFormat> replacement;
        ComPtr<IDWriteTextFormat3> axis_format;
        if (modern_factory) {
            // Leave opsz unspecified. DirectWrite chooses it for the current
            // font size, including each live text-scale adjustment.
            const std::array<DWRITE_FONT_AXIS_VALUE, 4> axes{{
                {DWRITE_FONT_AXIS_TAG_WEIGHT, static_cast<float>(effective_.font_weight)},
                {DWRITE_FONT_AXIS_TAG_WIDTH, 100.0F},
                {DWRITE_FONT_AXIS_TAG_SLANT, 0.0F},
                {DWRITE_FONT_AXIS_TAG_ITALIC, 0.0F}}};
            result = modern_factory->CreateTextFormat(selected.c_str(), selected_collection.Get(),
                axes.data(), static_cast<UINT32>(axes.size()), effective_.text_size_dip,
                L"en-us", axis_format.GetAddressOf());
            if (SUCCEEDED(result)) {
                replacement = axis_format;
            }
        } else {
            result = write_factory_->CreateTextFormat(selected.c_str(), selected_collection.Get(),
                static_cast<DWRITE_FONT_WEIGHT>(effective_.font_weight), DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, effective_.text_size_dip, L"en-us",
                replacement.GetAddressOf());
            if (SUCCEEDED(result)) {
                // Optional on older Windows 10. Keep the configured classic
                // family and weight even when automatic axes are unavailable.
                replacement.As(&axis_format);
            }
        }
        bool automatic_optical = false;
        if (SUCCEEDED(result) && axis_format) {
            result = axis_format->SetAutomaticFontAxes(DWRITE_AUTOMATIC_FONT_AXES_OPTICAL_SIZE);
            automatic_optical = SUCCEEDED(result) &&
                (axis_format->GetAutomaticFontAxes() & DWRITE_AUTOMATIC_FONT_AXES_OPTICAL_SIZE) != 0;
            if (SUCCEEDED(result) && !automatic_optical) {
                result = E_FAIL;
            }
        }
        if (SUCCEEDED(result)) {
            result = replacement->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (SUCCEEDED(result)) {
            result = replacement->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (SUCCEEDED(result)) {
            result = replacement->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (SUCCEEDED(result)) {
            ComPtr<IDWriteInlineObject> ellipsis;
            result = write_factory_->CreateEllipsisTrimmingSign(replacement.Get(), ellipsis.GetAddressOf());
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            if (SUCCEEDED(result)) { result = replacement->SetTrimming(&trimming, ellipsis.Get()); }
        }
        if (FAILED(result)) {
            throw std::runtime_error("Cannot create the configured tab font.");
        }
        text_format_ = std::move(replacement);
        if (selected != resolved_font_family_ || typographic != typographic_font_family_ ||
                automatic_optical != automatic_optical_size_) {
            log_.write("Tab font family: ", utf8(selected), "; weight=",
                std::to_string(effective_.font_weight), "; collection=",
                (typographic ? "typographic" : "classic"), "; automatic-optical-size=",
                (automatic_optical ? "enabled" : "unavailable"), ".");
        }
        resolved_font_family_ = std::move(selected);
        typographic_font_family_ = typographic;
        automatic_optical_size_ = automatic_optical;
    }

    void calculate_layout(bool strict_fit = false)
    {
        // Native tools retain pointers to the owned labels in icons_. Unregister
        // every tool before releasing or moving the corresponding drawing view.
        clear_icon_tooltips();
        icons_.clear();
        // Derive every tab dimension from the effective font em, so changing
        // the configured points and adjusting text at runtime behave alike.
        const float em = effective_.text_size_dip;
        tab_slope_dip_ = 0.5F * em;
        tab_label_padding_dip_ = 0.75F * em;
        std::vector<float> label_widths;
        float line_height = effective_.text_size_dip;
        for (std::size_t index = 0; index < configuration_.tabs.size(); ++index) {
            const auto& tab = configuration_.tabs[index];
            const auto label = utf16(index == active_tab_ || tab.short_name.empty()
                ? tab.name : tab.short_name);
            ComPtr<IDWriteTextLayout> text;
            HRESULT result = write_factory_->CreateTextLayout(label.c_str(),
                static_cast<UINT32>(label.size()), text_format_.Get(), 4096.0F,
                4096.0F, text.GetAddressOf());
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(result)) {
                result = text->GetMetrics(&metrics);
            }
            if (FAILED(result)) {
                throw std::runtime_error("Cannot measure the tab labels.");
            }
            label_widths.push_back(metrics.widthIncludingTrailingWhitespace);
            line_height = std::max(line_height, metrics.height);
        }
        const float row_overlap = 0.25F * em;
        const float horizontal_overlap = tab_slope_dip_;
        const float active_rise = kTabActiveRiseEm * em;
        const RECT work = monitor_.information.rcWork;
        const float pixel_to_dip = 96.0F / static_cast<float>(dpi_);
        const float available_parallel = static_cast<float>(vertical()
            ? work.bottom - work.top : work.right - work.left) * pixel_to_dip;
        const float available_depth = static_cast<float>(vertical()
            ? work.right - work.left : work.bottom - work.top) * pixel_to_dip;
        const float gap_dip = static_cast<float>(effective_.icon_spacing_px) * pixel_to_dip;
        if ((static_cast<float>(layout_.columns) + 1.0F) * gap_dip +
                static_cast<float>(layout_.columns) * pixel_to_dip > available_parallel ||
            (static_cast<float>(layout_.rows) + 1.0F) * gap_dip +
                static_cast<float>(layout_.rows) * pixel_to_dip >= available_depth) {
            throw launcher::ConfigError("The fixed icon spacing does not fit this display. "
                "Reduce spacing or the grid dimensions.");
        }
        const auto build = [&](float candidate_scale) {
            fit_scale_ = candidate_scale;
            tab_label_padding_dip_ = 0.75F * em + sheet_thickness_dip();
            const float baseline_inset = tab_extra_baseline_padding_dip();
            const float depth = line_height + kTabVerticalPaddingEm * em + baseline_inset;
            // Grow the Left heads below the text, preserving the row pitch and
            // label positions even when multiple rows overlap.
            const float left_inset = configuration_.dock.edge == launcher::Edge::left ? baseline_inset : 0.0F;
            const float wall = prism_outer_wall_dip();
            // Later rows must not spend the earlier row's capital-letter room
            // on a visible side wall. Reserve its projection plus one pixel of
            // separation in the pitch, keeping the font and head depth intact.
            const float edge_clearance = wall > 0.0F ? wall + pixel_to_dip / candidate_scale : 0.0F;
            // Reversing the label puts descenders on the overlapping side.
            // Retain the full line box there instead of borrowing its cap gap.
            const float row_step = depth - (reverse_tab_text() ? 0.0F : row_overlap + left_inset) + edge_clearance;
            tabs_.clear();
            tab_draw_order_.clear();
            // The drawing still uses its common fit transform. Inflate the
            // canonical gap by its inverse, leaving an exact physical-pixel
            // gap after fitting. Snap icon sizes and the panel/tab boundary
            // to physical pixels so every cell edge shares that pixel grid.
            const float rendered_icon_pixels = std::max(1.0F, std::floor(
                effective_.icon_size_dip * candidate_scale / pixel_to_dip + 0.5F));
            drawn_icon_size_dip_ = rendered_icon_pixels * pixel_to_dip / candidate_scale;
            icon_gap_dip_ = gap_dip / candidate_scale;
            parallel_span_dip_ = static_cast<float>(layout_.columns) * drawn_icon_size_dip_ +
                (static_cast<float>(layout_.columns) + 1.0F) * icon_gap_dip_;
            panel_depth_dip_ = static_cast<float>(layout_.rows) * drawn_icon_size_dip_ +
                (static_cast<float>(layout_.rows) + 1.0F) * icon_gap_dip_;
            float parallel = 0.0F;
            std::size_t row = 0;
            for (std::size_t index = 0; index < configuration_.tabs.size(); ++index) {
                const auto& tab = configuration_.tabs[index];
                const std::wstring visible_label = utf16(index == active_tab_ || tab.short_name.empty()
                    ? tab.name : tab.short_name);
                const float tab_span = std::min(parallel_span_dip_, std::max(3.75F * em,
                    label_widths[index] + 2.0F * tab_label_padding_dip_ + pixel_to_dip / candidate_scale));
                if (parallel > 0.0F && parallel + tab_span > parallel_span_dip_) {
                    ++row;
                    parallel = 0.0F;
                }
                tabs_.push_back(TabVisual{visible_label, &tab.colors,
                    {}, D2D1::RectF(parallel, active_rise + static_cast<float>(row) * row_step,
                        parallel + tab_span, active_rise + static_cast<float>(row) * row_step + depth), row});
                parallel += std::max(tab_span - horizontal_overlap, tab_span * 0.5F);
            }
            // One rise accommodates the taller active head; a second adds
            // visible leading before its first overflow row. Further overflow
            // rows retain the regular pitch instead of accumulating this gap.
            const float inner_row_leading = row > 0 ? 2.0F * active_rise : 0.0F;
            const float raw_tab_depth = active_rise + static_cast<float>(row) * row_step +
                inner_row_leading + depth;
            tab_depth_dip_ = std::ceil(raw_tab_depth * candidate_scale / pixel_to_dip) *
                pixel_to_dip / candidate_scale;
            const auto selected_row = tabs_[active_tab_].row;
            for (std::size_t index = 0; index < tabs_.size(); ++index) {
                auto& tab = tabs_[index];
                // Lift the selected row against the panel while keeping all
                // other rows in their original relative order.
                tab.row = tab.row == selected_row ? row :
                    tab.row - (tab.row > selected_row ? 1 : 0);
                tab.logical_bounds.top = active_rise + static_cast<float>(tab.row) * row_step +
                    (tab.row == row ? inner_row_leading : 0.0F);
                tab.logical_bounds.bottom = tab.logical_bounds.top + depth;
                if (tab.row == row) {
                    tab.logical_bounds.bottom = tab_depth_dip_;
                }
                if (index == active_tab_) {
                    tab.logical_bounds.top -= active_rise;
                }
            }
            // Stagger rows away from the page by half a sloped end per row.
            // Preserve label widths and use one step that fits every row.
            float row_stagger = tab_slope_dip_ * 0.5F;
            for (const auto& tab : tabs_) {
                const auto rows_from_page = row - tab.row;
                if (rows_from_page > 0) {
                    row_stagger = std::min(row_stagger,
                        std::max(0.0F, parallel_span_dip_ - tab.logical_bounds.right) /
                            static_cast<float>(rows_from_page));
                }
            }
            for (auto& tab : tabs_) {
                const float offset = static_cast<float>(row - tab.row) * row_stagger;
                tab.logical_bounds.left += offset;
                tab.logical_bounds.right += offset;
            }
            // Outer long tabs go down first; inner rows cover their stems.
            // Keep left-to-right overlap within a row, with the active tab last.
            for (std::size_t index = 0; index < tabs_.size(); ++index) {
                tab_draw_order_.push_back(index);
            }
            std::stable_sort(tab_draw_order_.begin(), tab_draw_order_.end(),
                [this](std::size_t first, std::size_t second) {
                    if (tabs_[first].row != tabs_[second].row) {
                        return tabs_[first].row < tabs_[second].row;
                    }
                    return second == active_tab_ && first != active_tab_;
                });
            return parallel_span_dip_ * candidate_scale <= available_parallel + 0.001F &&
                (panel_depth_dip_ + tab_depth_dip_) * candidate_scale <= available_depth + 0.001F;
        };
        if (!build(1.0F)) {
            if (strict_fit) {
                throw launcher::ConfigError("The configured grid and tabs do not fit this display. "
                    "Reduce icon size, spacing or the grid dimensions.");
            }
            float lower = std::min(1.0F, pixel_to_dip / effective_.icon_size_dip);
            float upper = 1.0F;
            if (!build(lower)) {
                throw launcher::ConfigError("The fixed icon spacing and tab strip do not fit this "
                    "display even with one-pixel icons. Reduce spacing or the grid dimensions.");
            }
            for (unsigned iteration = 0; iteration < 24; ++iteration) {
                const float middle = (lower + upper) * 0.5F;
                if (build(middle)) { lower = middle; }
                else { upper = middle; }
            }
            build(lower);
        }
        const float total_depth = tab_depth_dip_ + panel_depth_dip_;
        width_dip_ = vertical() ? total_depth : parallel_span_dip_;
        height_dip_ = vertical() ? parallel_span_dip_ : total_depth;
        panel_bounds_ = layout_to_view_dip_rect(D2D1::RectF(0.0F, tab_depth_dip_,
            parallel_span_dip_, total_depth));
        for (auto& tab : tabs_) {
            auto full_bounds = tab.logical_bounds;
            full_bounds.bottom = tab_depth_dip_;
            tab.bounds = layout_to_view_dip_rect(full_bounds);
        }
        if (!configuration_.tabs.empty()) {
            const auto& active_tab = configuration_.tabs[active_tab_];
            for (const auto& placement : layout_.tabs[active_tab_].placements) {
                const auto& item = active_tab.items.at(placement.item_index);
                if (item.kind != launcher::ItemKind::icon) {
                    continue;
                }
                icons_.push_back(IconVisual{item.icon_source, utf16(item.label),
                    grid_cell_view_dip_bounds(placement.column, placement.row), {}, {}, placement.item_index,
                    utf16(item.label)});
            }
        }
    }

    RECT icon_client_px_rect(const IconVisual& icon) const noexcept
    {
        // Icons have already been rotated back upright within their cells.
        // Native tools use those same physical client-pixel bounds; their text
        // is drawn by Windows and therefore remains upright on every edge.
        return {pixels(icon.bounds.left * fit_scale_, dpi_) - render_offset_px_.x,
            pixels(icon.bounds.top * fit_scale_, dpi_) - render_offset_px_.y,
            pixels(icon.bounds.right * fit_scale_, dpi_) - render_offset_px_.x,
            pixels(icon.bounds.bottom * fit_scale_, dpi_) - render_offset_px_.y};
    }

    bool icon_tooltips_permitted() const noexcept
    {
        return window_ != nullptr && peek_.state() == launcher::DockState::expanded &&
            !hidden_to_taskbar_ && !interaction_hold_ && !menu_active_ && !display_layout_failed_ && !arranging_ && !motion_active_;
    }

    void clear_icon_tooltips(bool clear_interaction = true) noexcept
    {
        if (clear_interaction) { pressed_icon_.reset(); clear_hover(); }
        tooltip_active_ = false;
        if (tooltip_window_ == nullptr) { return; }
        SendMessageW(tooltip_window_, TTM_ACTIVATE, FALSE, 0);
        SendMessageW(tooltip_window_, TTM_POP, 0, 0);
        // Unnamed icons leave gaps in tool IDs. Remove the registered tools,
        // rather than assuming IDs 1 through the count are consecutive.
        for (std::size_t index = tooltip_tool_count_; index > 0; --index) {
            TOOLINFOW tool{};
            tool.cbSize = sizeof(tool);
            if (SendMessageW(tooltip_window_, TTM_ENUMTOOLSW, index - 1, reinterpret_cast<LPARAM>(&tool))) {
                SendMessageW(tooltip_window_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
            }
        }
        tooltip_tool_count_ = 0;
    }

    void destroy_icon_tooltips() noexcept
    {
        clear_icon_tooltips();
        const HWND tooltip = std::exchange(tooltip_window_, nullptr);
        if (tooltip != nullptr && IsWindow(tooltip)) { DestroyWindow(tooltip); }
    }

    void create_icon_tooltips()
    {
        const INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
        if (!InitCommonControlsEx(&controls)) {
            log_.result("Icon tooltips unavailable: common controls", HRESULT_FROM_WIN32(GetLastError()));
            return;
        }
        tooltip_window_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            window_, nullptr, instance_, nullptr);
        if (tooltip_window_ == nullptr) {
            log_.result("Icon tooltips unavailable: create window", HRESULT_FROM_WIN32(GetLastError()));
            return;
        }
        SetWindowPos(tooltip_window_, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        // A fresh dwell is required after moving to another icon, even when
        // the preceding tooltip was already visible. Windows owns the timers.
        const LPARAM delay = static_cast<LPARAM>(configuration_.appearance.tooltip_delay_ms);
        SendMessageW(tooltip_window_, TTM_SETDELAYTIME, TTDT_INITIAL, delay);
        SendMessageW(tooltip_window_, TTM_SETDELAYTIME, TTDT_RESHOW, delay);
        SendMessageW(tooltip_window_, TTM_SETDELAYTIME, TTDT_AUTOPOP, 10000);
        SendMessageW(tooltip_window_, TTM_ACTIVATE, FALSE, 0);
        log_.write("Native icon label tooltips initialized.");
    }

    void sync_icon_tooltips()
    {
        clear_icon_tooltips(false);
        if (tooltip_window_ == nullptr || !icon_tooltips_permitted()) { return; }
        // A positive width enables multiline labels, including labels longer
        // than the native single-line tooltip limit. It follows monitor DPI.
        SendMessageW(tooltip_window_, TTM_SETMAXTIPWIDTH, 0, pixels(320.0F, dpi_));
        for (std::size_t index = 0; index < icons_.size(); ++index) {
            auto& icon = icons_[index];
            if (icon.tooltip.empty()) { continue; }
            TOOLINFOW tool{};
            tool.cbSize = sizeof(tool);
            tool.uFlags = TTF_SUBCLASS | TTF_TRANSPARENT;
            tool.hwnd = window_;
            tool.uId = static_cast<UINT_PTR>(index + 1);
            tool.rect = icon_client_px_rect(icon);
            tool.hinst = instance_;
            tool.lpszText = icon.tooltip.data();
            if (!SendMessageW(tooltip_window_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool))) {
                clear_icon_tooltips(false);
                log_.write("Icon tooltips unavailable: native tool registration failed.");
                return;
            }
            ++tooltip_tool_count_;
        }
        tooltip_active_ = tooltip_tool_count_ != 0;
        SendMessageW(tooltip_window_, TTM_ACTIVATE, tooltip_active_, 0);
    }

    HRESULT verify_icon_tooltips(bool expanded)
    {
        const auto fail = [this](const char* message) {
            log_.write(message);
            return E_FAIL;
        };
        if (tooltip_window_ == nullptr) {
            return fail("Smoke icon tooltips failed: native tooltip window unavailable.");
        }
        const auto expected_count = expanded ? static_cast<std::size_t>(std::count_if(
            icons_.begin(), icons_.end(), [](const IconVisual& icon) { return !icon.tooltip.empty(); })) : 0;
        const bool expected_active = expanded && expected_count != 0;
        if (tooltip_tool_count_ != expected_count ||
                static_cast<std::size_t>(SendMessageW(tooltip_window_, TTM_GETTOOLCOUNT, 0, 0)) != expected_count ||
                tooltip_active_ != expected_active || (!expected_active && IsWindowVisible(tooltip_window_))) {
            return fail("Smoke icon tooltips failed: tools survived collapse/suppression or did not rebuild.");
        }
        if (!expanded) {
            log_.write("Smoke icon tooltips: inactive tools=0; tooltip hidden.");
            return S_OK;
        }
        const auto style = static_cast<DWORD_PTR>(GetWindowLongPtrW(tooltip_window_, GWL_STYLE));
        const auto extended = static_cast<DWORD_PTR>(GetWindowLongPtrW(tooltip_window_, GWL_EXSTYLE));
        if ((style & TTS_NOPREFIX) == 0 || (extended & WS_EX_NOACTIVATE) == 0 ||
                GetWindow(tooltip_window_, GW_OWNER) != window_ ||
                GetForegroundWindow() == tooltip_window_ || GetFocus() == tooltip_window_ ||
                SendMessageW(tooltip_window_, TTM_GETMAXTIPWIDTH, 0, 0) <= 0 ||
                SendMessageW(tooltip_window_, TTM_GETDELAYTIME, TTDT_INITIAL, 0) !=
                    static_cast<LRESULT>(configuration_.appearance.tooltip_delay_ms) ||
                SendMessageW(tooltip_window_, TTM_GETDELAYTIME, TTDT_RESHOW, 0) !=
                    static_cast<LRESULT>(configuration_.appearance.tooltip_delay_ms)) {
            return fail("Smoke icon tooltips failed: delay, literal text, wrapping, ownership or activation mismatch.");
        }
        RECT client{};
        if (!GetClientRect(window_, &client)) { return E_FAIL; }
        std::size_t unicode_labels = 0;
        std::size_t literal_ampersands = 0;
        std::size_t long_labels = 0;
        for (std::size_t index = 0; index < icons_.size(); ++index) {
            const auto& icon = icons_[index];
            const RECT expected = icon_client_px_rect(icon);
            std::vector<wchar_t> text(icon.tooltip.size() + 1, L'\0');
            TOOLINFOW tool{};
            tool.cbSize = sizeof(tool);
            tool.hwnd = window_;
            tool.uId = static_cast<UINT_PTR>(index + 1);
            tool.lpszText = text.data();
            if (icon.tooltip.empty()) {
                TTHITTESTINFOW hit{};
                hit.hwnd = window_;
                hit.pt = {(expected.left + expected.right) / 2, (expected.top + expected.bottom) / 2};
                hit.ti.cbSize = sizeof(hit.ti);
                if (SendMessageW(tooltip_window_, TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&tool)) ||
                        SendMessageW(tooltip_window_, TTM_HITTESTW, 0, reinterpret_cast<LPARAM>(&hit))) {
                    return fail("Smoke icon tooltips failed: an unnamed icon acquired a tooltip.");
                }
                continue;
            }
            if (!SendMessageW(tooltip_window_, TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&tool)) ||
                    !EqualRect(&tool.rect, &expected) ||
                    (tool.uFlags & (TTF_SUBCLASS | TTF_TRANSPARENT)) != (TTF_SUBCLASS | TTF_TRANSPARENT) ||
                    expected.right <= expected.left || expected.bottom <= expected.top ||
                    expected.left < client.left || expected.top < client.top ||
                    expected.right > client.right || expected.bottom > client.bottom) {
                return fail("Smoke icon tooltips failed: tool flags or rotated physical client bounds differ.");
            }
            tool.lpszText = text.data();
            SendMessageW(tooltip_window_, TTM_GETTEXTW, static_cast<WPARAM>(text.size()),
                reinterpret_cast<LPARAM>(&tool));
            if (std::wstring_view(text.data()) != icon.tooltip) {
                return fail("Smoke icon tooltips failed: complete Unicode label was not retained.");
            }
            TTHITTESTINFOW hit{};
            hit.hwnd = window_;
            hit.pt = {(expected.left + expected.right) / 2, (expected.top + expected.bottom) / 2};
            hit.ti.cbSize = sizeof(hit.ti);
            if (!SendMessageW(tooltip_window_, TTM_HITTESTW, 0, reinterpret_cast<LPARAM>(&hit)) ||
                    hit.ti.uId != static_cast<UINT_PTR>(index + 1)) {
                return fail("Smoke icon tooltips failed: native hit test did not match an icon.");
            }
            unicode_labels += std::any_of(icon.label.begin(), icon.label.end(),
                [](wchar_t character) { return character > 127; }) ? 1 : 0;
            literal_ampersands += icon.label.find(L'&') != std::wstring::npos ? 1 : 0;
            long_labels += icon.label.size() > 80 ? 1 : 0;
        }
        // A spacer or an unused cell must not receive its neighbour's label.
        const float step = drawn_icon_size_dip_ + icon_gap_dip_;
        for (std::size_t row = 0; row < layout_.rows; ++row) {
            for (std::size_t column = 0; column < layout_.columns; ++column) {
                const auto point = layout_to_view_dip_point(icon_gap_dip_ + static_cast<float>(column) * step +
                    drawn_icon_size_dip_ * 0.5F, tab_depth_dip_ + icon_gap_dip_ +
                    static_cast<float>(row) * step + drawn_icon_size_dip_ * 0.5F);
                if (std::any_of(icons_.begin(), icons_.end(),
                        [point](const IconVisual& icon) { return contains(icon.bounds, point); })) { continue; }
                TTHITTESTINFOW hit{};
                hit.hwnd = window_;
                hit.pt = {pixels(point.x * fit_scale_, dpi_) - render_offset_px_.x,
                    pixels(point.y * fit_scale_, dpi_) - render_offset_px_.y};
                hit.ti.cbSize = sizeof(hit.ti);
                if (SendMessageW(tooltip_window_, TTM_HITTESTW, 0, reinterpret_cast<LPARAM>(&hit))) {
                    return fail("Smoke icon tooltips failed: a structural or empty cell acquired a label.");
                }
            }
        }
        log_.write("Smoke icon tooltips: tools=", std::to_string(expected_count),
            "; delay-ms=", std::to_string(configuration_.appearance.tooltip_delay_ms),
            "; unicode-labels=", std::to_string(unicode_labels),
            "; literal-ampersands=", std::to_string(literal_ampersands),
            "; long-labels=", std::to_string(long_labels),
            "; native rectangles/text/hit tests verified; no tooltip activation.");
        return S_OK;
    }

    HRESULT verify_icon_clicks(bool expanded)
    {
        const auto send = [this](UINT message, POINT point, bool shift = false) {
            SendMessageW(window_, message, (message == WM_LBUTTONDOWN ? MK_LBUTTON : 0) | (shift ? MK_SHIFT : 0),
                MAKELPARAM(point.x, point.y));
        };
        const POINT outside{-1, -1};
        for (std::size_t index = 0; index < icons_.size(); ++index) {
            const auto bounds = icon_client_px_rect(icons_[index]);
            const POINT point{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
            const auto item_index = icons_[index].item_index;
            const auto& item = configuration_.tabs[active_tab_].items[item_index];
            const bool enabled = expanded && item.launch.has_value();
            auto count = smoke_launch_count_;
            send(WM_LBUTTONUP, point); // An unmatched release must do nothing.
            send(WM_LBUTTONDOWN, point);
            if (smoke_launch_count_ != count) { return E_FAIL; }
            send(WM_LBUTTONUP, point);
            if (smoke_launch_count_ != count + (enabled ? 1U : 0U) ||
                    (enabled && smoke_last_launch_ != std::pair{active_tab_, item_index})) {
                return E_FAIL;
            }
            count = smoke_launch_count_;
            send(WM_LBUTTONDOWN, point);
            send(WM_LBUTTONUP, outside);
            send(WM_LBUTTONDOWN, point);
            SendMessageW(window_, WM_MOUSELEAVE, 0, 0);
            send(WM_LBUTTONUP, point);
            if (smoke_launch_count_ != count || pressed_icon_ || drag_ ||
                    GetCapture() == window_ || GetForegroundWindow() == window_) { return E_FAIL; }
            // The release determines the action, even if Shift changed after
            // the press. Missing actions chirp without falling back.
            for (const bool shift : {false, true}) {
                const auto& action = shift ? item.shift_launch : item.launch;
                count = smoke_launch_count_;
                const auto chirps = smoke_chirp_count_;
                send(WM_LBUTTONDOWN, point, !shift);
                send(WM_LBUTTONUP, point, shift);
                if (smoke_launch_count_ != count + (expanded && action ? 1U : 0U) ||
                        smoke_chirp_count_ != chirps + (expanded && !action ? 1U : 0U) ||
                        (expanded && action && smoke_cached_launch_ != action)) { return E_FAIL; }
            }
            // Inspect the same native menu commands used by right-click.
            const auto menu = CreatePopupMenu();
            if (!menu) { return E_FAIL; }
            append_activation_commands(menu, item, true);
            const bool valid_menu = GetMenuItemCount(menu) == static_cast<int>(item.launch.has_value() + item.shift_launch.has_value()) &&
                (GetMenuState(menu, kIconOpenCommand, MF_BYCOMMAND) != static_cast<UINT>(-1)) == item.launch.has_value() &&
                (GetMenuState(menu, kIconAlternateCommand, MF_BYCOMMAND) != static_cast<UINT>(-1)) == item.shift_launch.has_value();
            DestroyMenu(menu);
            if (!valid_menu) { return E_FAIL; }
            // Menu choices are explicit and do not follow keyboard modifiers.
            count = smoke_launch_count_;
            const auto chirps = smoke_chirp_count_;
            launch_icon(index, {}, true, false, nullptr, nullptr, true);
            if (smoke_launch_count_ != count + (item.shift_launch ? 1U : 0U) ||
                    smoke_chirp_count_ != chirps + (item.shift_launch ? 0U : 1U) ||
                    (item.shift_launch && smoke_cached_launch_ != item.shift_launch)) { return E_FAIL; }
        }
        log_.write("Smoke icon clicks: page=", std::to_string(active_tab_),
            "; expanded=", (expanded ? "yes" : "no"),
            "; matched releases route configured actions; placeholders, unmatched releases, "
            "outside releases and departed presses are inert; Shift release selects alternate, missing actions chirp; "
            "Open/Alternate menu presence and alternate dispatch verified; no real launches.");
        return S_OK;
    }

    HRESULT verify_tab_selection()
    {
        const auto saved_controller = peek_;
        const auto saved_tab = active_tab_;
        const auto columns = layout_.columns;
        const auto rows = layout_.rows;
        const auto position = effective_.edge_position;
        const auto client_point = [&](D2D1_POINT_2F point) {
            return POINT{pixels(point.x * fit_scale_, dpi_) - render_offset_px_.x,
                pixels(point.y * fit_scale_, dpi_) - render_offset_px_.y};
        };
        const auto click = [&](std::size_t index) {
            const auto& bounds = tabs_[index].logical_bounds;
            const auto point = client_point(layout_to_view_dip_point((bounds.left + bounds.right) * 0.5F,
                (bounds.top + bounds.bottom) * 0.5F));
            SendMessageW(window_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
            SendMessageW(window_, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
        };
        const auto verify_page = [&](bool expanded) -> HRESULT {
            // Test-only settling includes optional presentation preparation;
            // selecting a collapsed page still does not activate discovery.
            if (!settle_version_discovery()) { return E_FAIL; }
            if (layout_.columns != columns || layout_.rows != rows ||
                    effective_.edge_position != position || GetCapture() == window_ ||
                    GetForegroundWindow() == window_) { return E_FAIL; }
            for (std::size_t index = 0; index < configuration_.tabs.size(); ++index) {
                const auto& tab = configuration_.tabs[index];
                const auto expected = utf16(index == active_tab_ || tab.short_name.empty()
                    ? tab.name : tab.short_name);
                if (tabs_[index].label != expected) { return E_FAIL; }
            }
            if (FAILED(verify_tab_stacking())) { return E_FAIL; }
            std::size_t icon_index = 0;
            for (const auto& item : configuration_.tabs[active_tab_].items) {
                if (item.kind != launcher::ItemKind::icon) { continue; }
                if (icon_index >= icons_.size() || icons_[icon_index].source != item.icon_source ||
                        icons_[icon_index].label != resolved_item_label(item)) { return E_FAIL; }
                ++icon_index;
            }
            if (icon_index != icons_.size()) { return E_FAIL; }
            HRESULT result = render();
            if (result == D2DERR_RECREATE_TARGET) { result = render(); }
            if (SUCCEEDED(result) && std::any_of(icons_.begin(), icons_.end(),
                    [](const IconVisual& icon) { return !icon.image || !icon.bitmap; })) {
                return E_FAIL;
            }
            if (SUCCEEDED(result)) { result = verify_grid_spacing(); }
            if (SUCCEEDED(result)) { result = verify_grid_screen_order(); }
            if (SUCCEEDED(result)) { result = verify_tab_metrics(); }
            if (SUCCEEDED(result)) { result = verify_icon_tooltips(expanded); }
            if (SUCCEEDED(result)) { result = verify_icon_clicks(expanded); }
            if (SUCCEEDED(result) && !verify_native_state(expanded
                    ? launcher::DockState::expanded : launcher::DockState::collapsed)) {
                return E_FAIL;
            }
            return result;
        };
        HRESULT result = S_OK;
        for (const bool expanded : {true, false}) {
            peek_ = launcher::PeekController(true);
            peek_.set_menu_open(expanded);
            apply_state(true);
            for (std::size_t index = 0; index < configuration_.tabs.size(); ++index) {
                click(index);
                if (active_tab_ != index || FAILED(result = verify_page(expanded))) {
                    result = E_FAIL;
                    break;
                }
                const auto old_placement = placement_;
                const auto* old_icons = icons_.data();
                const auto* old_target = drawing_target_.Get();
                click(index);
                if (active_tab_ != index || icons_.data() != old_icons || drawing_target_.Get() != old_target ||
                        placement_.expanded != old_placement.expanded ||
                        placement_.collapsed_tabs != old_placement.collapsed_tabs) {
                    result = E_FAIL;
                    break;
                }
                // Use the leftmost face in the outermost row: neighbouring
                // overlaps can legitimately fill another tab's sloped cutout.
                const auto& outer = *std::min_element(tabs_.begin(), tabs_.end(),
                    [](const TabVisual& first, const TabVisual& second) {
                        return first.row != second.row ? first.row < second.row :
                            first.logical_bounds.left < second.logical_bounds.left;
                    });
                const auto& first = outer.logical_bounds;
                const float slope = std::min(tab_slope_dip_,
                    (first.right - first.left) * 0.5F);
                // Stay inside the rectangular bounds while outside the slope.
                // An exact corner can be excluded by rectangle hit testing alone.
                // Skip only when fitting makes this cutout too small to sample
                // reliably with native integer mouse coordinates.
                if (slope * fit_scale_ * static_cast<float>(dpi_) / 96.0F >= 4.0F) {
                    const auto corner = client_point(layout_to_view_dip_point(first.left + slope * 0.25F,
                        first.top + (first.bottom - first.top) * 0.25F));
                    bool gap = true;
                    if (window_bitmap_) {
                        // An extruded wall can fill the former flat-face cutout.
                        // A truly transparent pixel must still reject a tab hit.
                        WICRect sample{corner.x, corner.y, 1, 1};
                        std::array<BYTE, 4> pixel{};
                        if (FAILED(window_bitmap_->CopyPixels(&sample, 4, 4, pixel.data()))) { return E_FAIL; }
                        gap = pixel[3] == 0;
                    }
                    if (!contains(outer.bounds, client_px_to_view_dip_point(corner)) || (gap && tab_at(corner))) {
                        result = E_FAIL;
                        break;
                    }
                }
            }
            if (FAILED(result)) { break; }
        }
        if (SUCCEEDED(result)) {
            // Adjustment success and failure must retain the page and its tools.
            std::wstring error;
            const auto selected = active_tab_;
            if (!rebuild_view(runtime_, selected, error)) { result = E_FAIL; }
            auto invalid = runtime_;
            invalid.text_scale = 0.0;
            if (SUCCEEDED(result) && (rebuild_view(invalid, selected, error) ||
                    active_tab_ != selected || FAILED(verify_page(false)))) { result = E_FAIL; }
            if (select_tab(configuration_.tabs.size())) { result = E_FAIL; }
        }
        select_tab(saved_tab);
        peek_ = saved_controller;
        apply_state(true);
        if (SUCCEEDED(result)) {
            log_.write("Smoke tab selection: pages=", std::to_string(configuration_.tabs.size()),
                "; expanded/collapsed clicks, full/short names, selected icons/tooltips, "
                "shared grid, active-row adjacency, long stems/overlaps, no-op clicks, "
                "sloped corners, preview rollback and no activation verified.");
        } else {
            log_.write("Smoke tab selection failed: selected page, native geometry, resources or hit testing differ.");
        }
        return result;
    }

    HRESULT verify_tab_gap_hover()
    {
        if (configuration_.tabs.size() < 3) {
            log_.write("Smoke tab gap hover: skipped; fewer than three pages.");
            return S_OK;
        }
        const auto saved_controller = peek_;
        const auto saved_tab = active_tab_;
        const bool required_sample = configuration_.tabs.size() == 4 &&
            ((configuration_.tabs[0].name == "General" &&
              configuration_.tabs[1].name == "Documents and References" &&
              configuration_.tabs[2].name == "Links") ||
             (configuration_.tabs[0].name == "General utilities" &&
              configuration_.tabs[1].name == "Documents and reference material" &&
              configuration_.tabs[2].name == "An empty page"));
        const auto verify = [&]() -> HRESULT {
            struct Region final {
                HRGN value = CreateRectRgn(0, 0, 0, 0);
                ~Region() { if (value) { DeleteObject(value); } }
            } before_region, after_region;
            if (!before_region.value || !after_region.value) { return E_FAIL; }
            peek_ = launcher::PeekController(true);
            peek_.pointer_changed(true);
            peek_.timer_elapsed();
            apply_state(true);
            if (!select_tab(1)) { return E_FAIL; }
            RECT before{};
            if (!GetWindowRect(window_, &before) ||
                    GetWindowRgn(window_, before_region.value) == ERROR) { return E_FAIL; }
            if (!select_tab(2)) { return E_FAIL; }
            RECT after{};
            RECT common{};
            if (!GetWindowRect(window_, &after) ||
                    GetWindowRgn(window_, after_region.value) == ERROR) { return E_FAIL; }
            std::optional<POINT> gap;
            if (IntersectRect(&common, &before, &after)) {
                // Find an actual native pixel lost when the former active tab
                // shrinks/reflows, while remaining inside both window frames.
                // This avoids assumptions about fonts, DPI or orientation.
                for (LONG y = common.top; y < common.bottom && !gap; ++y) {
                    for (LONG x = common.left; x < common.right; ++x) {
                        if (PtInRegion(before_region.value, x - before.left, y - before.top) &&
                                !PtInRegion(after_region.value, x - after.left, y - after.top)) {
                            gap = POINT{x, y};
                            break;
                        }
                    }
                }
            }
            if (!gap) {
                log_.write(required_sample
                    ? "Smoke tab gap hover failed: the sample did not exercise a shrinking-tab gap."
                    : "Smoke tab gap hover: skipped; this page transition creates no native gap.");
                return required_sample ? E_FAIL : S_OK;
            }
            // Inject geometry, never the desktop cursor. Hidden smoke windows
            // cannot use the production visibility/WindowFromPoint wrapper.
            const bool gap_inside = pointer_inside_at(*gap, nullptr);
            peek_.pointer_changed(gap_inside);
            peek_.timer_elapsed();
            if (!gap_inside || peek_.state() != launcher::DockState::expanded ||
                    peek_.pending() != launcher::Pending::none) {
                log_.write("Smoke tab gap hover failed: expanded native gap scheduled hiding.");
                return E_FAIL;
            }
            const POINT outside{after.right, after.top + (after.bottom - after.top) / 2};
            const bool outside_inside = pointer_inside_at(outside, nullptr);
            peek_.pointer_changed(outside_inside);
            if (outside_inside || peek_.pending() != launcher::Pending::hide) {
                log_.write("Smoke tab gap hover failed: point outside expanded frame did not schedule hiding.");
                return E_FAIL;
            }
            peek_.timer_elapsed();
            apply_state(true);
            RECT collapsed{};
            if (peek_.state() != launcher::DockState::collapsed ||
                    !GetWindowRect(window_, &collapsed) ||
                    GetWindowRgn(window_, after_region.value) == ERROR) {
                log_.write("Smoke tab gap hover failed: hide expiry did not produce collapsed native geometry.");
                return E_FAIL;
            }
            // Collapse repositions the strip. Probe its current native region,
            // rather than reusing an expanded screen coordinate.
            std::optional<POINT> collapsed_gap;
            for (LONG y = collapsed.top; y < collapsed.bottom && !collapsed_gap; ++y) {
                for (LONG x = collapsed.left; x < collapsed.right; ++x) {
                    if (!PtInRegion(after_region.value, x - collapsed.left, y - collapsed.top)) {
                        collapsed_gap = POINT{x, y};
                        break;
                    }
                }
            }
            if (!collapsed_gap) {
                log_.write("Smoke tab gap hover failed: collapsed strip provided no transparent native sample.");
                return E_FAIL;
            }
            const bool collapsed_inside = pointer_inside_at(*collapsed_gap, nullptr);
            peek_.pointer_changed(collapsed_inside);
            peek_.timer_elapsed();
            if (collapsed_inside || peek_.state() != launcher::DockState::collapsed ||
                    peek_.pending() != launcher::Pending::none) {
                log_.write("Smoke tab gap hover failed: transparent collapsed pixel scheduled revealing.");
                return E_FAIL;
            }
            log_.write("Smoke tab gap hover: sample=1; page 1->2 native gap retains expanded hover, "
                "real exit schedules hide, and collapsed gaps do not reveal; desktop cursor unchanged.");
            return S_OK;
        };
        HRESULT result = verify();
        if (!select_tab(saved_tab)) { result = E_FAIL; }
        peek_ = saved_controller;
        apply_state(true);
        if (FAILED(result)) {
            log_.write("Smoke tab gap hover failed: native geometry or delayed hide/reveal ownership differs.");
        }
        return result;
    }

    HRESULT verify_tab_stacking()
    {
        const auto innermost_row = std::max_element(tabs_.begin(), tabs_.end(),
            [](const TabVisual& first, const TabVisual& second) { return first.row < second.row; })->row;
        if (tabs_[active_tab_].row != innermost_row ||
                std::abs(tabs_[active_tab_].logical_bounds.bottom - tab_depth_dip_) > 0.001F ||
                tab_draw_order_.size() != tabs_.size() || tab_draw_order_.back() != active_tab_) {
            log_.write("Smoke stacked tabs failed: selected row is not adjacent to the panel.");
            return E_FAIL;
        }
        HRGN region = CreateRectRgn(0, 0, 0, 0);
        if (!region || GetWindowRgn(window_, region) == ERROR) {
            if (region) { DeleteObject(region); }
            return E_FAIL;
        }
        // Probe the native region and click ownership in the short heads and
        // down the long bodies. Reference ownership follows row depth, then
        // selected-tab/left-to-right overlap, independently of the paint list.
        std::size_t exposed_stems = 0;
        bool valid = true;
        for (std::size_t index = 0; index < tabs_.size() && valid; ++index) {
            const auto& bounds = tabs_[index].logical_bounds;
            for (const float fraction : {0.1F, 0.5F, 0.9F}) {
                const float parallel = bounds.left + (bounds.right - bounds.left) * fraction;
                for (const float depth : {(bounds.top + bounds.bottom) * 0.5F,
                        (bounds.bottom + tab_depth_dip_) * 0.5F}) {
                    const auto physical = layout_to_view_dip_point(parallel, depth);
                    const POINT client{pixels(physical.x * fit_scale_, dpi_) - render_offset_px_.x,
                        pixels(physical.y * fit_scale_, dpi_) - render_offset_px_.y};
                    // Convert the rounded native mouse coordinate back into
                    // logical space before computing reference ownership.
                    D2D1::Matrix3x2F inverse = canvas_to_view_transform();
                    inverse.Invert();
                    const auto canvas = inverse.TransformPoint(client_px_to_view_dip_point(client));
                    const auto logical = tabs_above_page()
                        ? canvas : D2D1::Point2F(canvas.x, panel_depth_dip_ + tab_depth_dip_ - canvas.y);
                    std::optional<std::size_t> expected;
                    bool near_boundary = false;
                    const float wall_allowance = static_cast<float>(sheet_thickness_pixels()) * 1.5F;
                    const float tolerance = (1.5F + wall_allowance) * 96.0F / static_cast<float>(dpi_) / fit_scale_;
                    for (std::size_t candidate = 0; candidate < tabs_.size(); ++candidate) {
                        const auto& head = tabs_[candidate].logical_bounds;
                        const float slope = std::min(tab_slope_dip_, (head.right - head.left) * 0.5F);
                        const float inset = slope * std::clamp((head.bottom - logical.y) /
                            (head.bottom - head.top), 0.0F, 1.0F);
                        const float left = head.left + inset;
                        const float right = head.right - inset;
                        if (logical.y >= head.top - tolerance && logical.y <= tab_depth_dip_ + tolerance &&
                                logical.x >= left - tolerance && logical.x <= right + tolerance &&
                                (std::abs(logical.x - left) < tolerance ||
                                 std::abs(logical.x - right) < tolerance ||
                                 std::abs(logical.y - head.top) < tolerance ||
                                 std::abs(logical.y - tab_depth_dip_) < tolerance)) {
                            near_boundary = true;
                        }
                        if (logical.y < head.top || logical.y >= tab_depth_dip_ ||
                                logical.x < left || logical.x >= right) { continue; }
                        if (!expected || tabs_[candidate].row > tabs_[*expected].row ||
                                (tabs_[candidate].row == tabs_[*expected].row &&
                                 (candidate == active_tab_ || (*expected != active_tab_ && candidate > *expected)))) {
                            expected = candidate;
                        }
                    }
                    // Raster rounding at an outline is deliberately excluded.
                    if (near_boundary) { continue; }
                    const auto actual = tab_at(client);
                    const bool visible = PtInRegion(region, client.x, client.y) != FALSE;
                    if (actual != expected || (expected && !visible)) {
                        valid = false;
                        break;
                    }
                    if (expected == index && logical.y > bounds.bottom + tolerance) { ++exposed_stems; }
                }
                if (!valid) { break; }
            }
        }
        DeleteObject(region);
        if (!valid) {
            log_.write("Smoke stacked tabs failed: visible head/stem overlap and native click region differ.");
            return E_FAIL;
        }
        log_.write("Smoke stacked tabs: rows=", std::to_string(innermost_row + 1),
            "; active-row=", std::to_string(tabs_[active_tab_].row),
            "; exposed-stem-samples=", std::to_string(exposed_stems),
            "; head/stem ownership and native regions verified.");
        return S_OK;
    }

    Monitor select_monitor(bool initial)
    {
        std::vector<Monitor> monitors;
        if (!EnumDisplayMonitors(nullptr, nullptr, collect_monitor,
                reinterpret_cast<LPARAM>(&monitors)) || monitors.empty()) {
            throw std::runtime_error("Cannot enumerate the connected displays.");
        }
        const std::wstring requested = utf16(configuration_.dock.display);
        const Monitor* primary = nullptr;
        const Monitor* selected = nullptr;
        for (const auto& entry : monitors) {
            if ((entry.information.dwFlags & MONITORINFOF_PRIMARY) != 0) {
                primary = &entry;
            }
            if (_wcsicmp(requested.c_str(), entry.information.szDevice) == 0) {
                selected = &entry;
            }
            if (initial) {
                log_.write("Display ", utf8(entry.information.szDevice),
                    "; screen=", rect_text(entry.information.rcMonitor),
                    "; work=", rect_text(entry.information.rcWork),
                    ((entry.information.dwFlags & MONITORINFOF_PRIMARY) != 0
                        ? "; primary." : "."));
            }
        }
        if (primary == nullptr) {
            primary = &monitors.front();
        }
        if (_wcsicmp(requested.c_str(), L"primary") == 0) {
            selected = primary;
        }
        if (selected == nullptr) {
            if (initial) {
                throw launcher::ConfigError("Configured display '" +
                    configuration_.dock.display + "' is unavailable. Use 'primary' or "
                    "a display device name listed in launcher.log.");
            }
            selected = primary;
            log_.write("Configured display unavailable; falling back to primary display.");
        }
        return *selected;
    }

    void update_placement(bool strict_fit, bool log_placement = true)
    {
        const RECT work = monitor_.information.rcWork;
        if (strict_fit && fit_scale_ < 0.999F) {
            throw launcher::ConfigError("The configured grid and tabs do not fit the selected "
                "display at its current DPI. Reduce the grid dimensions or icon size.");
        }
        const int parallel = std::max(1, pixels(parallel_span_dip_ * fit_scale_, dpi_));
        int panel_depth = std::max(1, pixels(panel_depth_dip_ * fit_scale_, dpi_));
        const int tab_depth = std::max(1, pixels(tab_depth_dip_ * fit_scale_, dpi_));
        const int available_depth_pixels = vertical()
            ? work.right - work.left : work.bottom - work.top;
        panel_depth = std::min(panel_depth, available_depth_pixels - tab_depth);
        placement_ = launcher::place_dock(configuration_.dock.edge, pixel_rect(work),
            parallel, panel_depth, tab_depth, effective_.edge_position);
        if (log_placement) {
            log_.write("Dock placement: display=", utf8(monitor_.information.szDevice),
                "; edge=", edge_name(configuration_.dock.edge), "; dpi=",
                std::to_string(dpi_), "; fit-scale=", std::to_string(fit_scale_), ".");
        }
    }

    void cancel_motion() noexcept
    {
        KillTimer(window_, kMotionTimer);
        motion_active_ = false;
        if (motion_frame_) { discard_device_resources(); }
        motion_frame_.Reset();
    }

    void cache_motion_frame()
    {
        if (motion_frame_) { return; }
        // Draw the full sheet once. Animation only crops and transfers these
        // pixels, so icon decoding and Direct2D shading stay off the timer path.
        discard_device_resources();
        const auto saved_offset = render_offset_px_;
        const int padding = prism_frame_padding_px();
        render_offset_px_ = {-padding, -padding};
        const UINT width = static_cast<UINT>(placement_.expanded.right - placement_.expanded.left + 2 * padding);
        const UINT height = static_cast<UINT>(placement_.expanded.bottom - placement_.expanded.top + 2 * padding);
        HRESULT result = wic_factory_->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnLoad, preview_bitmap_.ReleaseAndGetAddressOf());
        if (SUCCEEDED(result)) { result = render(); }
        motion_frame_ = std::move(preview_bitmap_);
        render_offset_px_ = saved_offset;
        if (FAILED(result)) {
            cancel_motion();
            throw std::runtime_error("Cannot prepare the sliding dock frame.");
        }
    }

    void position_dock_frame(launcher::PixelRect canvas)
    {
        const auto destination = launcher::dock_viewport(canvas,
            pixel_rect(monitor_.information.rcWork), prism_frame_padding_px());
        render_offset_px_ = {destination.left - canvas.left, destination.top - canvas.top};
        const int padding = prism_frame_padding_px();
        motion_crop_ = {render_offset_px_.x + padding, render_offset_px_.y + padding,
            destination.right - destination.left, destination.bottom - destination.top};
        // Every intermediate viewport stays inside the selected display's work area.
        arranging_ = true;
        const bool topmost = (GetWindowLongPtrW(window_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        const BOOL moved = SetWindowPos(window_, topmost ? HWND_TOPMOST : nullptr,
            destination.left, destination.top, destination.right - destination.left,
            destination.bottom - destination.top, SWP_NOACTIVATE | SWP_NOCOPYBITS | (topmost ? 0U : SWP_NOZORDER));
        arranging_ = false;
        if (!moved) { throw std::runtime_error("Cannot position the dock window."); }
        HRESULT result = render();
        if (result == D2DERR_RECREATE_TARGET) { result = render(); }
        if (FAILED(result)) {
            log_.result("Interactive dock frame", result);
            throw std::runtime_error("Cannot draw the dock window.");
        }
        if (!smoke_mode_ && !restart_pending_) { ShowWindow(window_, SW_SHOWNOACTIVATE); }
        InvalidateRect(window_, nullptr, FALSE);
    }

    void advance_motion(std::uint64_t now)
    {
        if (!motion_active_) { return; }
        const double expansion = motion_.value(now);
        const bool finished = !motion_.running(now);
        if (finished) { cancel_motion(); }
        position_dock_frame(launcher::dock_canvas(placement_, expansion));
        if (finished) {
            sync_icon_tooltips();
            if (!smoke_mode_) { pointer_changed(pointer_inside()); sync_hover(); }
        }
    }

    void apply_state(bool force = false)
    {
        // Manual hiding owns the native window until the user restores it.
        // Fullscreen suppression and stale hover messages must not reveal it.
        if (hidden_to_taskbar_ || taskbar_transition_) { sync_version_activation(); return; }
        // A settings dialog, context menu, or drag needs a stationary owner,
        // including when its logical target already says 'expanded'.
        if (motion_active_ && (interaction_hold_ || menu_active_ || drag_)) { force = true; }
        const auto state = peek_.state();
        const bool changed = !applied_state_ || *applied_state_ != state;
        if (!force && applied_state_ && *applied_state_ == state) {
            sync_version_activation();
            return;
        }
        if (diagnostics_enabled_) {
            ++diagnostics_.state_applications;
        }
        if (state != launcher::DockState::expanded) {
            cancel_gap_tracking();
        }
        clear_icon_tooltips();
        if (state == launcher::DockState::suppressed) {
            cancel_motion();
            motion_.reset(false);
            cancel_pointer_tracking();
            ShowWindow(window_, SW_HIDE);
        } else {
            const bool expanded = state == launcher::DockState::expanded;
            const auto duration = expanded ? configuration_.dock.reveal_duration_ms : configuration_.dock.hide_duration_ms;
            const bool animate = changed && !force && !smoke_mode_ && !restart_pending_ &&
                !interaction_hold_ && !menu_active_ && applied_state_ &&
                *applied_state_ != launcher::DockState::suppressed && duration != 0;
            if (animate) {
                cache_motion_frame();
                motion_.start(expanded, duration, GetTickCount64());
                motion_active_ = true;
                if (!SetTimer(window_, kMotionTimer, 15, nullptr)) {
                    throw std::runtime_error("Cannot start the dock movement timer.");
                }
            } else {
                cancel_motion();
                motion_.reset(expanded);
            }
            const double expansion = motion_.value(GetTickCount64());
            position_dock_frame(launcher::dock_canvas(placement_, expansion));
        }
        const char* description = state == launcher::DockState::collapsed ? "collapsed"
            : state == launcher::DockState::expanded ? "expanded" : "suppressed";
        if (changed) {
            log_.write("Dock state: ", description, ".");
        }
        applied_state_ = state;
        sync_version_activation();
        sync_icon_tooltips();
        if (!smoke_mode_) { sync_hover(); }
    }

    void cancel_timer() noexcept
    {
        if (timer_id_ != 0) {
            KillTimer(window_, timer_id_);
            timer_id_ = 0;
        }
        timer_pending_ = launcher::Pending::none;
    }

    void sync_timer()
    {
        sync_gap_tracking();
        const auto pending = peek_.pending();
        if (smoke_mode_ || hidden_to_taskbar_ || pending == launcher::Pending::none) {
            cancel_timer();
            return;
        }
        if (timer_id_ != 0 && pending == timer_pending_) {
            return;
        }
        cancel_timer();
        // Unique IDs reject previously queued WM_TIMER messages after cancellation.
        ++timer_generation_;
        const UINT_PTR requested_id = 100 + timer_generation_;
        UINT delay = pending == launcher::Pending::reveal
            ? configuration_.dock.hover_delay_ms : configuration_.dock.hide_delay_ms;
        const auto now = GetTickCount64();
        if (pending == launcher::Pending::hide && now < launch_reveal_until_) {
            delay = std::max(delay, static_cast<UINT>(launch_reveal_until_ - now));
        }
        timer_id_ = SetTimer(window_, requested_id, std::max<UINT>(10, delay), nullptr);
        if (timer_id_ == 0) {
            throw std::runtime_error("Cannot start the dock hover timer.");
        }
        timer_pending_ = pending;
        if (diagnostics_enabled_) {
            ++diagnostics_.timer_starts;
        }
    }

    bool pointer_inside_at(POINT cursor, HWND target) const noexcept
    {
        if (hidden_to_taskbar_) { return false; }
        if (target == window_) { return true; }
        if (!ScreenToClient(window_, &cursor)) { return false; }
        // A native tooltip can cover the hovered icon near a screen edge.
        // Treat only its overlap with an actual icon as owner hover. The tip
        // extending outside the launcher cannot keep the launcher revealed.
        const auto point = client_px_to_view_dip_point(cursor);
        if (target == tooltip_window_ && tooltip_active_ && icon_tooltips_permitted() &&
                std::any_of(icons_.begin(), icons_.end(),
                    [point](const IconVisual& icon) { return contains(icon.bounds, point); })) {
            return true;
        }
        RECT client{};
        if (peek_.state() != launcher::DockState::expanded ||
                !GetClientRect(window_, &client) || !PtInRect(&client, cursor)) {
            return false;
        }
        // A lower window can own this point only through a native cutout.
        // Keep those gaps, including rasterized sloping edges, within the
        // expanded hover footprint. An overlay above Launcher still owns hover.
        const HWND root = target == nullptr ? nullptr : GetAncestor(target, GA_ROOT);
        for (HWND above = GetWindow(window_, GW_HWNDPREV); root != nullptr && above != nullptr;
                above = GetWindow(above, GW_HWNDPREV)) {
            if (above == root) { return false; }
        }
        return true;
    }

    bool pointer_inside() const noexcept
    {
        POINT cursor{};
        return IsWindowVisible(window_) && GetCursorPos(&cursor) &&
            pointer_inside_at(cursor, WindowFromPoint(cursor));
    }

    void cancel_gap_tracking() noexcept
    {
        if (gap_tracking_) { KillTimer(window_, kGapPointerTimer); }
        gap_tracking_ = false;
    }

    void sync_gap_tracking()
    {
        POINT cursor{};
        const bool enabled = !smoke_mode_ && configuration_.dock.auto_hide &&
            !hidden_to_taskbar_ &&
            peek_.state() == launcher::DockState::expanded && !interaction_hold_ &&
            !menu_active_ && !drag_ && !display_layout_failed_ &&
            IsWindowVisible(window_) && GetCursorPos(&cursor);
        const HWND target = enabled ? WindowFromPoint(cursor) : nullptr;
        if (!enabled || target == window_ || !pointer_inside_at(cursor, target)) {
            cancel_gap_tracking();
        } else if (!gap_tracking_) {
            if (SetTimer(window_, kGapPointerTimer, 75, nullptr) == 0) {
                throw std::runtime_error("Cannot track departure from the transparent tab area.");
            }
            gap_tracking_ = true;
            if (diagnostics_enabled_) { ++diagnostics_.timer_starts; }
        }
    }

    void track_pointer()
    {
        if (mouse_tracked_ || hidden_to_taskbar_) {
            return;
        }
        TRACKMOUSEEVENT tracking{};
        tracking.cbSize = sizeof(tracking);
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = window_;
        mouse_tracked_ = TrackMouseEvent(&tracking) != FALSE;
    }

    void cancel_pointer_tracking() noexcept
    {
        if (mouse_tracked_) {
            TRACKMOUSEEVENT tracking{};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_CANCEL | TME_LEAVE;
            tracking.hwndTrack = window_;
            TrackMouseEvent(&tracking);
        }
        mouse_tracked_ = false;
    }

    void pointer_changed(bool inside)
    {
        if (display_layout_failed_ || hidden_to_taskbar_) { return; }
        peek_.pointer_changed(inside);
        // Re-entering a retreating sheet returns from its current position
        // immediately, without another hover dwell or a jump to either end.
        if (inside && motion_active_ && peek_.pending() == launcher::Pending::reveal) { peek_.timer_elapsed(); }
        apply_state();
        sync_timer();
    }

    struct SkippedOverlay final {
        HWND window{};
        DWORD process{};
        std::array<wchar_t, 128> class_name{};
        launcher::WindowTransparency transparency;
    };

    struct FullscreenSearch final {
        RECT monitor{};
        bool fullscreen = false;
        HWND candidate{};
        DWORD process{};
        RECT bounds{};
        std::array<wchar_t, 128> class_name{};
        bool ordinary_maximized = false;
        bool native_bounds = false;
        std::uint64_t visits = 0;
        SkippedOverlay overlay;
    };

    static BOOL CALLBACK find_display_main_window(HWND window, LPARAM parameter)
    {
        auto& search = *reinterpret_cast<FullscreenSearch*>(parameter);
        ++search.visits;
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        const auto extended_style = static_cast<DWORD_PTR>(GetWindowLongPtrW(window, GWL_EXSTYLE));
        if (process == GetCurrentProcessId() || !IsWindowVisible(window) || IsIconic(window) ||
            (extended_style & WS_EX_TOOLWINDOW) != 0) {
            return TRUE;
        }
        std::array<wchar_t, 128> name{};
        GetClassNameW(window, name.data(), static_cast<int>(name.size()));
        if (wcscmp(name.data(), L"Progman") == 0 || wcscmp(name.data(), L"WorkerW") == 0 ||
            wcscmp(name.data(), L"Shell_TrayWnd") == 0 ||
            wcscmp(name.data(), L"Shell_SecondaryTrayWnd") == 0 ||
            wcscmp(name.data(), L"#32768") == 0) {
            return TRUE;
        }
        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked,
                sizeof(cloaked))) && cloaked) {
            return TRUE;
        }
        RECT bounds{};
        const HRESULT bounds_result = DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS,
            &bounds, sizeof(bounds));
        const bool native_bounds = FAILED(bounds_result) ||
            bounds.right <= bounds.left || bounds.bottom <= bounds.top;
        if (native_bounds) {
            if (!GetWindowRect(window, &bounds) || bounds.right <= bounds.left ||
                bounds.bottom <= bounds.top) {
                return TRUE;
            }
        }
        RECT intersection{};
        if (!IntersectRect(&intersection, &bounds, &search.monitor)) {
            return TRUE;
        }
        launcher::WindowTransparency transparency;
        transparency.layered = (extended_style & WS_EX_LAYERED) != 0;
        transparency.click_through = (extended_style & WS_EX_TRANSPARENT) != 0;
        if (transparency.layered) {
            COLORREF color_key = 0;
            BYTE alpha = 255;
            DWORD flags = 0;
            transparency.attributes_known =
                GetLayeredWindowAttributes(window, &color_key, &alpha, &flags) != FALSE;
            if (transparency.attributes_known) {
                transparency.color_key = (flags & LWA_COLORKEY) != 0;
                transparency.alpha_blend = (flags & LWA_ALPHA) != 0;
                transparency.alpha = alpha;
            }
        }
        if (launcher::is_transparent_overlay(transparency)) {
            if (search.overlay.window == nullptr) {
                search.overlay = SkippedOverlay{window, process, name, transparency};
            }
            // A transparent overlay does not make the display fullscreen and
            // must not hide the opaque main window beneath it from this search.
            return TRUE;
        }
        const auto style = static_cast<DWORD_PTR>(GetWindowLongPtrW(window, GWL_STYLE));
        const bool ordinary_maximized = IsZoomed(window) && (style & WS_CAPTION) == WS_CAPTION;
        search.candidate = window;
        search.process = process;
        search.bounds = bounds;
        search.class_name = name;
        search.ordinary_maximized = ordinary_maximized;
        search.native_bounds = native_bounds;
        search.fullscreen = !ordinary_maximized &&
            bounds.left <= search.monitor.left + 1 && bounds.top <= search.monitor.top + 1 &&
            bounds.right >= search.monitor.right - 1 && bounds.bottom >= search.monitor.bottom - 1;
        // Stop at the first relevant main window on this display. A fullscreen
        // window lower in z-order may be covered and must not suppress the dock.
        return FALSE;
    }

    void log_overlay_change(const SkippedOverlay& overlay) const noexcept
    {
        const auto& current = overlay.transparency;
        const auto& previous = logged_overlay_.transparency;
        if (overlay.window == logged_overlay_.window && overlay.process == logged_overlay_.process &&
            current.layered == previous.layered && current.click_through == previous.click_through &&
            current.attributes_known == previous.attributes_known && current.color_key == previous.color_key &&
            current.alpha_blend == previous.alpha_blend && current.alpha == previous.alpha) {
            return;
        }
        logged_overlay_ = overlay;
        try {
            if (overlay.window == nullptr) {
                log_.write("No transparent overlay skipped before the display main window.");
                return;
            }
            const char* reason = current.click_through ? "click-through"
                : current.color_key ? "color-key" : "global-alpha";
            log_.write("Skipped transparent overlay: class=", utf8(overlay.class_name.data()),
                "; pid=", std::to_string(overlay.process), "; reason=", reason,
                "; layered=", std::to_string(current.layered), "; click-through=", std::to_string(current.click_through),
                "; attributes-known=", std::to_string(current.attributes_known),
                "; color-key=", std::to_string(current.color_key), "; alpha-enabled=", std::to_string(current.alpha_blend),
                "; alpha=", std::to_string(static_cast<unsigned>(current.alpha)), ".");
        } catch (...) {
            // This diagnostic has no effect on candidate selection.
        }
    }

    bool fullscreen_active() const noexcept
    {
        if (!configuration_.dock.suppress_fullscreen) {
            return false;
        }
        FullscreenSearch search{};
        search.monitor = monitor_.information.rcMonitor;
        EnumWindows(find_display_main_window, reinterpret_cast<LPARAM>(&search));
        log_overlay_change(search.overlay);
        if (diagnostics_enabled_) {
            ++diagnostics_.fullscreen_scans;
            diagnostics_.windows_visited += search.visits;
        }
        last_display_window_ = search.candidate;
        if (!candidate_log_initialized_ || search.candidate != logged_candidate_ ||
            search.process != logged_candidate_process_ ||
            search.fullscreen != logged_candidate_fullscreen_ ||
            search.ordinary_maximized != logged_candidate_maximized_ ||
            search.native_bounds != logged_candidate_native_bounds_) {
            candidate_log_initialized_ = true;
            logged_candidate_ = search.candidate;
            logged_candidate_process_ = search.process;
            logged_candidate_fullscreen_ = search.fullscreen;
            logged_candidate_maximized_ = search.ordinary_maximized;
            logged_candidate_native_bounds_ = search.native_bounds;
            try {
                const char* classification = search.candidate == nullptr ? "none"
                    : search.fullscreen ? "fullscreen"
                    : search.ordinary_maximized ? "ordinary-maximized" : "ordinary";
                log_.write("Fullscreen candidate: class=", utf8(search.class_name.data()),
                    "; pid=", std::to_string(search.process),
                    "; bounds=", rect_text(search.bounds),
                    "; classification=", classification,
                    "; bounds-source=", (search.native_bounds ? "native" : "dwm"), ".");
            } catch (...) {
                // Diagnostic formatting must not alter suppression behavior.
            }
        }
        return search.fullscreen;
    }

    void check_fullscreen()
    {
        if (diagnostics_enabled_) {
            ++diagnostics_.fullscreen_messages;
        }
        fullscreen_message_pending_ = false;
        if (interaction_hold_ || display_layout_failed_ || hidden_to_taskbar_) {
            // A settings dialog or an edge drag keeps its owner available even
            // if another application becomes full screen during the interaction.
            return;
        }
        const bool fullscreen = fullscreen_active();
        peek_.set_suppressed(fullscreen);
        apply_state();
        if (!fullscreen) {
            pointer_changed(pointer_inside());
        } else {
            sync_timer();
        }
    }

    bool fullscreen_event_relevant(DWORD event, HWND window, LONG object, LONG child) const noexcept
    {
        // Foreground changes can uncover a window on this display even when
        // the new foreground belongs to another display.
        if (event == EVENT_SYSTEM_FOREGROUND) { return true; }
        if (std::find(kFullscreenObjectEvents.begin(), kFullscreenObjectEvents.end(), event) == kFullscreenObjectEvents.end() ||
                child != CHILDID_SELF) { return false; }
        // Reordering top-level windows may be reported against their desktop
        // parent/client container rather than any one affected main window.
        if (event == EVENT_OBJECT_REORDER && (window == nullptr || window == GetDesktopWindow())) {
            return object == OBJID_WINDOW || object == OBJID_CLIENT;
        }
        if (object != OBJID_WINDOW) { return false; }
        // The selected window may already be destroyed, hidden, minimized or
        // moved away. Its old identity is enough to invalidate the selection.
        if (window != nullptr && window == last_display_window_) { return true; }
        // Removing a window that was not the first eligible one cannot change
        // which main window controls this display. In particular, do not turn
        // unrelated helper-window destruction into desktop enumeration.
        if (event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_HIDE) { return false; }
        if (window == nullptr || (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) != 0 ||
                GetAncestor(window, GA_ROOT) != window || !IsWindowVisible(window) || IsIconic(window)) { return false; }
        RECT bounds{};
        RECT intersection{};
        return GetWindowRect(window, &bounds) && IntersectRect(&intersection, &bounds, &monitor_.information.rcMonitor);
    }

    static void CALLBACK winevent(HWINEVENTHOOK, DWORD event, HWND window,
        LONG object, LONG child, DWORD, DWORD)
    {
        auto* self = event_target_;
        if (self == nullptr || self->window_ == nullptr) {
            return;
        }
        // Desktop-switch events need not identify a window or object. Handle
        // them before the fullscreen filters, and rebuild only on the UI thread.
        if (event == EVENT_SYSTEM_DESKTOPSWITCH) {
            if (!self->desktop_redraw_message_pending_) {
                self->desktop_redraw_message_pending_ =
                    PostMessageW(self->window_, kDesktopSwitchMessage, 0, 0) != FALSE;
            }
            return;
        }
        if (self->diagnostics_enabled_) {
            ++self->diagnostics_.winevents;
            switch (event) {
            case EVENT_SYSTEM_FOREGROUND: ++self->diagnostics_.foreground_events; break;
            case EVENT_OBJECT_LOCATIONCHANGE: ++self->diagnostics_.location_events; break;
            case EVENT_OBJECT_SHOW: ++self->diagnostics_.show_events; break;
            case EVENT_OBJECT_HIDE: ++self->diagnostics_.hide_events; break;
            case EVENT_OBJECT_DESTROY: ++self->diagnostics_.destroy_events; break;
            case EVENT_OBJECT_REORDER: ++self->diagnostics_.reorder_events; break;
            default: break;
            }
        }
        if (self->interaction_hold_ || self->display_layout_failed_ || self->hidden_to_taskbar_) {
            if (self->diagnostics_enabled_) { ++self->diagnostics_.fullscreen_filtered; }
            return;
        }
        if (self->fullscreen_message_pending_) {
            if (self->diagnostics_enabled_) { ++self->diagnostics_.fullscreen_coalesced; }
            return;
        }
        if (!self->fullscreen_event_relevant(event, window, object, child)) {
            if (self->diagnostics_enabled_) { ++self->diagnostics_.fullscreen_filtered; }
            return;
        }
        self->fullscreen_message_pending_ = true;
        if (!PostMessageW(self->window_, kFullscreenMessage, 0, 0)) {
            self->fullscreen_message_pending_ = false;
        }
    }

    void install_hooks()
    {
        event_target_ = this;
        constexpr DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
        desktop_hook_ = SetWinEventHook(EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH,
            nullptr, winevent, 0, 0, flags);
        if (desktop_hook_ == nullptr) {
            // Shell completion still provides recovery for Launcher-initiated UAC.
            log_.write("Desktop-switch redraw hook unavailable; launch-completion recovery remains enabled.");
        }
        if (!configuration_.dock.suppress_fullscreen) {
            return;
        }
        foreground_hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, winevent, 0, 0, flags);
        if (foreground_hook_ == nullptr) { throw std::runtime_error("Cannot monitor foreground full-screen windows."); }
        for (std::size_t index = 0; index < object_hooks_.size(); ++index) {
            const auto event = kFullscreenObjectEvents[index];
            object_hooks_[index] = SetWinEventHook(event, event, nullptr, winevent, 0, 0, flags);
            if (object_hooks_[index] == nullptr) { throw std::runtime_error("Cannot monitor full-screen window changes."); }
        }
    }

    void remove_hooks() noexcept
    {
        if (desktop_hook_ != nullptr) {
            UnhookWinEvent(desktop_hook_);
            desktop_hook_ = nullptr;
        }
        if (foreground_hook_ != nullptr) {
            UnhookWinEvent(foreground_hook_);
            foreground_hook_ = nullptr;
        }
        for (auto& hook : object_hooks_) {
            if (hook != nullptr) { UnhookWinEvent(hook); hook = nullptr; }
        }
        if (event_target_ == this) {
            event_target_ = nullptr;
        }
    }

    void refresh_display()
    {
        // Restore refreshes the selected display, including changes while hidden.
        if (arranging_ || display_layout_failed_ || hidden_to_taskbar_ || taskbar_transition_) {
            return;
        }
        if (drag_) {
            finish_drag(false);
        }
        cancel_motion();
        cancel_timer();
        cancel_gap_tracking();
        cancel_pointer_tracking();
        clear_icon_tooltips();
        ShowWindow(window_, SW_HIDE);
        monitor_ = select_monitor(false);
        arranging_ = true;
        SetWindowPos(window_, nullptr, monitor_.information.rcWork.left + 1,
            monitor_.information.rcWork.top + 1, 1, 1, SWP_NOACTIVATE | SWP_NOZORDER);
        arranging_ = false;
        dpi_ = GetDpiForWindow(window_);
        update_window_icons();
        try {
            effective_ = launcher::resolve_effective_settings(configuration_, runtime_,
                desktop_icons_.size_dip, dpi_);
            calculate_layout();
            update_placement(false);
        } catch (const launcher::ConfigError& error) {
            // A valid fixed gap may become impossible after losing a display or
            // receiving a smaller work area. Explain this before closing rather
            // than silently exiting or reducing the requested pixel spacing.
            display_layout_failed_ = true;
            cancel_timer();
            cancel_pointer_tracking();
            remove_hooks();
            log_.write("Display layout cannot fit; showing restart guidance: ", error.what());
            if (!smoke_mode_ && window_ != nullptr) {
                const std::wstring text = L"The launcher cannot fit on the selected display after "
                    L"the display or work area changed.\n\nIcon and panel-edge spacing is fixed at " +
                    std::to_wstring(effective_.icon_spacing_px) + L" pixels.\n" + utf16(error.what()) +
                    L"\n\nChoose another display or reduce the grid dimensions in:\n" +
                    config_path_.wstring() +
                    L"\n\nTo reduce spacing, edit icon_spacing_px in the runtime adjustments "
                    L"file if it contains a saved override:\n" + runtime_path_.wstring() +
                    L"\nOtherwise edit icon_spacing_px in the configuration above."
                    L"\n\nRestart Launcher after editing.";
                // The hidden 1x1 owner is already on the selected display.
                // Activation is limited to this actual failure notification.
                MessageBoxW(window_, text.c_str(), L"Launcher display layout",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
            }
            throw;
        }
        discard_device_resources();
        load_icons();
        peek_.pointer_changed(false);
        peek_.set_suppressed(!smoke_mode_ && !interaction_hold_ && fullscreen_active());
        apply_state(true);
        sync_timer();
        if (adjustment_dialog_ != nullptr) {
            SetDlgItemTextW(adjustment_dialog_, IDC_EFFECTIVE_SIZES, adjustment_description().c_str());
        }
    }

    void load_icons()
    {
        // Bound one page's decoded data as well as admitted workers. Extremely
        // large grids use a smaller raster, scaled by Direct2D at display time.
        const auto per_icon = (64ULL * 1024 * 1024) / std::max<std::size_t>(1, icons_.size());
        const int maximum = std::clamp(static_cast<int>(std::sqrt(static_cast<double>(per_icon / 4))), 1, 4096);
        const UINT size = static_cast<UINT>(std::clamp(pixels(drawn_icon_size_dip_ * fit_scale_, dpi_), 1, maximum));
        prepare_pending_overlay(size);
        for (auto& icon : icons_) {
            const auto& item = configuration_.tabs[active_tab_].items.at(icon.item_index);
            auto& record = item_presentations_[{active_tab_, icon.item_index}];
            if (record.adopted && icon.presentation_generation != record.adopted->generation) {
                bind_item_presentation(icon, *record.adopted);
            }
            if (!icon.image) {
                icon.label = cold_item_label(item);
                icon.image = fallback_item_image(size);
                icon.tooltip = icon.label.empty() ? L"" : icon.label + (versioned_item(item) ? L"\n" + version_description(item) : L"");
            }
            queue_item_presentation(icon, size);
        }
        trim_item_presentations();
        admit_item_presentations();
    }

    std::wstring cold_item_label(const launcher::Item& item) const
    {
        if (item.label_explicit || !item.label.empty() || !item.launch) { return utf16(item.label); }
        auto target = utf16(item.launch->target);
        // This path is deliberately lexical; no version resource or Shell
        // provider is consulted while the initial presentation is unavailable.
        if (item.launch->handler == "shell" && target.find(L':') == 1) {
            return std::filesystem::path(target).filename().wstring();
        }
        if (item.launch->handler == "shell" && target.find(L':') == std::wstring::npos) {
            const auto filename = std::filesystem::path(target).filename().wstring();
            if (!filename.empty()) { return filename; }
        }
        return target;
    }

    ComPtr<IWICBitmapSource> fallback_item_image(UINT size)
    {
        // Embedded, device-independent artwork: no filesystem or Shell access.
        if (fallback_item_size_ != size || !fallback_item_image_) {
            static constexpr std::string_view artwork =
                "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'>"
                "<rect x='3' y='4' width='26' height='24' rx='3' fill='#c5ccd4'/>"
                "<path d='M3 8V7a3 3 0 013-3h20a3 3 0 013 3v1z' fill='#667687'/>"
                "<path d='M8 13h16v10H8z' fill='#edf0f4'/></svg>";
            ComPtr<IWICBitmapSource> image;
            const HRESULT result = launcher::render_svg_data(wic_factory_.Get(), artwork, size, image);
            if (SUCCEEDED(result)) { fallback_item_image_ = std::move(image); fallback_item_size_ = size; }
            else { log_.result("Embedded pending icon failed", result); }
        }
        return fallback_item_image_;
    }

    void prepare_pending_overlay(UINT size)
    {
        const float extent = drawn_icon_size_dip_ * fit_scale_;
        const auto slats = launcher::pending_overlay_slats(extent);
        if (pending_overlay_size_ == size && pending_overlay_slats_ == slats) { return; }
        pending_overlay_size_ = size;
        pending_overlay_slats_ = slats;
        pending_overlay_image_.Reset();
        pending_overlay_bitmap_.Reset();
        const HRESULT result = launcher::render_pending_overlay(wic_factory_.Get(), extent, size, pending_overlay_image_);
        if (FAILED(result)) { log_.result("Pending version overlay unavailable", result); }
        if (drawing_target_ && pending_overlay_image_) {
            const HRESULT bitmap = drawing_target_->CreateBitmapFromWicBitmap(pending_overlay_image_.Get(), nullptr, pending_overlay_bitmap_.GetAddressOf());
            if (FAILED(bitmap)) { log_.result("Pending version overlay bitmap unavailable", bitmap); }
        }
    }

    bool pending_overlay_visible(std::size_t icon_index) const
    {
        if (highlighted_icon() != icon_index || icon_index >= icons_.size()) { return false; }
        const auto& item = configuration_.tabs[active_tab_].items.at(icons_[icon_index].item_index);
        const auto key = version_search(item);
        if (!key) { return false; }
        const auto shown = version_presentations_.find(*key);
        return shown != version_presentations_.end() && shown->second.activity != launcher::VersionDiscoveryActivity::idle;
    }

    void bind_item_presentation(IconVisual& icon, const ItemPresentationJob& job, bool reuse_image = false)
    {
        if (!job.result) { return; }
        const auto& value = *job.result;
        if (!reuse_image || !icon.image) {
            ComPtr<IWICBitmap> image;
            if (!value.pixels.empty()) {
                const HRESULT result = wic_factory_->CreateBitmapFromMemory(value.width, value.height, GUID_WICPixelFormat32bppPBGRA,
                    value.stride, static_cast<UINT>(value.pixels.size()), const_cast<BYTE*>(value.pixels.data()), image.GetAddressOf());
                if (FAILED(result)) { log_.result("Create owned presentation image", result); }
            }
            icon.image = image ? ComPtr<IWICBitmapSource>(image) : fallback_item_image(job.request.pixel_size);
            icon.bitmap.Reset();
        }
        icon.launch = value.launch;
        icon.label = value.label;
        icon.catalog = job.catalog;
        icon.versions = job.versions;
        icon.pinned_version = job.pinned_version;
        icon.can_admin = value.can_admin;
        icon.can_reveal = value.can_reveal;
        icon.presentation_generation = job.generation;
        const auto& item = configuration_.tabs[active_tab_].items.at(icon.item_index);
        icon.tooltip = icon.label.empty() ? L"" : icon.label + (versioned_item(item) ? L"\n" + version_description(item) : L"");
        if (drawing_target_ && icon.image && !icon.bitmap) {
            const HRESULT result = drawing_target_->CreateBitmapFromWicBitmap(icon.image.Get(), nullptr, icon.bitmap.GetAddressOf());
            if (FAILED(result)) { discard_device_resources(); }
        }
    }

    void queue_item_presentation(const IconVisual& icon, UINT size)
    {
        const auto& item = configuration_.tabs[active_tab_].items.at(icon.item_index);
        const auto pinned = default_item_version(item);
        std::shared_ptr<const std::vector<launcher::TargetVersion>> catalog;
        if (const auto key = version_search(item)) {
            const auto state = version_presentations_.find(*key);
            if (state != version_presentations_.end()) { catalog = state->second.snapshot; }
        }
        auto& record = item_presentations_[{active_tab_, icon.item_index}];
        bool startup_can_join = false;
        if (record.desired && record.desired->visit == 0) {
            const auto& startup = *record.desired;
            startup_can_join = !startup.ready.load(std::memory_order_acquire) ||
                (startup.result && startup.error.empty() && startup.result->diagnostics.empty() &&
                 (!startup.request.action || startup.result->launch.result.success));
        }
        if (record.desired && record.desired->catalog == catalog && record.desired->pinned_version == pinned && record.desired->request.pixel_size == size &&
                (record.desired->visit == presentation_visit_ || startup_can_join)) {
            // Startup preparation joins the first visible visit. Subsequent
            // visits refresh file-sensitive images, labels, and failed facts.
            record.desired->visit = presentation_visit_;
            return;
        }
        auto job = std::make_shared<ItemPresentationJob>();
        job->generation = ++record.generation;
        job->visit = presentation_visit_;
        job->catalog = std::move(catalog);
        job->pinned_version = pinned;
        if (item.launch) { job->versions = launcher::ItemVersionCatalog(*item.launch, job->catalog); }
        job->request = {item, {}, icon.source, config_directory_, size};
        if (item.launch) {
            if (!versioned_item(item)) { job->request.action = item.launch; }
            else if (job->catalog) {
                if (const auto selected = job->versions.select(pinned).version) { job->request.action = resolved_item_action(item, selected); }
            }
        }
        job->bytes = static_cast<std::uint64_t>(size) * size * 4;
        record.desired = std::move(job);
    }

    void admit_item_presentations()
    {
        if (restart_pending_ || version_view_rebuilding_ || window_ == nullptr || display_layout_failed_) { return; }
        constexpr std::size_t active_limit = 4;
        constexpr std::uint64_t pixel_limit = 64ULL * 1024 * 1024;
        for (const auto& icon : icons_) {
            auto& record = item_presentations_[{active_tab_, icon.item_index}];
            const auto job = record.desired;
            if (!job || record.running || job->ready.load(std::memory_order_acquire)) { continue; }
            if (active_presentations_ >= active_limit || admitted_presentation_bytes_ + job->bytes > pixel_limit) { continue; }
            record.running = job;
            ++active_presentations_;
            admitted_presentation_bytes_ += job->bytes;
            try {
                background_.start([job] {
                    try { job->result = launcher::prepare_item_presentation(job->request); }
                    catch (const std::exception& error) { job->error = error.what(); }
                    catch (...) { job->error = "Unknown presentation preparation failure."; }
                    job->ready.store(true, std::memory_order_release);
                });
            } catch (const std::exception& error) {
                job->error = error.what();
                job->ready.store(true, std::memory_order_release);
                --active_presentations_;
                admitted_presentation_bytes_ -= job->bytes;
                record.running.reset();
                log_.write("Item presentation could not start: ", job->error);
            }
        }
    }

    void collect_item_presentations()
    {
        for (auto& [binding, record] : item_presentations_) {
            const auto job = record.running;
            if (!job || !job->ready.load(std::memory_order_acquire)) { continue; }
            if (!job->error.empty()) { log_.write("Item presentation failed: ", job->error); }
            if (job->result) {
                for (const auto& diagnostic : job->result->diagnostics) { log_.write(diagnostic); }
            }
            --active_presentations_;
            admitted_presentation_bytes_ -= job->bytes;
            record.running.reset();
            if (binding.first != active_tab_ && record.desired == job) { record.desired.reset(); }
        }
        admit_item_presentations();
    }

    void adopt_item_presentations()
    {
        if (version_adoption_held()) { return; }
        struct AdoptionHold final { bool& value; ~AdoptionHold() { value = false; } } hold{version_adopting_};
        version_adopting_ = true;
        bool changed = false;
        for (auto& icon : icons_) {
            auto& record = item_presentations_[{active_tab_, icon.item_index}];
            const auto job = record.desired;
            if (!job || record.running == job || !job->ready.load(std::memory_order_acquire) || !job->result ||
                    icon.presentation_generation == job->generation) { continue; }
            const auto previous = record.adopted;
            const bool reuse_image = previous && previous->result && icon.image &&
                previous->result->width == job->result->width && previous->result->height == job->result->height &&
                previous->result->stride == job->result->stride && previous->result->pixels == job->result->pixels;
            if (!changed) { clear_icon_tooltips(false); }
            record.adopted = job;
            bind_item_presentation(icon, *job, reuse_image);
            changed = true;
        }
        if (changed) {
            sync_icon_tooltips();
            InvalidateRect(window_, nullptr, FALSE);
            trim_item_presentations();
        }
    }

    void trim_item_presentations()
    {
        constexpr std::uint64_t cache_limit = 64ULL * 1024 * 1024;
        std::uint64_t bytes = 0;
        for (auto& [binding, record] : item_presentations_) {
            if (binding.first != active_tab_) {
                if (record.desired != record.running) { record.desired.reset(); }
            }
            if (record.adopted && record.adopted->result) { bytes += record.adopted->result->pixels.size(); }
        }
        for (auto& [binding, record] : item_presentations_) {
            if (bytes <= cache_limit) { break; }
            if (binding.first == active_tab_ || !record.adopted) { continue; }
            bytes -= record.adopted->result ? record.adopted->result->pixels.size() : 0;
            record.adopted.reset();
        }
    }

    HRESULT create_device_resources()
    {
        if (preview_bitmap_ && drawing_target_) { return S_OK; }
        RECT client{};
        if (!GetClientRect(window_, &client)) { return HRESULT_FROM_WIN32(GetLastError()); }
        const auto size = D2D1::SizeU(
            static_cast<UINT32>(std::max<LONG>(1, client.right - client.left)),
            static_cast<UINT32>(std::max<LONG>(1, client.bottom - client.top)));
        if (!preview_bitmap_) {
            if (window_bitmap_ && drawing_target_) {
                const auto cached = drawing_target_->GetPixelSize();
                if (cached.width == size.width && cached.height == size.height) { return S_OK; }
                discard_device_resources();
            }
            HRESULT result = wic_factory_->CreateBitmap(size.width, size.height, GUID_WICPixelFormat32bppPBGRA,
                WICBitmapCacheOnLoad, window_bitmap_.ReleaseAndGetAddressOf());
            if (FAILED(result)) { return result; }
        }
        IWICBitmap* bitmap = preview_bitmap_ ? preview_bitmap_.Get() : window_bitmap_.Get();
        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(dpi_), static_cast<float>(dpi_));
        HRESULT result = d2d_factory_->CreateWicBitmapRenderTarget(bitmap, properties, drawing_target_.GetAddressOf());
        if (FAILED(result)) {
            return result;
        }
        result = drawing_target_->CreateSolidColorBrush(D2D1::ColorF(0xC98084),
            outline_.GetAddressOf());
        if (SUCCEEDED(result)) {
            result = drawing_target_->CreateSolidColorBrush(D2D1::ColorF(0x131313),
                text_brush_.GetAddressOf());
        }
        if (FAILED(result)) {
            discard_device_resources();
            return result;
        }
        for (auto& icon : icons_) {
            if (icon.image) {
                result = drawing_target_->CreateBitmapFromWicBitmap(icon.image.Get(), nullptr,
                    icon.bitmap.GetAddressOf());
                if (FAILED(result)) {
                    log_.result("Direct2D icon bitmap failed", result);
                    discard_device_resources();
                    return result;
                }
            }
        }
        if (pending_overlay_image_) {
            const HRESULT overlay = drawing_target_->CreateBitmapFromWicBitmap(pending_overlay_image_.Get(), nullptr, pending_overlay_bitmap_.GetAddressOf());
            if (FAILED(overlay)) { log_.result("Pending version overlay bitmap unavailable", overlay); }
        }
        return S_OK;
    }

    void discard_device_resources()
    {
        for (auto& icon : icons_) {
            icon.bitmap.Reset();
        }
        pending_overlay_bitmap_.Reset();
        outline_.Reset();
        text_brush_.Reset();
        surfaces_.clear();
        drawing_target_.Reset();
        window_bitmap_.Reset();
    }

    HRESULT gradient(std::span<const D2D1_GRADIENT_STOP> stops,
        D2D1_POINT_2F first, D2D1_POINT_2F last,
        ComPtr<ID2D1LinearGradientBrush>& brush)
    {
        ComPtr<ID2D1GradientStopCollection> collection;
        HRESULT result = drawing_target_->CreateGradientStopCollection(stops.data(),
            static_cast<UINT32>(stops.size()), collection.GetAddressOf());
        if (SUCCEEDED(result)) {
            result = drawing_target_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(first, last),
                collection.Get(), brush.GetAddressOf());
        }
        return result;
    }

    HRESULT polygon_geometry(std::span<const D2D1_POINT_2F> points, ComPtr<ID2D1PathGeometry>& geometry)
    {
        HRESULT result = d2d_factory_->CreatePathGeometry(geometry.GetAddressOf());
        if (FAILED(result)) {
            return result;
        }
        ComPtr<ID2D1GeometrySink> sink;
        result = geometry->Open(sink.GetAddressOf());
        if (FAILED(result)) {
            return result;
        }
        sink->BeginFigure(points.front(),
            D2D1_FIGURE_BEGIN_FILLED);
        for (std::size_t index = 1; index < points.size(); ++index) {
            sink->AddLine(points[index]);
        }
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        result = sink->Close();
        return result;
    }

    HRESULT surface_brush(std::size_t index, ComPtr<ID2D1LinearGradientBrush>& brush)
    {
        const auto& tab = tabs_[index];
        const bool active = index == active_tab_;
        const auto points = tab_canvas_dip_points(tab);
        std::vector<D2D1_POINT_2F> outline(points.begin(), points.end());
        if (active) {
            // The active head and page share one face, without an internal edge.
            const float far_edge = tab_depth_dip_ + panel_depth_dip_;
            outline = {points[0], points[1], points[2], points[3],
                layout_to_canvas_dip_point(parallel_span_dip_, tab_depth_dip_),
                layout_to_canvas_dip_point(parallel_span_dip_, far_edge),
                layout_to_canvas_dip_point(0.0F, far_edge), layout_to_canvas_dip_point(0.0F, tab_depth_dip_), points[4], points[5]};
        }
        surfaces_.resize(tabs_.size());
        auto& cached = surfaces_[index];
        const auto& ramp = active ? tab.colors->page.gradient : tab.colors->tab.gradient;
        const bool same_outline = std::equal(outline.begin(), outline.end(), cached.outline.begin(), cached.outline.end(),
            [](D2D1_POINT_2F first, D2D1_POINT_2F second) { return first.x == second.x && first.y == second.y; });
        if (!cached.brush || !same_outline || cached.stops != ramp) {
            const auto started = std::chrono::steady_clock::now();
            std::vector<D2D1_GRADIENT_STOP> stops;
            stops.reserve(ramp.size());
            for (const auto& stop : ramp) { stops.push_back({stop.at, color_rgb(stop.rgb)}); }
            ComPtr<ID2D1LinearGradientBrush> candidate;
            HRESULT result = S_OK;
            if (active) {
                // Keep the tab and join at the clean first color; the oblique
                // gradient reaches across the page toward its far edge.
                const float ax = 0.35F / parallel_span_dip_;
                const float ay = 1.0F / panel_depth_dip_;
                const float inverse_length_squared = 1.0F / (ax * ax + ay * ay);
                const float start_parallel = parallel_span_dip_ * 0.5F;
                const float start_depth = tab_depth_dip_ + panel_depth_dip_ * 0.18F;
                result = gradient(stops, layout_to_canvas_dip_point(start_parallel, start_depth),
                    layout_to_canvas_dip_point(start_parallel + ax * inverse_length_squared,
                        start_depth + ay * inverse_length_squared), candidate);
            } else {
                result = gradient(stops, layout_to_canvas_dip_point(0.0F, tab.logical_bounds.top),
                    layout_to_canvas_dip_point(0.0F, tab.logical_bounds.bottom), candidate);
            }
            if (FAILED(result)) { return result; }
            cached.brush = std::move(candidate);
            cached.outline = std::move(outline);
            cached.stops = ramp;
            ++surface_builds_;
            surface_build_elapsed_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        }
        brush = cached.brush;
        return S_OK;
    }

    HRESULT draw_prism(std::size_t index)
    {
        ComPtr<ID2D1LinearGradientBrush> face_brush;
        HRESULT result = surface_brush(index, face_brush);
        if (FAILED(result)) { return result; }
        const auto& surface = surfaces_[index];
        const auto& points = surface.outline;
        const auto orientation = canvas_to_view_transform();
        std::vector<D2D1_POINT_2F> view_points;
        view_points.reserve(points.size());
        for (const auto point : points) { view_points.push_back(orientation.TransformPoint(point)); }
        float area = 0.0F;
        for (std::size_t edge = 0; edge < points.size(); ++edge) {
            const auto a = view_points[edge];
            const auto b = view_points[(edge + 1) % points.size()];
            area += a.x * b.y - b.x * a.y;
        }
        const float thickness = sheet_thickness_dip();
        const auto direction = prism_view_direction();
        const auto extrusion = view_to_canvas_vector(direction.x * thickness, direction.y * thickness);
        const auto clean = color_rgb(surface.stops.front().rgb);
        // Classify in view coordinates so the same two adjacent walls are
        // visible on top, bottom, left, and right dock orientations.
        for (std::size_t edge = 0; thickness > 0.0F && edge < points.size(); ++edge) {
            const std::size_t next = (edge + 1) % points.size();
            const float dx = view_points[next].x - view_points[edge].x;
            const float dy = view_points[next].y - view_points[edge].y;
            const float length = std::hypot(dx, dy);
            if (length < 0.001F) { continue; }
            const float winding = area > 0.0F ? 1.0F : -1.0F;
            const float nx = winding * dy / length;
            const float ny = -winding * dx / length;
            if (nx * direction.x + ny * direction.y <= 0.001F) {
                // These sides recede from the viewer: show their shadow outside
                // the face, rather than pretending their wall is also visible.
                for (const auto& [width, opacity] : std::array<std::pair<float, float>, 3>{{
                        {6.0F, 0.035F}, {3.5F, 0.065F}, {1.5F, 0.12F}}}) {
                    const auto offset = view_to_canvas_vector(nx * thickness * 0.45F, ny * thickness * 0.45F);
                    outline_->SetColor(D2D1::ColorF(D2D1::ColorF::Black, opacity));
                    drawing_target_->DrawLine(D2D1::Point2F(points[edge].x + offset.x, points[edge].y + offset.y),
                        D2D1::Point2F(points[next].x + offset.x, points[next].y + offset.y), outline_.Get(), width * thickness * 0.5F);
                }
            } else {
                // Extrusion joins corresponding vertices of two congruent
                // outlines. The wall has constant thickness and a flat color.
                const std::array<D2D1_POINT_2F, 4> wall{{points[edge], points[next],
                    D2D1::Point2F(points[next].x + extrusion.x, points[next].y + extrusion.y),
                    D2D1::Point2F(points[edge].x + extrusion.x, points[edge].y + extrusion.y)}};
                ComPtr<ID2D1PathGeometry> wall_geometry;
                result = polygon_geometry(wall, wall_geometry);
                if (FAILED(result)) { return result; }
                const float shade = 0.80F - 0.18F * std::max(0.0F, ny);
                outline_->SetColor(D2D1::ColorF(clean.r * shade, clean.g * shade, clean.b * shade));
                drawing_target_->FillGeometry(wall_geometry.Get(), outline_.Get());
            }
        }
        ComPtr<ID2D1PathGeometry> face_geometry;
        result = polygon_geometry(points, face_geometry);
        if (FAILED(result)) { return result; }
        drawing_target_->FillGeometry(face_geometry.Get(), face_brush.Get());
        if (hovered_tab_ == index) {
            // The selected face includes the entire page. Tint only its tab;
            // inactive tabs keep their full visible stem and normal overlap.
            ComPtr<ID2D1PathGeometry> tab_geometry;
            const auto head = tab_canvas_dip_points(tabs_[index]);
            result = polygon_geometry(head, tab_geometry);
            if (FAILED(result)) { return result; }
            outline_->SetColor(color_rgb(tabs_[index].colors->tab.hover));
            drawing_target_->FillGeometry(tab_geometry.Get(), outline_.Get());
        }
        outline_->SetColor(color_rgb(tabs_[index].colors->outline));
        if (thickness > 0.0F) {
            drawing_target_->DrawGeometry(face_geometry.Get(), outline_.Get(), 96.0F / (static_cast<float>(dpi_) * fit_scale_));
        }
        draw_tab_label(tabs_[index]);
        return S_OK;
    }

    void draw_tab_label(const TabVisual& tab)
    {
        const auto head = layout_to_canvas_dip_rect(tab.logical_bounds);
        const auto label = tab_label_bounds(head);
        D2D1::Matrix3x2F original;
        drawing_target_->GetTransform(&original);
        drawing_target_->SetTransform(tab_text_transform(head) * original);
        text_brush_->SetColor(color_rgb(tab.colors->text));
        drawing_target_->DrawTextW(tab.label.c_str(), static_cast<UINT32>(tab.label.size()),
            text_format_.Get(), label, text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        drawing_target_->SetTransform(original);
    }

    HRESULT render()
    {
        if (motion_active_ && motion_frame_) {
            return layered_frame_.present(window_, motion_frame_.Get(), &motion_crop_);
        }
        const HRESULT result = render_frame();
        // Resource creation can fail before EndDraw. Every such recreation
        // error must release the stale target before the next paint retries.
        if (result == D2DERR_RECREATE_TARGET) { discard_device_resources(); }
        return result;
    }

    D2D1::Matrix3x2F frame_viewport_transform() const noexcept
    {
        const float pixel_to_dip = 96.0F / static_cast<float>(dpi_);
        return D2D1::Matrix3x2F::Scale(fit_scale_, fit_scale_) *
            D2D1::Matrix3x2F::Translation(-static_cast<float>(render_offset_px_.x) * pixel_to_dip,
                -static_cast<float>(render_offset_px_.y) * pixel_to_dip);
    }

    HRESULT draw_page_surfaces(const D2D1::Matrix3x2F& viewport)
    {
        // Rotate the whole drawing coordinate system; icon artwork is a separate
        // upright pass at the rotated grid-cell destinations below.
        drawing_target_->SetTransform(canvas_to_view_transform() * viewport);
        drawing_target_->Clear(D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F));
        for (const auto index : tab_draw_order_) {
            const HRESULT result = draw_prism(index);
            if (FAILED(result)) { return result; }
        }
        return S_OK;
    }

    std::optional<std::size_t> highlighted_icon() const noexcept
    {
        return menu_icon_ ? menu_icon_ : hovered_icon_;
    }

    void draw_icon_hover()
    {
        const auto index = highlighted_icon();
        if (!index || *index >= icons_.size()) { return; }
        const auto& bounds = icons_[*index].bounds;
        const float padding = launcher::icon_hover_padding_px(effective_.icon_spacing_px) *
            96.0F / static_cast<float>(dpi_) / fit_scale_;
        const auto fill = D2D1::RectF(bounds.left - padding, bounds.top - padding,
            bounds.right + padding, bounds.bottom + padding);
        const float radius = std::min(3.0F, drawn_icon_size_dip_ * 0.125F);
        outline_->SetColor(color_rgb(configuration_.tabs[active_tab_].colors.page.hover));
        drawing_target_->FillRoundedRectangle(D2D1::RoundedRect(fill, radius, radius), outline_.Get());
    }

    void draw_version_choices_badge(const IconVisual& icon)
    {
        // The badge stays upright with the artwork on every dock edge.
        const float extent = std::min(icon.bounds.right - icon.bounds.left, icon.bounds.bottom - icon.bounds.top);
        const float pixel = 96.0F / static_cast<float>(dpi_) / fit_scale_;
        const float outset = effective_.icon_spacing_px >= 1 ? pixel : 0.0F;
        const float left = icon.bounds.left - outset;
        const float top = icon.bounds.top - outset;
        const auto badge = D2D1::RectF(left, top, left + extent * 0.375F, top + extent * 0.21875F);
        const float radius = extent * 0.03125F;
        outline_->SetColor(D2D1::ColorF(0x202020, 0.8F));
        drawing_target_->FillRoundedRectangle(D2D1::RoundedRect(badge, radius, radius), outline_.Get());
        outline_->SetColor(D2D1::ColorF(D2D1::ColorF::White));
        const float center_x = (badge.left + badge.right) * 0.5F;
        const float center_y = (badge.top + badge.bottom) * 0.5F;
        // Preserve the rectangle's proportions while keeping the dots legible
        // at small sizes. The height limit also covers extreme fitting scales.
        const float dot_radius = std::min(extent * 0.046875F, std::max(extent * 0.0234375F, pixel));
        for (int dot = -1; dot <= 1; ++dot) {
            const auto center = D2D1::Point2F(center_x + static_cast<float>(dot) * extent * 0.09375F, center_y);
            drawing_target_->FillEllipse(D2D1::Ellipse(center, dot_radius, dot_radius), outline_.Get());
        }
    }

    void draw_icons()
    {
        for (std::size_t index = 0; index < icons_.size(); ++index) {
            const auto& icon = icons_[index];
            if (icon.bitmap) {
                drawing_target_->DrawBitmap(icon.bitmap.Get(), icon.bounds, 1.0F,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
            if (pending_overlay_bitmap_ && pending_overlay_visible(index)) {
                drawing_target_->DrawBitmap(pending_overlay_bitmap_.Get(), icon.bounds, 1.0F, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
            if (highlighted_icon() == index && icon.versions.multiple()) {
                draw_version_choices_badge(icon);
            }
        }
    }

    HRESULT render_frame()
    {
        const HRESULT resources = create_device_resources();
        if (FAILED(resources)) { return resources; }

        drawing_target_->BeginDraw();
        const auto viewport = frame_viewport_transform();
        const HRESULT surfaces = draw_page_surfaces(viewport);
        drawing_target_->SetTransform(viewport);
        draw_icon_hover();
        draw_icons();
        const HRESULT draw_result = drawing_target_->EndDraw();
        if (FAILED(draw_result)) { return draw_result; }
        if (FAILED(surfaces)) { return surfaces; }
        return window_bitmap_ ? layered_frame_.present(window_, window_bitmap_.Get()) : S_OK;
    }

    // Undo viewport clipping/fitting and DPI scaling; leave orientation applied
    // because icon and tab hit bounds are stored in full-view DIPs.
    D2D1_POINT_2F client_px_to_view_dip_point(POINT client) const noexcept
    {
        const float scale = 96.0F / static_cast<float>(dpi_) / fit_scale_;
        return D2D1::Point2F(static_cast<float>(client.x + render_offset_px_.x) * scale,
            static_cast<float>(client.y + render_offset_px_.y) * scale);
    }

    std::optional<std::size_t> tab_at(POINT client) const noexcept
    {
        if (hidden_to_taskbar_ || peek_.state() == launcher::DockState::suppressed || tabs_.empty()) {
            return std::nullopt;
        }
        auto point = client_px_to_view_dip_point(client);
        // Native regions and alpha coverage describe pixels. Test their
        // centers so a one-pixel wall does not fall on a rounded face edge.
        const float half_pixel = 0.5F * 96.0F / static_cast<float>(dpi_) / fit_scale_;
        point.x += half_pixel;
        point.y += half_pixel;
        if (contains(panel_bounds_, point)) { return std::nullopt; }
        const auto hit = [&](std::size_t index) {
            if (index < surfaces_.size() && surfaces_[index].brush) {
                const auto orientation = canvas_to_view_transform();
                auto inverse = orientation;
                inverse.Invert();
                const auto canvas = inverse.TransformPoint(point);
                const auto& outline = surfaces_[index].outline;
                const auto inside = [canvas](std::span<const D2D1_POINT_2F> polygon) {
                    bool enclosed = false;
                    for (std::size_t edge = 0; edge < polygon.size(); ++edge) {
                        const auto a = polygon[edge];
                        const auto b = polygon[(edge + 1) % polygon.size()];
                        if ((a.y > canvas.y) != (b.y > canvas.y) &&
                                canvas.x < a.x + (canvas.y - a.y) * (b.x - a.x) / (b.y - a.y)) {
                            enclosed = !enclosed;
                        }
                    }
                    return enclosed;
                };
                if (inside(outline)) { return true; }
                const float thickness = sheet_thickness_dip();
                if (thickness == 0.0F) { return false; }
                float area = 0.0F;
                for (std::size_t edge = 0; edge < outline.size(); ++edge) {
                    const auto a = orientation.TransformPoint(outline[edge]);
                    const auto b = orientation.TransformPoint(outline[(edge + 1) % outline.size()]);
                    area += a.x * b.y - b.x * a.y;
                }
                const auto direction = prism_view_direction();
                const auto extrusion = view_to_canvas_vector(direction.x * thickness, direction.y * thickness);
                for (std::size_t edge = 0; edge < outline.size(); ++edge) {
                    const auto a = outline[edge];
                    const auto b = outline[(edge + 1) % outline.size()];
                    const auto first = orientation.TransformPoint(a);
                    const auto last = orientation.TransformPoint(b);
                    const float dx = last.x - first.x;
                    const float dy = last.y - first.y;
                    if ((area > 0.0F ? 1.0F : -1.0F) * (dy * direction.x - dx * direction.y) <=
                            0.001F * std::hypot(dx, dy)) { continue; }
                    const std::array<D2D1_POINT_2F, 4> wall{{a, b,
                        D2D1::Point2F(b.x + extrusion.x, b.y + extrusion.y),
                        D2D1::Point2F(a.x + extrusion.x, a.y + extrusion.y)}};
                    if (inside(wall)) { return true; }
                }
                return false;
            }
            return false;
        };
        // The foremost painted face owns the click, including outer stems
        // exposed beside partial rows or behind dog-eared corners.
        for (auto entry = tab_draw_order_.rbegin(); entry != tab_draw_order_.rend(); ++entry) {
            if (hit(*entry)) { return *entry; }
        }
        return std::nullopt;
    }

    bool select_tab(std::size_t index)
    {
        if (index >= configuration_.tabs.size() || hidden_to_taskbar_ || interaction_hold_ || menu_active_ || drag_ ||
                version_gesture_dispatch_ || version_adopting_ ||
                peek_.state() == launcher::DockState::suppressed) {
            return false;
        }
        if (index == active_tab_) {
            return true;
        }
        std::wstring error;
        if (!rebuild_view(runtime_, index, error)) { return false; }
        sync_version_activation();
        log_.write("Tab selected: index=", std::to_string(active_tab_),
            "; name=", configuration_.tabs[active_tab_].name,
            "; icon-count=", std::to_string(icons_.size()), ".");
        return true;
    }

    bool blank_panel_point(POINT client) const noexcept
    {
        if (peek_.state() != launcher::DockState::expanded) {
            return false;
        }
        const auto point = client_px_to_view_dip_point(client);
        return contains(panel_bounds_, point) && std::none_of(icons_.begin(), icons_.end(),
            [point](const IconVisual& icon) { return contains(icon.bounds, point); });
    }

    std::optional<std::size_t> icon_at(POINT client) const noexcept
    {
        if (!icon_tooltips_permitted()) { return std::nullopt; }
        const auto point = client_px_to_view_dip_point(client);
        for (std::size_t index = 0; index < icons_.size(); ++index) {
            if (contains(icons_[index].bounds, point)) { return index; }
        }
        return std::nullopt;
    }

    HRESULT verify_hover(bool expanded = true)
    {
        // Earlier adjustment previews may still have optional image work.
        // A hover test starts after preparation, so pointer motion is the only
        // change whose drawing/cache effects are being measured here.
        if (!settle_version_discovery()) { return E_FAIL; }
        const auto fail = [this](const char* message) { log_.write(message); clear_hover(); return E_FAIL; };
        const auto client = [this](float x, float y) {
            return POINT{pixels(x * fit_scale_, dpi_) - render_offset_px_.x,
                pixels(y * fit_scale_, dpi_) - render_offset_px_.y};
        };
        const auto move = [this](POINT point) {
            SendMessageW(window_, WM_MOUSEMOVE, 0, MAKELPARAM(point.x, point.y));
        };
        const auto pixel = [this](POINT point, std::array<BYTE, 4>& value) {
            const WICRect rectangle{point.x, point.y, 1, 1};
            return window_bitmap_->CopyPixels(&rectangle, 4, 4, value.data());
        };
        // Optional artifact output for visual inspection; ordinary smoke runs
        // neither export files nor write to the desktop user's Pictures folder.
        const auto export_frame = [this, expanded](const wchar_t* name) -> HRESULT {
            if (!expanded) { return S_OK; }
            std::array<wchar_t, MAX_PATH> directory{};
            const auto length = GetEnvironmentVariableW(L"LAUNCHER_HOVER_SMOKE_DIR", directory.data(),
                static_cast<DWORD>(directory.size()));
            if (!length) { return S_OK; }
            if (length >= directory.size()) { return E_INVALIDARG; }
            return launcher::write_png(wic_factory_.Get(), window_bitmap_.Get(),
                std::filesystem::path(directory.data()) / name, dpi_, files_);
        };
        clear_hover();
        HRESULT result = render();
        if (FAILED(result)) { return result; }
        const auto builds = surface_builds_;
        bool version_hover_exported = false;
        for (std::size_t index = 0; expanded && index < icons_.size(); ++index) {
            const auto& icon = icons_[index];
            const auto& bounds = icon.bounds;
            const auto& item = configuration_.tabs[active_tab_].items[icon.item_index];
            const bool launchable = item.launch || item.shift_launch;
            auto* image = icon.image.Get();
            auto* bitmap = icon.bitmap.Get();
            const auto center = client((bounds.left + bounds.right) * 0.5F, (bounds.top + bounds.bottom) * 0.5F);
            const auto gap = client(bounds.left - icon_gap_dip_ * 0.25F, (bounds.top + bounds.bottom) * 0.5F);
            const float padding = launcher::icon_hover_padding_px(effective_.icon_spacing_px) *
                96.0F / static_cast<float>(dpi_) / fit_scale_;
            const auto frame_pixel = client(bounds.left - padding, (bounds.top + bounds.bottom) * 0.5F);
            const bool test_gap = effective_.icon_spacing_px >= 4;
            std::array<BYTE, 4> before{}, during{}, after{};
            if (test_gap && FAILED(pixel(frame_pixel, before))) { return E_FAIL; }
            move(center);
            if (hovered_icon_ != (launchable ? std::optional(index) : std::nullopt) || hovered_tab_) {
                return fail("Smoke hover failed: launchable or decorative cell mismatch.");
            }
            result = render();
            if (FAILED(result)) { return result; }
            if (launchable) {
                if (test_gap) {
                    const auto rgb = configuration_.tabs[active_tab_].colors.page.hover;
                    const std::array<BYTE, 4> expected{static_cast<BYTE>(rgb), static_cast<BYTE>(rgb >> 8),
                        static_cast<BYTE>(rgb >> 16), 255};
                    if (FAILED(pixel(frame_pixel, during)) || during != expected) {
                        return fail("Smoke hover failed: palette color did not reach the requested frame width.");
                    }
                }
                if (index == 0 && FAILED(export_frame(L"icon-hover.png"))) { return E_FAIL; }
                if (icon.versions.multiple() && !version_hover_exported) {
                    if (FAILED(export_frame(L"version-choices-hover.png"))) { return E_FAIL; }
                    version_hover_exported = true;
                }
            }
            if (test_gap) {
                move(gap);
                if (hovered_icon_ || hovered_tab_ || icon_at(gap) || !blank_panel_point(gap)) {
                    return fail("Smoke hover failed: gap acquired an icon action or lost page context.");
                }
                if (FAILED(render()) || FAILED(pixel(frame_pixel, after)) || before != after) {
                    return fail("Smoke hover failed: leaving the cell did not restore the background.");
                }
            }
            if (icon.image.Get() != image || icon.bitmap.Get() != bitmap || surface_builds_ != builds) {
                return fail("Smoke hover failed: pointer movement rebuilt image or gradient resources.");
            }
        }
        for (std::size_t index = 0; index < tabs_.size(); ++index) {
            const auto& bounds = tabs_[index].logical_bounds;
            const auto point = layout_to_view_dip_point((bounds.left + bounds.right) * 0.5F,
                (bounds.top + bounds.bottom) * 0.5F);
            const auto hit = client(point.x, point.y);
            move(hit);
            auto expected_tab = tab_at(hit);
            if (expected_tab == active_tab_) { expected_tab.reset(); }
            if (hovered_tab_ != expected_tab || hovered_icon_) {
                return fail("Smoke hover failed: tab hover differed from visible foreground hit testing.");
            }
            if (FAILED(render())) { return E_FAIL; }
            if (index == active_tab_ && FAILED(export_frame(L"active-tab.png"))) { return E_FAIL; }
            if (index == 1 && FAILED(export_frame(L"tab-hover.png"))) { return E_FAIL; }
        }
        menu_active_ = true;
        update_hover(POINT{});
        menu_active_ = false;
        if (hovered_icon_ || hovered_tab_) { return fail("Smoke hover failed: context menu retained a highlight."); }
        const auto source = std::find_if(icons_.begin(), icons_.end(), [this](const auto& icon) {
            const auto& item = configuration_.tabs[active_tab_].items[icon.item_index];
            return item.launch || item.shift_launch;
        });
        if (expanded && source != icons_.end()) {
            const auto index = static_cast<std::size_t>(source - icons_.begin());
            menu_icon_ = index;
            menu_active_ = true;
            clear_icon_tooltips();
            update_hover(POINT{});
            sync_hover();
            const bool retained = !hovered_icon_ && !hovered_tab_ && highlighted_icon() == index;
            result = render();
            if (SUCCEEDED(result) && effective_.icon_spacing_px >= 4) {
                const float padding = launcher::icon_hover_padding_px(effective_.icon_spacing_px) *
                    96.0F / static_cast<float>(dpi_) / fit_scale_;
                const auto point = client(source->bounds.left - padding, (source->bounds.top + source->bounds.bottom) * 0.5F);
                const auto rgb = configuration_.tabs[active_tab_].colors.page.hover;
                const std::array<BYTE, 4> expected{static_cast<BYTE>(rgb), static_cast<BYTE>(rgb >> 8), static_cast<BYTE>(rgb >> 16), 255};
                std::array<BYTE, 4> actual{};
                if (FAILED(pixel(point, actual)) || actual != expected) { result = E_FAIL; }
            }
            if (SUCCEEDED(result)) { result = export_frame(L"icon-menu-highlight.png"); }
            menu_icon_.reset();
            menu_active_ = false;
            sync_icon_tooltips();
            if (!retained || highlighted_icon() || FAILED(result)) {
                return fail("Smoke hover failed: icon menu lost its source highlight or retained it after dismissal.");
            }
        }
        if (surface_builds_ != builds) { return fail("Smoke hover failed: tab hover rebuilt gradient resources."); }
        result = render();
        if (SUCCEEDED(result)) { result = export_frame(L"normal.png"); }
        log_.write("Smoke hover passed: launchable/decorative cells, pixel-sized frame, unchanged gap/page hit tests, "
            "inactive tab faces, neutral active tab, menu source highlight retention/dismissal, cached icons/gradients; no activation or real launch.");
        return result;
    }

    void clear_hover() noexcept
    {
        const bool changed = hovered_icon_.has_value() || hovered_tab_.has_value();
        hovered_icon_.reset();
        hovered_tab_.reset();
        if (changed && window_) { InvalidateRect(window_, nullptr, FALSE); }
    }

    void update_hover(POINT client)
    {
        if (hidden_to_taskbar_ || interaction_hold_ || menu_active_ || display_layout_failed_ ||
                arranging_ || motion_active_ || drag_ || peek_.state() == launcher::DockState::suppressed) {
            clear_hover();
            return;
        }
        auto tab = tab_at(client);
        if (tab == active_tab_) { tab.reset(); }
        auto icon = icon_at(client);
        if (icon) {
            const auto& item = configuration_.tabs[active_tab_].items[icons_[*icon].item_index];
            if (!item.launch && !item.shift_launch) { icon.reset(); }
        }
        if (tab != hovered_tab_ || icon != hovered_icon_) {
            hovered_tab_ = tab;
            hovered_icon_ = icon;
            if (icon) { request_item_versions(configuration_.tabs[active_tab_].items[icons_[*icon].item_index]); }
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    void sync_hover()
    {
        POINT point{};
        if (GetCursorPos(&point)) {
            const auto owner = WindowFromPoint(point);
            if ((owner == window_ || (owner == tooltip_window_ && tooltip_active_)) &&
                    ScreenToClient(window_, &point)) {
                update_hover(point);
                return;
            }
        }
        clear_hover();
    }

    bool versioned_item(const launcher::Item& item) const
    {
        return version_search(item).has_value();
    }

    std::optional<launcher::VersionSearchKey> version_search(const launcher::Item& item) const
    {
        if (!item.launch) { return std::nullopt; }
        if (item.launch->handler == "adobeCC") { return launcher::adobe_cc_search_key(*item.launch, config_directory_); }
        if (const auto pattern = launcher::version_target_pattern(*item.launch, config_directory_)) {
            auto identity = launcher::normalize_target_version_pattern(*pattern).native();
            CharLowerBuffW(identity.data(), static_cast<DWORD>(identity.size()));
            return launcher::VersionSearchKey{launcher::VersionSearchKind::target_pattern, {std::move(identity)}, 1};
        }
        return std::nullopt;
    }

    std::vector<launcher::VersionSearchKey> page_version_searches() const
    {
        std::vector<launcher::VersionSearchKey> keys;
        for (const auto& item : configuration_.tabs[active_tab_].items) {
            if (const auto key = version_search(item); key && std::find(keys.begin(), keys.end(), *key) == keys.end()) { keys.push_back(*key); }
            if (item.shift_launch && item.shift_launch->handler == "adobeCC") {
                const auto adobe = launcher::adobe_cc_search_key(*item.shift_launch, config_directory_);
                if (std::find(keys.begin(), keys.end(), adobe) == keys.end()) { keys.push_back(adobe); }
            }
        }
        return keys;
    }

    std::shared_ptr<const std::vector<launcher::TargetVersion>> item_catalog(const launcher::Item& item) const
    {
        const auto key = version_search(item);
        if (!key) { return {}; }
        // A discovery result can be newer than this binding's prepared image
        // and action. Menus/default selection use this icon's coherent revision.
        const auto& items = configuration_.tabs[active_tab_].items;
        for (const auto& icon : icons_) {
            if (&items.at(icon.item_index) == &item) { return icon.catalog; }
        }
        const auto same_binding = [&](const launcher::Item& configured) {
            if (!configured.launch || !item.launch) { return false; }
            const auto& left = *configured.launch;
            const auto& right = *item.launch;
            return left.handler == right.handler && left.target == right.target && left.arguments == right.arguments &&
                left.working_directory == right.working_directory && left.show == right.show && left.include_beta == right.include_beta && left.root_paths == right.root_paths &&
                configured.icon_source == item.icon_source && configured.label == item.label &&
                configured.icon_explicit == item.icon_explicit && configured.label_explicit == item.label_explicit;
        };
        for (const auto& icon : icons_) {
            if (same_binding(items.at(icon.item_index))) { return icon.catalog; }
        }
        const auto found = version_presentations_.find(*key);
        return found == version_presentations_.end() ? nullptr : found->second.snapshot;
    }

    launcher::ItemVersionCatalog item_version_catalog(const launcher::Item& item) const
    {
        const auto catalog = item_catalog(item);
        if (!item.launch || !catalog) { return {}; }
        for (const auto& icon : icons_) {
            const auto& configured = configuration_.tabs[active_tab_].items.at(icon.item_index);
            if (icon.versions.snapshot() == catalog && configured.launch &&
                    configured.launch->handler == item.launch->handler && configured.launch->target == item.launch->target &&
                    configured.launch->include_beta == item.launch->include_beta && configured.launch->root_paths == item.launch->root_paths) { return icon.versions; }
        }
        return launcher::ItemVersionCatalog(*item.launch, catalog);
    }

    std::vector<launcher::TargetVersion> item_versions(const launcher::Item& item) const
    {
        const auto catalog = item_version_catalog(item);
        const auto versions = catalog.versions();
        return {versions.begin(), versions.end()};
    }

    std::optional<launcher::TargetVersion> latest_item_version(const launcher::Item& item) const
    {
        const auto catalog = item_version_catalog(item);
        const auto latest = catalog.latest();
        return latest ? std::optional(*latest) : std::nullopt;
    }

    std::optional<std::filesystem::path> default_item_version(const launcher::Item& item) const
    {
        if (!item.launch || active_versions_.empty()) { return std::nullopt; }
        // Presentation, tooltip and ordinary-click callers use the configured
        // item itself. Menus and pending intents carry their source explicitly.
        for (std::size_t tab = 0; tab < configuration_.tabs.size(); ++tab) {
            const auto& items = configuration_.tabs[tab].items;
            for (std::size_t index = 0; index < items.size(); ++index) {
                if (&items[index] == &item) {
                    return launcher::active_version(active_versions_, launcher::version_item_identity(configuration_, tab, index));
                }
            }
        }
        return std::nullopt;
    }

    std::optional<launcher::TargetVersion> selected_item_version(const launcher::Item& item) const
    {
        const auto catalog = item_version_catalog(item);
        const auto selected = catalog.select(default_item_version(item));
        return selected.version ? std::optional(*selected.version) : std::nullopt;
    }

    launcher::PreparedLaunch prepared_icon_launch(std::size_t index) const
    {
        const auto& icon = icons_.at(index);
        const auto& item = configuration_.tabs[active_tab_].items.at(icon.item_index);
        // A saved choice can change before its replacement image is ready.
        // Never dispatch a cached action prepared for the previous choice.
        return icon.pinned_version == default_item_version(item) ? icon.launch : launcher::PreparedLaunch{};
    }

    bool change_default_version(ItemBinding source, const std::optional<std::filesystem::path>& executable)
    {
        try {
            const auto identity = launcher::version_item_identity(configuration_, source.first, source.second);
            if (executable) { launcher::set_active_version(runtime_path_, identity, *executable, files_); }
            else { launcher::reset_active_version(runtime_path_, identity, files_); }
            active_versions_ = launcher::load_active_versions(runtime_path_, files_);
            load_icons();
            const auto& item = configuration_.tabs[source.first].items[source.second];
            log_.write((executable ? "Default version saved: " : "Automatic latest version restored: "),
                "tab=", configuration_.tabs[source.first].name, "; item=", item.label,
                "; target=", item.launch->target, ".");
            scenario_checkpoint("default-version-changed");
            return true;
        } catch (const std::exception& error) {
            log_.write("Could not save default version: ", error.what());
            MessageBoxW(window_, (L"Could not save the default version.\n\n" + utf16(error.what())).c_str(), L"Launcher", MB_OK | MB_ICONERROR);
            return false;
        }
    }

    std::wstring version_description(const launcher::Item& item) const
    {
        const auto pinned = default_item_version(item);
        const auto selected = selected_item_version(item);
        auto version = selected ? (pinned ? L"Active version " : L"Version ") + selected->product_version +
            (selected->beta ? L" (Beta)" : L"") : pinned && item_catalog(item) ? L"Active version unavailable" : std::wstring{};
        const auto key = version_search(item);
        if (!key) { return {}; }
        const auto found = version_presentations_.find(*key);
        if (found == version_presentations_.end()) { return L"Not checked yet"; }
        const auto& state = found->second;
        if (!selected && state.snapshot && item_catalog(item) != state.snapshot &&
                launcher::ItemVersionCatalog(*item.launch, state.snapshot).select(pinned).version) {
            version = L"Preparing active version...";
        }
        if (state.activity != launcher::VersionDiscoveryActivity::idle) {
            return version.empty() ? L"Checking installed versions..." : version + L"\nChecking installed versions...";
        }
        if (state.last_attempt && (state.last_attempt->failure != launcher::VersionDiscoveryFailure::none ||
                state.last_attempt->observation.status != launcher::TargetScanStatus::complete)) {
            return version.empty() ? L"Could not check installed versions" : version + L"\nCould not refresh; using previous results";
        }
        if (!version.empty()) { return version; }
        if (state.snapshot && item_catalog(item) != state.snapshot) {
            if (launcher::ItemVersionCatalog(*item.launch, state.snapshot).select(pinned).version) { return L"Preparing installed version..."; }
        }
        return state.snapshot ? L"Not installed" : L"Not checked yet";
    }

    std::wstring version_menu_label(const launcher::TargetVersion& installed, bool latest, bool latest_known = false, bool active = false) const
    {
        auto label = installed.product_version + (installed.beta ? L" (Beta)" : L"");
        if (!installed.target_pattern.empty() && installed.has_version_resource && !installed.path_label.empty() &&
                installed.path_label != installed.product_version) {
            label += L" — " + installed.path_label;
        }
        return label + (active ? L" — Active" : L"") + (latest ? latest_known ? L" — Latest known" : L" — Latest" : L"");
    }

    std::optional<launcher::LaunchAction> resolved_item_action(const launcher::Item& item,
        const launcher::TargetVersion* installation = nullptr) const
    {
        if (!item.launch) { return std::nullopt; }
        auto action = *item.launch;
        if (versioned_item(item)) {
            const auto latest = installation ? std::optional<launcher::TargetVersion>(*installation)
                : selected_item_version(item);
            if (!latest) { return std::nullopt; }
            action.handler = "shell";
            action.target = utf8(latest->executable.wstring());
            action.include_beta = false;
            action.root_paths.clear();
        }
        return action;
    }

    std::wstring resolved_item_label(const launcher::Item& item,
        const launcher::PreparedLaunch* prepared = nullptr) const
    {
        // Smoke-only independent expectations. Production presentation uses
        // prepare_item_presentation on a worker and never calls this helper.
        if (item.label_explicit || !item.label.empty() || !item.launch) { return utf16(item.label); }
        if (prepared == nullptr) {
            const auto action = resolved_item_action(item);
            const auto resolved = action ? launcher::prepare_launch_action(*action, config_directory_)
                : launcher::PreparedLaunch{};
            return resolved_item_label(item, &resolved);
        }
        if (prepared->action) { return launcher::launch_display_label(*prepared->action); }
        launcher::NativeLaunchAction fallback;
        fallback.target = utf16(item.launch->target);
        return launcher::launch_display_label(fallback);
    }

    void refresh_version_icons()
    {
        clear_icon_tooltips(false);
        load_icons();
        sync_icon_tooltips();
        InvalidateRect(window_, nullptr, FALSE);
    }

    bool active_page_has_versions() const
    {
        const auto& items = configuration_.tabs[active_tab_].items;
        return std::any_of(items.begin(), items.end(), [this](const auto& item) {
            return versioned_item(item);
        });
    }

    bool version_adoption_held() const noexcept
    {
        return window_ == nullptr || menu_active_ || interaction_hold_ || drag_ || pressed_icon_ ||
            version_gesture_dispatch_ || version_adopting_ || version_view_rebuilding_ || arranging_ || taskbar_transition_;
    }

    void cancel_version_intents() noexcept
    {
        for (const auto& intent : pending_version_launches_) {
            if (!intent.terminal) { versions_->release_pending(intent.search); }
        }
        pending_version_launches_.clear();
    }

    std::optional<launcher::LaunchResult> version_check_failure(const std::optional<launcher::VersionDiscoveryState>& state) const
    {
        if (state && state->last_attempt && state->last_attempt->failure == launcher::VersionDiscoveryFailure::none &&
                state->last_attempt->observation.status == launcher::TargetScanStatus::complete) { return std::nullopt; }
        std::uint32_t error = ERROR_GEN_FAILURE;
        if (state && state->last_attempt && !state->last_attempt->observation.diagnostics.empty()) {
            error = static_cast<std::uint32_t>(state->last_attempt->observation.diagnostics.front().error.value());
        }
        return launcher::LaunchResult{false, error,
            L"Installed versions could not be checked. Hide and reveal this page to try again."};
    }

    std::optional<launcher::LaunchResult> version_selection_failure(const std::optional<launcher::VersionDiscoveryState>& state,
        const std::optional<std::filesystem::path>& pinned) const
    {
        if (const auto failure = version_check_failure(state)) { return failure; }
        if (pinned) {
            return launcher::LaunchResult{false, ERROR_FILE_NOT_FOUND, L"Active version unavailable:\n" + pinned->wstring() +
                L"\n\nChoose another installed version or select Always use latest."};
        }
        return std::nullopt;
    }

    void capture_version_results(bool admit_queued = true)
    {
        versions_->collect_ready(admit_queued);
        if (filesystem_diagnostics_->write_failed.exchange(false, std::memory_order_relaxed)) {
            log_.write("Filesystem error log could not be written: ", utf8(filesystem_diagnostics_->journal.path().native()), ".");
        }
        std::vector<launcher::PerSearchNoticeState*> notices;
        for (const auto& key : version_keys_) {
            const auto state = versions_->state(key);
            if (!state || !state->last_attempt || logged_version_attempts_[key] == state->last_attempt) { continue; }
            logged_version_attempts_[key] = state->last_attempt;
            const auto& attempt = *state->last_attempt;
            if (filesystem_notification_) {
                auto& notice = filesystem_notice_states_[key];
                if (notice.observe(attempt.observation)) { notices.push_back(&notice); }
            }
            const auto status = attempt.observation.status;
            const char* outcome = status == launcher::TargetScanStatus::complete ? "complete" :
                status == launcher::TargetScanStatus::incomplete ? "incomplete" : "failed";
            log_.write("Version discovery attempt: id=", std::to_string(attempt.id), "; search=",
                (key.kind == launcher::VersionSearchKind::adobe_catalog ? "<Adobe catalog>" : utf8(key.locations.front())),
                "; status=", outcome, "; retained=",
                (state->snapshot && status != launcher::TargetScanStatus::complete ? "yes" : "no"),
                "; observed=", std::to_string(attempt.observation.versions.size()), "; error=",
                std::to_string(attempt.observation.diagnostics.empty() ? 0 : attempt.observation.diagnostics.front().error.value()), ".");
            if (attempt.observation.omitted_diagnostics != 0) {
                log_.write("Version discovery diagnostics omitted: id=", std::to_string(attempt.id),
                    "; count=", std::to_string(attempt.observation.omitted_diagnostics), ".");
            }
            if (attempt.failure != launcher::VersionDiscoveryFailure::none) {
                log_.write("Version discovery failure: id=", std::to_string(attempt.id), "; stage=",
                    (attempt.failure == launcher::VersionDiscoveryFailure::start ? "start" : "scanner"), "; reason=", attempt.error, ".");
            }
        }
        if (!notices.empty()) {
            const auto result = filesystem_notification_->show(filesystem_diagnostics_->journal.path());
            if (SUCCEEDED(result)) {
                for (const auto notice : notices) { notice->mark_shown(); }
                log_.write("Filesystem notification requested; details=", utf8(filesystem_diagnostics_->journal.path().native()), ".");
                scenario_checkpoint("filesystem-notification-requested");
            } else { log_.result("Windows filesystem notification could not be requested", result); }
        }
        for (auto& intent : pending_version_launches_) {
            if (intent.terminal) { continue; }
            const auto state = versions_->state(intent.search);
            if (state && state->activity != launcher::VersionDiscoveryActivity::idle) { continue; }
            // A terminal attempt is consumed now, even if a menu/press defers
            // its action. A later visit can never revive this original intent.
            if (state && state->last_attempt && state->last_attempt != intent.previous_attempt && state->snapshot &&
                state->last_attempt->failure == launcher::VersionDiscoveryFailure::none &&
                state->last_attempt->observation.status == launcher::TargetScanStatus::complete) {
                const launcher::ItemVersionCatalog catalog(*intent.item.launch, state->snapshot);
                if (const auto selected = catalog.select(intent.pinned_version).version) { intent.resolved = resolved_item_action(intent.item, selected); }
            }
            if (!intent.resolved) { intent.failure = version_selection_failure(state, intent.pinned_version); }
            intent.terminal = true;
            versions_->release_pending(intent.search);
        }
    }

    void report_unmatched_adobe_targets(const launcher::VersionSearchKey& key, const launcher::VersionDiscoveryState& state)
    {
        if (smoke_mode_ || test_ui_mode_ || !state.snapshot || version_check_failure(state)) { return; }
#if LAUNCHER_VERSION_TEST_HOOKS
        if (scenario_) { return; }
#endif
        std::vector<std::wstring> unmatched;
        for (const auto& tab : configuration_.tabs) {
            for (const auto& item : tab.items) {
                for (const auto* action : {&item.launch, &item.shift_launch}) {
                    if (!*action || (*action)->handler != "adobeCC") { continue; }
                    if (launcher::adobe_cc_search_key(**action, config_directory_) != key) { continue; }
                    const auto target = utf16((*action)->target);
                    if (std::find(reported_adobe_targets_.begin(), reported_adobe_targets_.end(), std::pair{key, target}) == reported_adobe_targets_.end() &&
                            std::find(unmatched.begin(), unmatched.end(), target) == unmatched.end() &&
                            launcher::adobe_cc_versions(*state.snapshot, target).empty()) { unmatched.push_back(target); }
                }
            }
        }
        if (unmatched.empty()) { return; }
        const auto report = launcher::adobe_cc_tab_report(*state.snapshot, unmatched, key.locations);
        auto completion = std::make_shared<LaunchCompletion>();
        completion->label = L"Available AdobeCC applications";
        launch_completions_.push_back(completion);
        try {
            background_.start([completion, report] {
                try {
                    const auto path = write_adobe_cc_report(report);
                    completion->result = launcher::launch_prepared_action(filesystem_log_action(path));
                } catch (const std::exception& error) {
                    completion->result = {false, ERROR_GEN_FAILURE, utf16(error.what())};
                }
                completion->ready.store(true, std::memory_order_release);
            });
            for (const auto& target : unmatched) { reported_adobe_targets_.emplace_back(key, target); }
            log_.write("Unmatched AdobeCC targets; opening the complete installed-applications tab in Notepad.");
        } catch (...) {
            completion->result = {false, ERROR_NOT_ENOUGH_MEMORY, L"Cannot start the AdobeCC report worker."};
            completion->ready.store(true, std::memory_order_release);
        }
    }

    void adopt_version_results()
    {
        if (version_adoption_held()) { return; }
        struct AdoptionHold final { bool& value; ~AdoptionHold() { value = false; } } hold{version_adopting_};
        version_adopting_ = true;
        bool catalog_changed = false;
        bool presentation_changed = false;
        const auto visible = page_version_searches();
        for (const auto& key : version_keys_) {
            const auto state = versions_->state(key);
            if (!state) { continue; }
            const auto existing = version_presentations_.find(key);
            if (existing != version_presentations_.end()) {
                const auto& shown = existing->second;
                if (shown.activity == state->activity && shown.snapshot == state->snapshot &&
                        shown.last_attempt == state->last_attempt && shown.catalog_revision == state->catalog_revision &&
                        shown.checked_this_activation == state->checked_this_activation) { continue; }
            }
            if (key.kind == launcher::VersionSearchKind::adobe_catalog) { report_unmatched_adobe_targets(key, *state); }
            presentation_changed = true;
            const auto prior_snapshot = existing == version_presentations_.end() ? nullptr : existing->second.snapshot;
            if (prior_snapshot != state->snapshot && std::find(visible.begin(), visible.end(), key) != visible.end()) { catalog_changed = true; }
            version_presentations_[key] = *state;
        }
        if (catalog_changed && !hidden_to_taskbar_ && !display_layout_failed_) {
            refresh_version_icons();
        } else if (presentation_changed) {
            // Freshness/error text can change without invalidating the icon or
            // its drawing resources, including an unchanged successful scan.
            clear_icon_tooltips(false);
            for (auto& icon : icons_) {
                const auto& item = configuration_.tabs[active_tab_].items[icon.item_index];
                if (!versioned_item(item)) { continue; }
                icon.tooltip = icon.label.empty() ? L"" : icon.label + L"\n" + version_description(item);
            }
            sync_icon_tooltips();
            // Pending artwork must disappear on an unchanged or failed scan,
            // even if no image completion or pointer movement follows it.
            if (hovered_icon_) { InvalidateRect(window_, nullptr, FALSE); }
        }
        while (window_ != nullptr) {
            const auto intent = std::find_if(pending_version_launches_.begin(), pending_version_launches_.end(),
                [](const auto& candidate) { return candidate.terminal; });
            if (intent == pending_version_launches_.end()) { break; }
            auto ready = std::move(*intent);
            pending_version_launches_.erase(intent);
            start_item_launch(ready.item, ready.label, ready.verb, ready.reveal, nullptr, nullptr,
                ready.resolved ? &*ready.resolved : nullptr, true, ready.failure ? &*ready.failure : nullptr);
        }
    }

    void finish_version_discovery()
    {
        capture_version_results();
        adopt_version_results();
        collect_item_presentations();
        adopt_item_presentations();
    }

    void sync_version_activation()
    {
        if (!versions_ || window_ == nullptr || version_view_rebuilding_) { return; }
        const bool shown = peek_.state() == launcher::DockState::expanded && !hidden_to_taskbar_ &&
            !taskbar_transition_ && !restart_pending_ && !display_layout_failed_;
        if (shown && shown_version_tab_ == active_tab_) { return; }
        if (!shown && !shown_version_tab_) { return; }
        // Freeze old terminal intents before a new visit can replace their
        // attempt. Do not admit the tab's now-obsolete queued searches here.
        capture_version_results(false);
        if (shown) {
            shown_version_tab_ = active_tab_;
            const auto keys = page_version_searches();
            // Leave a result published since the pre-transition drain attached
            // to its running job. This visit joins it before the next capture,
            // so an original cold intent cannot follow a replacement attempt.
            const auto token = versions_->begin_activation(keys, false);
            presentation_visit_ = token;
            load_icons(); // One preparation check per shown-page visit, never per hover or repaint.
            log_.write("Version tab activation: token=", std::to_string(token), "; tab=", std::to_string(active_tab_),
                "; searches=", std::to_string(keys.size()), ".");
        } else {
            shown_version_tab_.reset();
            versions_->deactivate();
        }
        finish_version_discovery();
    }

    void request_item_versions(const launcher::Item& item)
    {
        const auto key = version_search(item);
        if (!key || !shown_version_tab_ || *shown_version_tab_ != active_tab_) { return; }
        capture_version_results(false);
        versions_->ensure_checked(*key, versions_->activation(), launcher::VersionRequestReason::attention);
        finish_version_discovery();
    }

    bool settle_version_discovery()
    {
        const auto deadline = GetTickCount64() + 5000;
        for (;;) {
            finish_version_discovery();
            const bool presentation_pending = active_presentations_ != 0 || std::any_of(icons_.begin(), icons_.end(), [this](const auto& icon) {
                const auto record = item_presentations_.find({active_tab_, icon.item_index});
                return record != item_presentations_.end() && record->second.desired &&
                    !record->second.desired->ready.load(std::memory_order_acquire);
            });
            if (versions_->active_count() == 0 && versions_->queued_count() == 0 && !presentation_pending) { return true; }
            const auto now = GetTickCount64();
            if (now >= deadline) { log_.write("Smoke version discovery timed out."); return false; }
            const DWORD result = WaitForSingleObject(background_.completion_event(), static_cast<DWORD>(deadline - now));
            if (result != WAIT_OBJECT_0) { log_.write("Smoke version discovery completion wait failed."); return false; }
            collect_background();
        }
    }

    std::vector<launcher::TargetVersion> append_installed_versions(HMENU menu,
        const launcher::Item& item, const std::optional<launcher::LaunchAction>& action, VersionMenuSession* session = nullptr)
    {
        auto versions = item_versions(item);
        const auto latest = latest_item_version(item);
        const auto latest_path = latest ? std::optional(latest->executable) : std::nullopt;
        const bool saved_default = default_item_version(item).has_value();
        const bool limited = versions.size() > static_cast<std::size_t>(kIconShellFirst - kIconVersionFirst);
        if (limited) { versions.resize(kIconShellFirst - kIconVersionFirst); }
        if (versions.empty()) { return versions; }
        const int first = GetMenuItemCount(menu);
        if (first < 0) { return {}; }
        const auto rollback = [menu, first] {
            while (GetMenuItemCount(menu) > first && DeleteMenu(menu, static_cast<UINT>(first), MF_BYPOSITION)) {}
        };
        const auto unavailable = [&] {
            rollback();
            if (session) { session->pages.clear(); session->versions.clear(); session->limited = true; }
            log_.write("Installed version menu unavailable: native menu allocation failed.");
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Installed version menu unavailable");
        };
        std::wstring heading_text{kInstalledVersionsHeading};
        MENUITEMINFOW heading{};
        heading.cbSize = sizeof(heading);
        heading.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_DATA | MIIM_STRING;
        heading.fType = MFT_STRING;
        heading.fState = MFS_DISABLED;
        heading.dwItemData = reinterpret_cast<ULONG_PTR>(kInstalledVersionsHeading);
        heading.dwTypeData = heading_text.data();
        if (!AppendMenuW(menu, MF_SEPARATOR, 0, nullptr) ||
                !InsertMenuItemW(menu, static_cast<UINT>(first) + 1, TRUE, &heading)) {
            unavailable();
            return {};
        }
        if (session) {
            session->versions = versions;
            session->normal_action = action;
            session->latest_version = latest_path;
            session->saved_default = saved_default;
            session->root = menu;
            session->limited = limited;
            if (const auto key = version_search(item)) {
                const auto state = version_presentations_.find(*key);
                session->latest_known = state != version_presentations_.end() &&
                    (state->second.activity != launcher::VersionDiscoveryActivity::idle ||
                     (state->second.last_attempt && (state->second.last_attempt->failure != launcher::VersionDiscoveryFailure::none ||
                      state->second.last_attempt->observation.status != launcher::TargetScanStatus::complete)));
                if (session->latest_known) {
                    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, state->second.activity != launcher::VersionDiscoveryActivity::idle
                        ? L"Checking installed versions; using previous results" : L"Could not refresh; using previous results");
                }
            }
        }
        const std::size_t inline_count = session ? std::min<std::size_t>(50, versions.size()) : versions.size();
        for (std::size_t index = 0; index < inline_count; ++index) {
            if (!append_version_row(menu, versions, action, index, latest_path, saved_default, session && session->latest_known)) {
                unavailable();
                return {};
            }
        }
        if (session && versions.size() > inline_count) {
            const auto prior_pages = session->pages.size();
            HMENU more = CreatePopupMenu();
            if (!more || !append_version_pages(more, *session, inline_count, versions.size()) ||
                    !AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(more), L"More versions")) {
                if (more) { DestroyMenu(more); }
                session->pages.resize(prior_pages);
                session->limited = true;
                AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Additional version menu unavailable");
            }
        }
        if (limited) { AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Version menu limit reached; some entries unavailable"); }
        if (!AppendMenuW(menu, MF_SEPARATOR, 0, nullptr)) {
            unavailable();
            return {};
        }
        return versions;
    }

    bool append_version_row(HMENU menu, const std::vector<launcher::TargetVersion>& versions,
        const std::optional<launcher::LaunchAction>& action, std::size_t index,
        const std::optional<std::filesystem::path>& latest, bool saved_default, bool latest_known = false)
    {
        if (index >= versions.size() || index >= kIconShellFirst - kIconVersionFirst) { return false; }
        const auto& installed = versions[index];
        const bool selected = action && installed.executable.wstring() == utf16(action->target);
        auto label = version_menu_label(installed, latest && installed.executable == *latest, latest_known, selected && saved_default);
        for (std::size_t ampersand = 0; (ampersand = label.find(L'&', ampersand)) != std::wstring::npos; ampersand += 2) {
            label.insert(ampersand, 1, L'&');
        }
        MENUITEMINFOW row{};
        row.cbSize = sizeof(row);
        row.fMask = MIIM_STRING | MIIM_ID | MIIM_STATE;
        row.fState = selected ? MFS_CHECKED : MFS_ENABLED;
        row.wID = kIconVersionFirst + static_cast<UINT>(index);
        row.dwTypeData = label.data();
        return InsertMenuItemW(menu, static_cast<UINT>(-1), TRUE, &row) != FALSE;
    }

    bool append_version_pages(HMENU parent, VersionMenuSession& session, std::size_t first, std::size_t end)
    {
        constexpr std::size_t rows_per_page = 50;
        constexpr std::size_t pages_per_group = 50;
        const auto pages = (end - first + rows_per_page - 1) / rows_per_page;
        const auto step = pages > pages_per_group ? rows_per_page * pages_per_group : rows_per_page;
        for (auto begin = first; begin < end; begin += step) {
            const auto limit = std::min(end, begin + step);
            HMENU submenu = CreatePopupMenu();
            if (!submenu) { return false; }
            const auto label = L"Versions " + std::to_wstring(begin + 1) + L"–" + std::to_wstring(limit);
            const bool built = step == rows_per_page
                ? AppendMenuW(submenu, MF_STRING | MF_GRAYED, 0, L"Preparing version page...") != FALSE
                : append_version_pages(submenu, session, begin, limit);
            if (!built || !AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), label.c_str())) {
                DestroyMenu(submenu);
                return false;
            }
            if (step == rows_per_page) { session.pages.push_back({submenu, begin, limit - begin}); }
        }
        return true;
    }

    void initialize_version_page(HMENU menu)
    {
        if (!active_version_menu_) { return; }
        auto& session = *active_version_menu_;
        const auto found = std::find_if(session.pages.begin(), session.pages.end(),
            [menu](const auto& page) { return page.menu == menu; });
        if (found == session.pages.end() || found->initialized) { return; }
        found->initialized = true;
        DeleteMenu(menu, 0, MF_BYPOSITION);
        for (std::size_t index = found->first; index < found->first + found->count; ++index) {
            if (!append_version_row(menu, session.versions, session.normal_action, index, session.latest_version, session.saved_default, session.latest_known)) {
                session.limited = true;
                AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Additional version rows unavailable");
                break;
            }
        }
    }

    std::optional<std::size_t> version_menu_row(HMENU menu, UINT position) const
    {
        if (!active_version_menu_) { return std::nullopt; }
        const auto& session = *active_version_menu_;
        if (menu != session.root && std::none_of(session.pages.begin(), session.pages.end(),
                [menu](const auto& page) { return page.menu == menu; })) { return std::nullopt; }
        MENUITEMINFOW row{};
        row.cbSize = sizeof(row);
        row.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, position, TRUE, &row) || (row.fType & MFT_SEPARATOR) != 0 || row.hSubMenu ||
                (row.fState & MFS_DISABLED) != 0 || row.wID < kIconVersionFirst ||
                row.wID - kIconVersionFirst >= session.versions.size()) {
            return std::nullopt;
        }
        return row.wID - kIconVersionFirst;
    }

    HRESULT verify_installed_versions()
    {
        launcher::Item missing;
        missing.launch = launcher::LaunchAction{"adobeCC", "Adobe nonexistent fixture product"};
        if (resolved_item_action(missing)) { return E_FAIL; }
        for (const auto& icon : icons_) {
            const auto& item = configuration_.tabs[active_tab_].items.at(icon.item_index);
            if (!versioned_item(item)) { continue; }
            const auto latest = latest_item_version(item);
            const auto action = resolved_item_action(item);
            const auto expected_tip = icon.label.empty() ? L"" : icon.label + L"\n" + version_description(item);
            if (action.has_value() != latest.has_value() || icon.tooltip != expected_tip) { return E_FAIL; }
            if (action) {
                const auto prepared = launcher::prepare_launch_action(*action, config_directory_);
                if (action->handler != "shell" || !prepared.action ||
                        prepared.action->target != latest->executable.wstring()) {
                    return E_FAIL;
                }
            }
            const HMENU menu = CreatePopupMenu();
            if (menu == nullptr) { return E_FAIL; }
            struct MenuCleanup final {
                HMENU value;
                ~MenuCleanup() { DestroyMenu(value); }
            } cleanup{menu};
            const auto versions = append_installed_versions(menu, item, action);
            const auto expected = item_versions(item);
            if (icon.versions.multiple() != (expected.size() > 1)) { return E_FAIL; }
            if (versions.size() != std::min(expected.size(),
                    static_cast<std::size_t>(kIconShellFirst - kIconVersionFirst)) ||
                    GetMenuItemCount(menu) != (versions.empty() ? 0 : static_cast<int>(versions.size()) + 3)) {
                return E_FAIL;
            }
            if (!versions.empty() &&
                    (((GetMenuState(menu, 0, MF_BYPOSITION) & MF_SEPARATOR) == 0) ||
                     ((GetMenuState(menu, static_cast<UINT>(versions.size()) + 2, MF_BYPOSITION) & MF_SEPARATOR) == 0))) {
                return E_FAIL;
            }
            if (!versions.empty()) {
                MENUITEMINFOW heading{};
                heading.cbSize = sizeof(heading);
                heading.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_DATA | MIIM_SUBMENU;
                if (!GetMenuItemInfoW(menu, 1, TRUE, &heading) || heading.fType != MFT_STRING ||
                        (heading.fState & MFS_DISABLED) == 0 || (heading.fState & MFS_DEFAULT) != 0 ||
                        heading.wID != 0 || heading.hSubMenu != nullptr ||
                        heading.dwItemData != reinterpret_cast<ULONG_PTR>(kInstalledVersionsHeading) ||
                        GetMenuDefaultItem(menu, FALSE, 0) != static_cast<UINT>(-1)) { return E_FAIL; }
            }
            for (std::size_t index = 0; index < versions.size(); ++index) {
                auto configured = item;
                configured.launch->arguments = "\"test project with spaces\"";
                configured.launch->working_directory = "Tests";
                configured.launch->show = launcher::LaunchShow::minimized;
                const auto pinned = resolved_item_action(configured, &versions[index]);
                const bool selected = latest && versions[index].executable == latest->executable;
                const auto command = kIconVersionFirst + static_cast<UINT>(index);
                const auto state = GetMenuState(menu, command, MF_BYCOMMAND);
                const int label_size = GetMenuStringW(menu, command, nullptr, 0, MF_BYCOMMAND);
                std::wstring label(static_cast<std::size_t>(label_size) + 1, L'\0');
                label.resize(static_cast<std::size_t>(GetMenuStringW(menu, command, label.data(), label_size + 1, MF_BYCOMMAND)));
                for (std::size_t ampersand = 0; (ampersand = label.find(L"&&", ampersand)) != std::wstring::npos;
                        ++ampersand) {
                    label.erase(ampersand, 1);
                }
                const auto expected_label = version_menu_label(versions[index], selected);
                if (!pinned || utf16(pinned->target) != versions[index].executable.wstring() ||
                        pinned->arguments != configured.launch->arguments ||
                        pinned->working_directory != configured.launch->working_directory ||
                        pinned->show != configured.launch->show ||
                        GetMenuItemID(menu, static_cast<int>(index) + 2) != command ||
                        GetSubMenu(menu, static_cast<int>(index) + 2) != nullptr || label != expected_label ||
                        ((state & MF_CHECKED) != 0) != selected) { return E_FAIL; }
                log_.write("Smoke installed versions version: product=", item.launch->target, "; version=",
                    utf8(versions[index].product_version), "; beta=", (versions[index].beta ? "yes" : "no"),
                    "; default=", (selected ? "yes" : "no"), "; executable=",
                    utf8(versions[index].executable.wstring()), ".");
            }
            log_.write("Smoke installed versions passed: product=", item.launch->target,
                "; inline native version menu, tooltip, latest icon target and pinned launch options verified.");
        }
        return S_OK;
    }

    HRESULT verify_version_activation()
    {
        const auto saved_controller = peek_;
        const auto saved_tab = active_tab_;
        const auto attempts = [this] {
            std::map<launcher::VersionSearchKey, std::shared_ptr<const launcher::VersionDiscoveryAttempt>> result;
            for (const auto& key : version_keys_) {
                const auto state = versions_->state(key);
                if (state) { result.emplace(key, state->last_attempt); }
            }
            return result;
        };
        const auto fail = [&](const char* detail) {
            log_.write("Smoke version activation failed: ", detail, ".");
            select_tab(saved_tab);
            peek_ = saved_controller;
            apply_state(true);
            return E_FAIL;
        };
        if (!settle_version_discovery()) { return fail("initial shown searches did not settle"); }
        const auto original_attempts = attempts();
        const auto original_token = versions_->activation();
        if (!shown_version_tab_ || *shown_version_tab_ != active_tab_ || original_token == 0) {
            return fail("shown page has no activation");
        }
        for (unsigned int repeat = 0; repeat < 3; ++repeat) {
            for (const auto& item : configuration_.tabs[active_tab_].items) { request_item_versions(item); }
            sync_version_activation();
            finish_version_discovery();
        }
        if (attempts() != original_attempts || versions_->activation() != original_token ||
                versions_->active_count() != 0 || versions_->queued_count() != 0 || !select_tab(saved_tab) ||
                versions_->activation() != original_token) { return fail("attention or a no-op tab click started another attempt"); }

        peek_ = launcher::PeekController(true);
        apply_state(true);
        const auto hidden_attempts = attempts();
        const auto hidden_tab = configuration_.tabs.size() > 1 ? (saved_tab + 1) % configuration_.tabs.size() : saved_tab;
        if (shown_version_tab_ || versions_->activation() != 0 || !select_tab(hidden_tab)) {
            return fail("collapse did not deactivate the shown page");
        }
        for (const auto& item : configuration_.tabs[active_tab_].items) { request_item_versions(item); }
        for (unsigned int repeat = 0; repeat < 3; ++repeat) { sync_version_activation(); finish_version_discovery(); }
        if (attempts() != hidden_attempts || shown_version_tab_ || versions_->activation() != 0 ||
                versions_->active_count() != 0 || versions_->queued_count() != 0) {
            return fail("a hidden page or idle drain started discovery");
        }

        peek_.set_menu_open(true);
        apply_state(true);
        const auto revealed_token = versions_->activation();
        if (!shown_version_tab_ || *shown_version_tab_ != hidden_tab || revealed_token == 0 ||
                revealed_token == original_token || !settle_version_discovery()) {
            return fail("revealing the selected page did not start its visit");
        }
        const auto visible = page_version_searches();
        for (const auto& [key, prior] : hidden_attempts) {
            const auto state = versions_->state(key);
            const bool shown = std::find(visible.begin(), visible.end(), key) != visible.end();
            if (!state || (shown ? !state->checked_this_activation || !state->last_attempt || state->last_attempt == prior
                                 : state->last_attempt != prior)) {
                return fail("a reveal refreshed the wrong search set");
            }
        }
        const auto revealed_attempts = attempts();
        for (const auto& item : configuration_.tabs[active_tab_].items) { request_item_versions(item); }
        if (!select_tab(active_tab_) || attempts() != revealed_attempts || versions_->activation() != revealed_token ||
                versions_->active_count() != 0 || versions_->queued_count() != 0) {
            return fail("repeated attention did not reuse the revealed visit");
        }
        if (!select_tab(saved_tab) || !settle_version_discovery()) { return fail("restoring the original shown page failed"); }
        peek_ = saved_controller;
        apply_state(true);
        log_.write("Smoke version activation passed: shown-page visits, hidden-tab selection, per-search activation checks, "
            "repeated attention/no-op clicks and idle drains verified; no periodic scan or configured target execution.");
        return S_OK;
    }

    HRESULT verify_version_discovery()
    {
        const HRESULT activation = verify_version_activation();
        if (FAILED(activation)) { return activation; }
        const auto icon = std::find_if(icons_.begin(), icons_.end(), [this](const auto& candidate) {
            const auto& item = configuration_.tabs[active_tab_].items[candidate.item_index];
            return versioned_item(item) && latest_item_version(item).has_value();
        });
        if (icon == icons_.end()) {
            log_.write("Smoke installed versions discovery service passed: activation verified; no installed target for adoption checks.");
            return S_OK;
        }
        const auto icon_index = static_cast<std::size_t>(icon - icons_.begin());
        const auto item_index = icon->item_index;
        auto clicked = configuration_.tabs[active_tab_].items[item_index];
        const auto key = *version_search(clicked);
        const auto before = versions_->state(key);
        if (!before || !before->snapshot || before->snapshot->empty() || !pending_version_launches_.empty()) { return E_FAIL; }
        const auto original = before->snapshot;
        auto old_catalog = std::make_shared<std::vector<launcher::TargetVersion>>(*original);
        for (auto& installed : *old_catalog) { installed.product_version += L" (old presentation)"; }
        version_presentations_[key].snapshot = old_catalog;
        refresh_version_icons();

        const HMENU menu = CreatePopupMenu();
        if (!menu) { return E_FAIL; }
        struct MenuCleanup final { HMENU value; ~MenuCleanup() { DestroyMenu(value); } } menu_cleanup{menu};
        const auto menu_versions = append_installed_versions(menu, clicked, resolved_item_action(clicked));
        if (menu_versions.empty()) { return E_FAIL; }
        const auto menu_text = [menu] {
            const int count = GetMenuStringW(menu, kIconVersionFirst, nullptr, 0, MF_BYCOMMAND);
            std::wstring result(static_cast<std::size_t>(count) + 1, L'\0');
            result.resize(static_cast<std::size_t>(GetMenuStringW(menu, kIconVersionFirst, result.data(), count + 1, MF_BYCOMMAND)));
            return result;
        };
        const auto frozen_menu_text = menu_text();
        clicked.launch->arguments = "\"queued project with spaces\"";
        clicked.launch->working_directory = "Queued project directory";
        clicked.launch->show = launcher::LaunchShow::minimized;
        smoke_cached_launch_.reset();
        smoke_cached_launch_label_.clear();
        const bool saved_menu_active = menu_active_;
        const auto saved_press = pressed_icon_;
        struct InteractionCleanup final {
            bool& menu;
            std::optional<std::size_t>& press;
            bool prior_menu;
            std::optional<std::size_t> prior_press;
            ~InteractionCleanup() { menu = prior_menu; press = prior_press; }
        } interaction_cleanup{menu_active_, pressed_icon_, saved_menu_active, saved_press};
        menu_active_ = true;
        const auto keys = page_version_searches();
        versions_->begin_activation(keys);
        if (!versions_->retain_pending(key)) { return E_FAIL; }
        pending_version_launches_.push_back({clicked, L"Original clicked versioned item", {}, false,
            key, before->last_attempt, {}, false, {active_tab_, item_index}});
        if (!settle_version_discovery()) { return E_FAIL; }
        const auto completed = versions_->state(key);
        if (!completed || completed->activity != launcher::VersionDiscoveryActivity::idle ||
                completed->last_attempt == before->last_attempt || completed->snapshot != original ||
                version_presentations_[key].snapshot != old_catalog || menu_text() != frozen_menu_text ||
                pending_version_launches_.size() != 1 || !pending_version_launches_.front().terminal || smoke_cached_launch_) {
            return E_FAIL;
        }
        menu_active_ = false;
        pressed_icon_ = icon_index;
        finish_version_discovery();
        if (version_presentations_[key].snapshot != old_catalog || pending_version_launches_.size() != 1 || smoke_cached_launch_) {
            return E_FAIL;
        }
        pressed_icon_.reset();
        finish_version_discovery();
        if (!settle_version_discovery()) { return E_FAIL; }
        const auto latest = latest_item_version(clicked);
        if (version_presentations_[key].snapshot != original || !pending_version_launches_.empty() || !latest ||
                !smoke_cached_launch_ || utf16(smoke_cached_launch_->target) != latest->executable.wstring() ||
                smoke_cached_launch_->arguments != clicked.launch->arguments ||
                smoke_cached_launch_->working_directory != clicked.launch->working_directory ||
                smoke_cached_launch_->show != clicked.launch->show ||
                smoke_cached_launch_label_ != L"Original clicked versioned item") { return E_FAIL; }
        HRESULT result = render();
        if (result == D2DERR_RECREATE_TARGET) { result = render(); }
        if (FAILED(result)) { return result; }
        const auto surface = drawing_target_;
        const auto bitmap = icons_.at(icon_index).bitmap;
        versions_->begin_activation(keys);
        if (!settle_version_discovery() || versions_->state(key)->snapshot != original ||
                drawing_target_.Get() != surface.Get() || icons_.at(icon_index).bitmap.Get() != bitmap.Get()) { return E_FAIL; }
        log_.write("Smoke installed versions discovery service passed: real per-search completion, frozen native menu, "
            "menu/mouse-press adoption holds, original queued item/options and unchanged drawing resources verified; no real launch.");
        return S_OK;
    }

    HRESULT verify_item_presentations()
    {
        if (!settle_version_discovery()) { return E_FAIL; }
        if (icons_.empty()) {
            log_.write("Smoke item presentation passed: empty page has no pending presentation work.");
            return S_OK;
        }
        auto& icon = icons_.front();
        const ItemBinding binding{active_tab_, icon.item_index};
        auto& record = item_presentations_.at(binding);
        if (!record.adopted || !record.adopted->result || record.running) { return E_FAIL; }
        const auto old_label = icon.label;
        const auto old_catalog = icon.catalog;
        const auto original_count = active_presentations_;
        const auto original_bytes = admitted_presentation_bytes_;
        struct Restore final {
            ItemPresentationRecord& record;
            ItemPresentationRecord saved_record;
            IconVisual& icon;
            IconVisual saved_icon;
            bool& menu;
            bool saved_menu;
            std::optional<std::size_t>& press;
            std::optional<std::size_t> saved_press;
            std::size_t& active;
            std::size_t saved_active;
            std::uint64_t& bytes;
            std::uint64_t saved_bytes;
            std::uint64_t& visit;
            std::uint64_t saved_visit;
            ~Restore() {
                record = std::move(saved_record); icon = std::move(saved_icon);
                menu = saved_menu; press = saved_press; active = saved_active; bytes = saved_bytes; visit = saved_visit;
            }
        } restore{record, record, icon, icon, menu_active_, menu_active_, pressed_icon_, pressed_icon_,
            active_presentations_, original_count, admitted_presentation_bytes_, original_bytes, presentation_visit_, presentation_visit_};
        auto late = std::make_shared<ItemPresentationJob>();
        late->generation = ++record.generation;
        late->request = record.adopted->request;
        late->catalog = old_catalog;
        late->result = *record.adopted->result;
        late->result->label = L"Late stale generation";
        late->bytes = record.adopted->bytes;
        record.running = late;
        ++active_presentations_;
        admitted_presentation_bytes_ += late->bytes;

        auto current = std::make_shared<ItemPresentationJob>();
        current->generation = ++record.generation;
        current->request = record.adopted->request;
        current->result = *record.adopted->result;
        current->result->label = L"New prepared generation";
        current->bytes = record.adopted->bytes;
        if (old_catalog) { current->catalog = std::make_shared<const std::vector<launcher::TargetVersion>>(*old_catalog); }
        const auto& current_item = configuration_.tabs[active_tab_].items.at(icon.item_index);
        if (current_item.launch) { current->versions = launcher::ItemVersionCatalog(*current_item.launch, current->catalog); }
        current->ready.store(true, std::memory_order_release);
        record.desired = current;
        menu_active_ = true;
        adopt_item_presentations();
        if (icon.label != old_label || icon.catalog != old_catalog) { return E_FAIL; }
        menu_active_ = false;
        pressed_icon_ = 0;
        adopt_item_presentations();
        if (icon.label != old_label || icon.catalog != old_catalog) { return E_FAIL; }
        pressed_icon_.reset();
        adopt_item_presentations();
        if (icon.label != current->result->label || icon.catalog != current->catalog ||
                icon.presentation_generation != current->generation) { return E_FAIL; }
        // Completion A arrives after desired generation B was already adopted.
        // Collection frees its admission, but it must not repaint B's binding.
        late->ready.store(true, std::memory_order_release);
        collect_item_presentations();
        adopt_item_presentations();
        if (record.running || icon.label != current->result->label || icon.catalog != current->catalog ||
                active_presentations_ != original_count || admitted_presentation_bytes_ != original_bytes) { return E_FAIL; }
        auto startup_failure = std::make_shared<ItemPresentationJob>();
        startup_failure->generation = ++record.generation;
        startup_failure->request = restore.saved_record.adopted->request;
        startup_failure->catalog = restore.saved_record.adopted->catalog;
        startup_failure->error = "Injected completed startup preparation failure.";
        startup_failure->ready.store(true, std::memory_order_release);
        record.desired = startup_failure;
        presentation_visit_ = 0;
        queue_item_presentation(icon, startup_failure->request.pixel_size);
        if (record.desired != startup_failure) { return E_FAIL; }
        presentation_visit_ = restore.saved_visit != 0 ? restore.saved_visit : 1;
        queue_item_presentation(icon, startup_failure->request.pixel_size);
        const auto retried = record.desired;
        if (retried == startup_failure || retried->visit == 0 || retried->generation <= startup_failure->generation) { return E_FAIL; }
        queue_item_presentation(icon, startup_failure->request.pixel_size);
        if (record.desired != retried) { return E_FAIL; }
        log_.write("Smoke item presentation passed: owned per-item result, coherent catalog/action adoption, menu/press holds, "
            "late stale generation rejection, released admission and startup-failure retry once per shown visit verified; no filesystem work or target execution.");
        return S_OK;
    }

    HRESULT verify_pending_overlay()
    {
        const auto found = std::find_if(icons_.begin(), icons_.end(), [this](const auto& icon) {
            return versioned_item(configuration_.tabs[active_tab_].items.at(icon.item_index));
        });
        if (found == icons_.end()) {
            log_.write("Smoke pending overlay passed: no versioned binding on this page.");
            return S_OK;
        }
        const auto index = static_cast<std::size_t>(found - icons_.begin());
        const auto key = *version_search(configuration_.tabs[active_tab_].items.at(found->item_index));
        auto& state = version_presentations_[key];
        struct Restore final {
            launcher::VersionDiscoveryState& state;
            launcher::VersionDiscoveryActivity activity;
            std::optional<std::size_t>& icon;
            std::optional<std::size_t> prior_icon;
            std::optional<std::size_t>& tab;
            std::optional<std::size_t> prior_tab;
            ~Restore() { state.activity = activity; icon = prior_icon; tab = prior_tab; }
        } restore{state, state.activity, hovered_icon_, hovered_icon_, hovered_tab_, hovered_tab_};
        hovered_icon_ = index;
        hovered_tab_.reset();
        state.activity = launcher::VersionDiscoveryActivity::idle;
        if (pending_overlay_visible(index) || FAILED(render()) || !pending_overlay_bitmap_) { return E_FAIL; }
        ComPtr<IWICBitmap> normal;
        if (FAILED(copy_window_image(normal))) { return E_FAIL; }
        const auto overlay = pending_overlay_bitmap_;
        const auto icon_bitmap = found->bitmap;
        state.activity = launcher::VersionDiscoveryActivity::queued;
        if (!pending_overlay_visible(index) || FAILED(render())) { return E_FAIL; }
        ComPtr<IWICBitmap> pending;
        if (FAILED(copy_window_image(pending))) { return E_FAIL; }
        UINT width = 0;
        UINT height = 0;
        if (FAILED(normal->GetSize(&width, &height))) { return E_FAIL; }
        const UINT stride = width * 4;
        std::vector<BYTE> before(static_cast<std::size_t>(stride) * height);
        std::vector<BYTE> after(before.size());
        if (FAILED(normal->CopyPixels(nullptr, stride, static_cast<UINT>(before.size()), before.data())) ||
                FAILED(pending->CopyPixels(nullptr, stride, static_cast<UINT>(after.size()), after.data()))) { return E_FAIL; }
        bool changed = false;
        const auto bounds = icon_client_px_rect(*found);
        for (UINT row = 0; row < height; ++row) {
            for (UINT column = 0; column < width; ++column) {
                const auto offset = static_cast<std::size_t>(row) * stride + column * 4;
                const bool different = !std::equal(before.begin() + offset, before.begin() + offset + 4, after.begin() + offset);
                if (different && !PtInRect(&bounds, POINT{static_cast<LONG>(column), static_cast<LONG>(row)})) { return E_FAIL; }
                changed = changed || different;
            }
        }
        if (!changed || pending_overlay_bitmap_.Get() != overlay.Get() || found->bitmap.Get() != icon_bitmap.Get()) { return E_FAIL; }
        state.activity = launcher::VersionDiscoveryActivity::running;
        if (!pending_overlay_visible(index)) { return E_FAIL; }
        hovered_icon_.reset();
        if (pending_overlay_visible(index)) { return E_FAIL; }
        hovered_icon_ = index;
        state.activity = launcher::VersionDiscoveryActivity::idle;
        if (pending_overlay_visible(index)) { return E_FAIL; }
        if (FAILED(render()) || FAILED(window_bitmap_->CopyPixels(nullptr, stride, static_cast<UINT>(after.size()), after.data())) ||
                before != after) { return E_FAIL; }
        std::wstring directory(32768, L'\0');
        const auto count = GetEnvironmentVariableW(L"LAUNCHER_HOVER_SMOKE_DIR", directory.data(), static_cast<DWORD>(directory.size()));
        if (count != 0 && count < directory.size()) {
            directory.resize(count);
            if (FAILED(launcher::write_png(wic_factory_.Get(), normal.Get(), std::filesystem::path(directory) / L"version-normal.png", dpi_, files_)) ||
                    FAILED(launcher::write_png(wic_factory_.Get(), pending.Get(), std::filesystem::path(directory) / L"version-pending.png", dpi_, files_))) { return E_FAIL; }
        }
        log_.write("Smoke pending overlay passed: queued/running binding only, attention-only visibility, icon clipping, "
            "completion removal and unchanged cached icon/overlay bitmaps; no animation timer.");
        return S_OK;
    }

    HRESULT verify_version_menu_pagination()
    {
        const auto fail = [this](const char* detail) { log_.write("Smoke version menu pagination failed: ", detail, "."); return E_FAIL; };
        const auto found = std::find_if(icons_.begin(), icons_.end(), [this](const auto& icon) {
            return icon.catalog && !icon.catalog->empty() &&
                latest_item_version(configuration_.tabs[active_tab_].items.at(icon.item_index)).has_value();
        });
        if (found == icons_.end()) {
            log_.write("Smoke version menu pagination passed: no installed version binding for catalog fixture.");
            return S_OK;
        }
        auto catalog = std::make_shared<std::vector<launcher::TargetVersion>>();
        catalog->reserve(4096);
        const auto prototype = *latest_item_version(configuration_.tabs[active_tab_].items.at(found->item_index));
        for (unsigned index = 0; index < 4096; ++index) {
            auto version = prototype;
            version.product_version = std::to_wstring(index) + L".0 资料 & edition";
            version.executable = prototype.executable.parent_path() / (L"Fixture " + std::to_wstring(index) + L".exe");
            catalog->push_back(std::move(version));
        }
        const auto root = std::make_shared<OwnedPopupMenu>();
        if (!root->menu) { return fail("cannot allocate the root menu"); }
        VersionMenuSession session;
        struct Restore final {
            std::shared_ptr<const std::vector<launcher::TargetVersion>>& catalog;
            std::shared_ptr<const std::vector<launcher::TargetVersion>> previous;
            VersionMenuSession*& active;
            VersionMenuSession* prior_active;
            ~Restore() { catalog = std::move(previous); active = prior_active; }
        } restore{found->catalog, found->catalog, active_version_menu_, active_version_menu_};
        found->catalog = catalog;
        const auto& item = configuration_.tabs[active_tab_].items.at(found->item_index);
        session.item = item;
        const auto action = resolved_item_action(item);
        const auto rows = append_installed_versions(root->menu, item, action, &session);
        active_version_menu_ = &session;
        if (rows.size() != 4096 || session.limited || session.pages.size() != 81 || GetMenuItemCount(root->menu) > 56) {
            return fail("catalog count, page count, inline limit or native allocation mismatch");
        }
        std::vector<bool> reached(4096);
        for (int position = 0; position < GetMenuItemCount(root->menu); ++position) {
            if (const auto index = version_menu_row(root->menu, position)) { reached.at(*index) = true; }
        }
        for (const auto& page : session.pages) {
            if (page.count > 50 || page.initialized || GetMenuItemCount(page.menu) != 1) { return fail("a lazy page was already populated or exceeded 50 rows"); }
            initialize_version_page(page.menu);
            if (GetMenuItemCount(page.menu) != static_cast<int>(page.count)) { return fail("a native page did not populate all expected rows"); }
            for (UINT position = 0; position < page.count; ++position) {
                const auto index = version_menu_row(page.menu, position);
                if (!index || *index != page.first + position || reached.at(*index)) { return fail("row identity was missing, duplicated or attached to another page"); }
                reached[*index] = true;
            }
        }
        const auto depth = [](auto&& self, HMENU menu) -> unsigned {
            unsigned result = 0;
            for (int position = 0; position < GetMenuItemCount(menu); ++position) {
                if (const auto child = GetSubMenu(menu, position)) { result = std::max(result, 1 + self(self, child)); }
            }
            return result;
        };
        if (!std::all_of(reached.begin(), reached.end(), [](bool value) { return value; }) || depth(depth, root->menu) > 3) { return fail("some accepted rows were unreachable or grouping depth exceeded three"); }
        log_.write("Smoke version menu pagination passed: all 4096 entries reachable, 50 rows per page, bounded grouping depth, "
            "lazy page construction, Unicode/ampersand labels and checked row identities; no filesystem work or target execution.");
        return S_OK;
    }

    void launch_icon(std::size_t index, std::string verb = {}, bool from_menu = false,
        bool reveal = false, const launcher::TargetVersion* installation = nullptr,
        const launcher::PreparedLaunch* snapshot = nullptr, bool alternate = false)
    {
        if (index >= icons_.size() || (!from_menu && !icon_tooltips_permitted())) { return; }
        const auto item_index = icons_[index].item_index;
        const auto& item = configuration_.tabs[active_tab_].items.at(item_index);
        const auto& action = alternate ? item.shift_launch : item.launch;
        if (!action) {
            if (smoke_mode_) { ++smoke_chirp_count_; }
            else { PlaySoundW(MAKEINTRESOURCEW(IDW_NO_ACTION_DING), GetModuleHandleW(nullptr), SND_RESOURCE | SND_ASYNC | SND_NODEFAULT | SND_NOSTOP); }
            return;
        }
        if (smoke_mode_) {
            ++smoke_launch_count_;
            smoke_last_launch_ = std::pair{active_tab_, item_index};
            smoke_cached_launch_ = action;
            if (!alternate) { return; } // Drawing tests never execute configured programs.
        }
        const auto label = icons_[index].label;
        if (alternate) {
            // Alternate activation owns its action immediately. Never reuse the
            // normal target's prepared launch or its pinned version selection.
            start_item_launch(item, label, verb, reveal, nullptr, nullptr, &*action, true);
            return;
        }
        const auto prepared = snapshot ? *snapshot : installation ? launcher::PreparedLaunch{} : prepared_icon_launch(index);
        // Mouse-up has already cleared pressed_icon_. Keep this action's
        // displayed target frozen through attention, collection and dispatch.
        const bool previous_hold = std::exchange(version_gesture_dispatch_, true);
        struct GestureHold final { bool& value; bool previous; ~GestureHold() { value = previous; } } hold{version_gesture_dispatch_, previous_hold};
        if (const auto key = version_search(item); key && !from_menu) {
            request_item_versions(item);
            if (!prepared.action) {
                const auto state = versions_->state(*key);
                if (state && state->activity != launcher::VersionDiscoveryActivity::idle) {
                    const auto source = std::pair{active_tab_, item_index};
                    const auto duplicate = std::find_if(pending_version_launches_.begin(), pending_version_launches_.end(),
                        [&](const auto& intent) { return intent.source == source && intent.verb == verb && intent.reveal == reveal; });
                    if (duplicate != pending_version_launches_.end()) { return; }
                    PendingVersionLaunch intent{item, label, std::move(verb), reveal, *key, state->last_attempt};
                    intent.source = source;
                    intent.pinned_version = default_item_version(item);
                    pending_version_launches_.push_back(std::move(intent));
                    if (versions_->retain_pending(*key)) {
                        log_.write("Version launch queued for initial discovery: label=", item.label, ".");
                        capture_version_results();
                        return;
                    }
                    pending_version_launches_.pop_back();
                }
                // A cold action consumes this visit's terminal outcome. Never
                // feed an unresolved pattern back to the synchronous preparer.
                std::optional<launcher::LaunchAction> resolved;
                if (state && state->snapshot && state->last_attempt &&
                    state->last_attempt->failure == launcher::VersionDiscoveryFailure::none &&
                    state->last_attempt->observation.status == launcher::TargetScanStatus::complete) {
                    const launcher::ItemVersionCatalog catalog(*item.launch, state->snapshot);
                    if (const auto selected = catalog.select(default_item_version(item)).version) { resolved = resolved_item_action(item, selected); }
                }
                const auto failure = resolved ? std::nullopt : version_selection_failure(state, default_item_version(item));
                start_item_launch(item, label, verb, reveal, nullptr, nullptr, resolved ? &*resolved : nullptr, true,
                    failure ? &*failure : nullptr);
                return;
            }
        }
        start_item_launch(item, label, verb, reveal, installation, installation ? nullptr : &prepared);
    }

    void start_item_launch(const launcher::Item& item, const std::wstring& label,
        const std::string& verb, bool reveal, const launcher::TargetVersion* installation = nullptr,
        const launcher::PreparedLaunch* snapshot = nullptr, const launcher::LaunchAction* resolved_override = nullptr,
        bool resolved_only = false, const launcher::LaunchResult* resolution_failure = nullptr)
    {
        if (window_ == nullptr) { return; }
        const auto resolved = snapshot && snapshot->action ? item.launch : resolved_only
            ? resolved_override ? std::optional(*resolved_override) : std::nullopt
            : resolved_item_action(item, installation);
        if (smoke_mode_) {
            smoke_cached_launch_ = resolved;
            smoke_cached_launch_label_ = label;
            return; // Queued-click smoke checks never activate configured targets.
        }
        auto completion = std::make_shared<LaunchCompletion>();
        completion->label = label;
        if (!resolved) {
            completion->result = resolution_failure ? *resolution_failure : launcher::LaunchResult{false, ERROR_FILE_NOT_FOUND,
                L"No installed version matches \"" + utf16(item.launch->target) + L"\"."};
            completion->ready.store(true, std::memory_order_release);
            launch_completions_.push_back(completion);
            finish_launch();
            return;
        }
        const auto action = *resolved;
        // A missing presentation is not a reason to perform path/registry work
        // on the UI thread. Its explicit action is prepared by this worker.
        const auto prepared = snapshot && snapshot->action ? std::optional(*snapshot) : std::nullopt;
        const auto directory = config_directory_;
#if LAUNCHER_VERSION_TEST_HOOKS
        const auto scenario = scenario_;
#endif
        launch_completions_.push_back(completion);
        try {
            // Shell activation can wait for another app or extension. Keep it
            // off the drawing thread; the worker owns all data it references.
            background_.start([completion, prepared, action, directory, verb, reveal
#if LAUNCHER_VERSION_TEST_HOOKS
                , scenario
#endif
                ] {
                try {
                    const auto launch = prepared ? *prepared : launcher::prepare_launch_action(action, directory);
                    if (!launch.action) {
                        completion->result = launch.result;
#if LAUNCHER_VERSION_TEST_HOOKS
                    } else if (scenario) {
                        scenario->capture_action(launch, verb, reveal);
                        completion->result = {true, 0, {}};
#endif
                    } else if (reveal) {
                        completion->result = launcher::reveal_launch_target(*launch.action, 0);
                    } else {
                        completion->result = launcher::launch_prepared_action(*launch.action, 0, verb);
                    }
                } catch (...) {
                    completion->result.error_code = ERROR_GEN_FAILURE;
                }
                completion->ready.store(true, std::memory_order_release);
            });
            log_.write("Launch requested: label=", item.label, "; handler=", action.handler,
                "; target=", (prepared ? utf8(prepared->action->target) : action.target), ".");
        } catch (const std::exception&) {
            completion->result = {false, ERROR_NOT_ENOUGH_MEMORY, L"Cannot start a launch worker."};
            completion->ready.store(true, std::memory_order_release);
            finish_launch();
        }
    }

    void recover_drawing()
    {
        discard_device_resources();
        if (window_ == nullptr || hidden_to_taskbar_ || taskbar_transition_ || display_layout_failed_) { return; }
        cancel_gap_tracking();
        cancel_pointer_tracking();
        clear_icon_tooltips();
        if (!smoke_mode_) { check_fullscreen(); }
        // Reapply clipping and the collapsed viewport even when the logical
        // state is unchanged, then present a fresh frame without activation.
        apply_state(true);
        if (!smoke_mode_) {
            pointer_changed(pointer_inside());
            POINT cursor{};
            if (GetCursorPos(&cursor) && WindowFromPoint(cursor) == window_) { track_pointer(); }
        }
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }

    HRESULT verify_drawing_recovery()
    {
        const auto saved_controller = peek_;
        const auto selected_tab = active_tab_;
        for (const bool accepted : {true, false}) {
            peek_ = launcher::PeekController(true);
            if (!accepted) { peek_.set_menu_open(true); }
            apply_state(true);
            HRESULT result = render();
            if (FAILED(result)) { return result; }
            RECT client{};
            GetClientRect(window_, &client);
            const auto size = drawing_target_->GetPixelSize();
            // Simulate the stale backbuffer that would stretch page artwork
            // into the narrow collapsed strip after a desktop transition.
            const auto stale_size = D2D1::SizeU(size.width + 17, size.height + 9);
            result = wic_factory_->CreateBitmap(stale_size.width, stale_size.height, GUID_WICPixelFormat32bppPBGRA,
                WICBitmapCacheOnLoad, window_bitmap_.ReleaseAndGetAddressOf());
            if (SUCCEEDED(result)) {
                const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                    static_cast<float>(dpi_), static_cast<float>(dpi_));
                result = d2d_factory_->CreateWicBitmapRenderTarget(window_bitmap_.Get(), properties,
                    drawing_target_.ReleaseAndGetAddressOf());
            }
            if (FAILED(result)) { return result; }
            result = render();
            if (FAILED(result)) { return result; }
            const auto repaired = drawing_target_->GetPixelSize();
            if (repaired.width != static_cast<UINT32>(client.right - client.left) ||
                    repaired.height != static_cast<UINT32>(client.bottom - client.top)) {
                log_.write("Smoke drawing recovery failed: stale backbuffer did not match the native client.");
                return E_FAIL;
            }
            const auto stale_target = drawing_target_; // Keep the old object alive to verify replacement.
            auto completion = std::make_shared<LaunchCompletion>();
            completion->label = accepted ? L"Test accepted elevation" : L"Test canceled elevation";
            completion->result = {accepted,
                static_cast<std::uint32_t>(accepted ? ERROR_SUCCESS : ERROR_CANCELLED), {}};
            completion->ready.store(true, std::memory_order_release);
            launch_completions_.push_back(completion);
            finish_launch(); // Exercise the actual UI completion path; no program is started.
            result = render();
            if (FAILED(result)) { return result; }
            const auto expected = accepted ? launcher::DockState::collapsed : launcher::DockState::expanded;
            if (drawing_target_.Get() == stale_target.Get() || active_tab_ != selected_tab ||
                    !verify_native_state(expected) || GetForegroundWindow() == window_ ||
                    std::any_of(icons_.begin(), icons_.end(), [](const IconVisual& icon) { return !icon.bitmap; })) {
                log_.write("Smoke drawing recovery failed: completion did not rebuild the current dock presentation.");
                return E_FAIL;
            }
        }
        peek_ = saved_controller;
        apply_state(true);
        log_.write("Smoke drawing recovery passed: mismatched surface, accepted/canceled elevation, collapsed/expanded frames.");
        return S_OK;
    }

    void finish_launch()
    {
        if (launch_dialog_active_ || menu_active_ || pressed_icon_ || drag_ || version_gesture_dispatch_) { return; }
        while (window_ != nullptr) {
            const auto ready = std::find_if(launch_completions_.begin(), launch_completions_.end(),
                [](const auto& completion) { return completion->ready.load(std::memory_order_acquire); });
            if (ready == launch_completions_.end()) { return; }
            const auto completion = *ready;
            launch_completions_.erase(ready);
            // Shell can switch to the secure desktop even for an ordinary Open
            // of a program requiring elevation. Recover on every completion,
            // including cancellation, without reloading the cached Shell icons.
            recover_drawing();
            if (completion->result.success) {
                log_.write("Launch accepted: label=", utf8(completion->label), ".");
                continue;
            }
            if (completion->result.error_code == ERROR_CANCELLED) {
                log_.write("Icon action canceled: label=", utf8(completion->label), ".");
                continue;
            }
            log_.result("Launch failed: label=" + utf8(completion->label),
                HRESULT_FROM_WIN32(completion->result.error_code));
#if LAUNCHER_VERSION_TEST_HOOKS
            if (scenario_) { scenario_->checkpoint("action-failed"); continue; }
#endif
            const auto message = L"Could not complete the action for \"" + completion->label + L"\".\n\n" +
                (completion->result.message.empty() ? L"Windows could not complete the launch."
                    : completion->result.message);
            launch_dialog_active_ = true;
            set_interaction_hold(true);
            MessageBoxW(window_, message.c_str(), L"Launcher", MB_OK | MB_ICONERROR);
            set_interaction_hold(false);
            launch_dialog_active_ = false;
        }
    }

    void set_interaction_hold(bool hold)
    {
        clear_icon_tooltips();
        interaction_hold_ = hold;
        cancel_timer();
        cancel_gap_tracking();
        cancel_pointer_tracking();
        if (hold) {
            peek_.set_suppressed(false);
            peek_.set_menu_open(true);
            apply_state();
        } else if (window_ != nullptr) {
            peek_.set_menu_open(false);
            check_fullscreen();
            sync_icon_tooltips();
        }
    }

    bool rebuild_view(const launcher::RuntimeAdjustments& candidate,
        std::size_t selected_tab, std::wstring& error)
    {
        if (window_ == nullptr) {
            error = L"The launcher has closed.";
            return false;
        }
        // Candidate layout/drawing may still roll back. Its temporary tab must
        // not start discovery or replace an interaction's adopted presentation.
        struct RebuildHold final {
            bool& value;
            bool previous;
            ~RebuildHold() { value = previous; }
        } hold{version_view_rebuilding_, std::exchange(version_view_rebuilding_, true)};
        clear_icon_tooltips();
        // Keep all resources and geometry belonging to the last working view.
        // A bad composed size or a native drawing failure can then roll back.
        const auto old_runtime = runtime_;
        const auto old_active_tab = active_tab_;
        const std::string operation = selected_tab == old_active_tab
            ? "Adjustment preview" : "Tab selection";
        const auto old_effective = effective_;
        const auto old_format = text_format_;
        const auto old_family = resolved_font_family_;
        const bool old_typographic = typographic_font_family_;
        const bool old_automatic_optical = automatic_optical_size_;
        const std::array<float, 10> old_dimensions{{width_dip_, height_dip_,
            parallel_span_dip_, panel_depth_dip_, tab_depth_dip_, fit_scale_,
            tab_slope_dip_, tab_label_padding_dip_, drawn_icon_size_dip_, icon_gap_dip_}};
        const auto old_panel = panel_bounds_;
        const auto old_placement = placement_;
        auto old_tabs = std::move(tabs_);
        auto old_tab_draw_order = std::move(tab_draw_order_);
        auto old_icons = std::move(icons_);
        try {
            effective_ = launcher::resolve_effective_settings(configuration_, candidate,
                desktop_icons_.size_dip, dpi_);
            runtime_ = candidate;
            active_tab_ = selected_tab;
            create_text_format();
            calculate_layout();
            update_placement(false);
            load_icons();
            if (std::any_of(icons_.begin(), icons_.end(),
                    [](const IconVisual& icon) { return !icon.image; })) {
                throw std::runtime_error("An icon and its fallback could not be loaded.");
            }
            discard_device_resources();
            cancel_pointer_tracking();
            apply_state(true);
            HRESULT result = render();
            if (result == D2DERR_RECREATE_TARGET) {
                result = render();
            }
            if (FAILED(result)) {
                log_.result(operation + " drawing failed", result);
                throw std::runtime_error("Windows could not draw the requested sizes.");
            }
            log_.write(operation, ": text-dip=", std::to_string(effective_.text_size_dip),
                "; icon-dip=", std::to_string(effective_.icon_size_dip),
                "; icon-and-edge-gap-px=", std::to_string(effective_.icon_spacing_px), ".");
            return true;
        } catch (const std::exception& failure) {
            error = utf16(failure.what());
            log_.write(operation, " rejected: ", failure.what());
        }
        clear_icon_tooltips();
        discard_device_resources();
        runtime_ = old_runtime;
        active_tab_ = old_active_tab;
        effective_ = old_effective;
        text_format_ = old_format;
        resolved_font_family_ = old_family;
        typographic_font_family_ = old_typographic;
        automatic_optical_size_ = old_automatic_optical;
        width_dip_ = old_dimensions[0];
        height_dip_ = old_dimensions[1];
        parallel_span_dip_ = old_dimensions[2];
        panel_depth_dip_ = old_dimensions[3];
        tab_depth_dip_ = old_dimensions[4];
        fit_scale_ = old_dimensions[5];
        tab_slope_dip_ = old_dimensions[6];
        tab_label_padding_dip_ = old_dimensions[7];
        drawn_icon_size_dip_ = old_dimensions[8];
        icon_gap_dip_ = old_dimensions[9];
        panel_bounds_ = old_panel;
        placement_ = old_placement;
        tabs_ = std::move(old_tabs);
        tab_draw_order_ = std::move(old_tab_draw_order);
        icons_ = std::move(old_icons);
        discard_device_resources();
        if (window_ != nullptr) {
            load_icons(); // Supersede a rejected candidate's size generation.
            apply_state(true);
            InvalidateRect(window_, nullptr, FALSE);
        }
        return false;
    }

    std::wstring adjustment_description() const
    {
        std::wostringstream text;
        text << std::fixed << std::setprecision(1)
            << L"Text: " << effective_.text_size_dip * 0.75F << L" pt, " << resolved_font_family_
            << L"\r\nIcons: " << effective_.icon_size_dip << L" DIP"
            << L" (desktop: " << desktop_icons_.size_dip << L" DIP)"
            << L"\r\nIcon/edge spacing: " << effective_.icon_spacing_px << L" px"
            << L"\r\nThickness: " << std::defaultfloat << std::setprecision(6) << effective_.thickness_dip
            << L" DIP; drawn: " << sheet_thickness_pixels() << L" px at "
            << std::lround(static_cast<double>(dpi_) * 100.0 / 96.0) << L"%";
        return text.str();
    }

    bool persist_adjustments(const launcher::RuntimeAdjustments& candidate,
        std::wstring& error)
    {
        if (window_ == nullptr || smoke_mode_) {
            error = L"The launcher cannot save adjustments in its current state.";
            return false;
        }
        try {
            launcher::save_runtime_adjustments(runtime_path_, candidate, files_);
            log_.write("Runtime adjustments saved.");
            return true;
        } catch (const std::exception& failure) {
            error = utf16(failure.what());
            log_.write("Runtime adjustments could not be saved: ", failure.what());
            return false;
        }
    }

    void adjust_sizes()
    {
        if (interaction_hold_ || smoke_mode_ || window_ == nullptr) {
            return;
        }
        set_interaction_hold(true);
        launcher::AdjustmentCallbacks callbacks;
        callbacks.preview = [this](const launcher::RuntimeAdjustments& candidate,
            std::wstring& error) { return rebuild_view(candidate, active_tab_, error); };
        callbacks.commit = [this](const launcher::RuntimeAdjustments& candidate,
            std::wstring& error) { return persist_adjustments(candidate, error); };
        callbacks.describe = [this] { return adjustment_description(); };
        callbacks.default_icon_spacing_px = [this] {
            return launcher::resolve_effective_settings(configuration_, {}, desktop_icons_.size_dip,
                dpi_).icon_spacing_px;
        };
        callbacks.default_thickness_dip = [this] { return configuration_.appearance.thickness_dip; };
        callbacks.load_presets = [this] { return launcher::load_adjustment_presets(runtime_path_, files_); };
        callbacks.store_preset = [this](std::string_view name, const launcher::RuntimeAdjustments& candidate, std::wstring& error) {
            try {
                auto snapshot = candidate;
                snapshot.edge_position = effective_.edge_position;
                launcher::store_adjustment_preset(runtime_path_, name, snapshot, files_);
                return true;
            } catch (const std::exception& failure) {
                error = utf16(failure.what());
                return false;
            }
        };
        callbacks.dialog_window = [this](HWND dialog) {
            adjustment_dialog_ = dialog;
            if (dialog != nullptr && test_ui_mode_) {
                // Match the main window's opt-in automation visibility while
                // preserving the normal owned-dialog presentation.
                const LONG_PTR style = GetWindowLongPtrW(dialog, GWL_EXSTYLE);
                SetWindowLongPtrW(dialog, GWL_EXSTYLE,
                    (style | WS_EX_APPWINDOW) & ~static_cast<LONG_PTR>(WS_EX_TOOLWINDOW));
                SetWindowPos(dialog, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE |
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }
        };
        const INT_PTR result = launcher::show_adjustments_dialog(instance_, window_, runtime_, callbacks);
        if (result == -1) {
            log_.result("Adjustment dialog creation failed", HRESULT_FROM_WIN32(GetLastError()));
        }
        adjustment_dialog_ = nullptr;
        set_interaction_hold(false);
    }

    struct DragState final {
        launcher::RuntimeAdjustments original;
        POINT initial_pointer{};
        double initial_position = 0.5;
        int usable_span = 0;
    };

    void begin_drag(POINT client)
    {
        if (interaction_hold_ || smoke_mode_ || !blank_panel_point(client)) {
            return;
        }
        POINT cursor{};
        if (!GetCursorPos(&cursor)) {
            return;
        }
        const RECT work = monitor_.information.rcWork;
        const int usable_span = vertical()
            ? work.bottom - work.top - (placement_.expanded.bottom - placement_.expanded.top)
            : work.right - work.left - (placement_.expanded.right - placement_.expanded.left);
        drag_ = DragState{runtime_, cursor, effective_.edge_position, std::max(0, usable_span)};
        set_interaction_hold(true);
        // This explicit click starts an interaction, unlike hover. Foreground
        // activation gives native capture its full scope and enables Escape.
        SetForegroundWindow(window_);
        SetFocus(window_);
        SetCapture(window_);
        if (GetCapture() != window_) {
            drag_.reset();
            set_interaction_hold(false);
            return;
        }
        log_.write("Edge drag started.");
    }

    void update_drag()
    {
        if (!drag_) {
            return;
        }
        POINT cursor{};
        if (!GetCursorPos(&cursor)) {
            return;
        }
        const long delta = vertical() ? cursor.y - drag_->initial_pointer.y
            : cursor.x - drag_->initial_pointer.x;
        const double position = drag_->usable_span == 0 ? drag_->initial_position
            : std::clamp(drag_->initial_position + static_cast<double>(delta) /
                drag_->usable_span, 0.0, 1.0);
        if (position == effective_.edge_position) {
            return;
        }
        runtime_.edge_position = position;
        effective_.edge_position = position;
        update_placement(false, false);
        apply_state(true);
    }

    void finish_drag(bool commit)
    {
        if (!drag_) {
            return;
        }
        // Clear the state before ReleaseCapture synchronously delivers
        // WM_CAPTURECHANGED, so a successful release cannot cancel itself.
        const auto original = drag_->original;
        drag_.reset();
        if (GetCapture() == window_) {
            ReleaseCapture();
        }
        std::wstring error;
        if (commit && !persist_adjustments(runtime_, error)) {
            commit = false;
        }
        if (!commit) {
            runtime_ = original;
            effective_.edge_position = runtime_.edge_position.value_or(configuration_.dock.position);
            update_placement(false);
            apply_state(true);
        }
        log_.write(commit ? "Edge drag saved." : "Edge drag canceled.");
        if (!error.empty() && window_ != nullptr) {
            MessageBoxW(window_, error.c_str(), L"Launcher", MB_OK | MB_ICONERROR);
        }
        set_interaction_hold(false);
    }

    void open_configuration(bool folder)
    {
        const auto& path = folder ? config_directory_ : config_path_;
        SHELLEXECUTEINFOW operation{};
        operation.cbSize = sizeof(operation);
        operation.fMask = SEE_MASK_FLAG_NO_UI;
        operation.hwnd = window_;
        operation.lpVerb = L"open";
        operation.lpFile = path.c_str();
        operation.nShow = SW_SHOWNORMAL;
        bool opened = false;
        DWORD error = ERROR_SUCCESS;
        if (folder) {
            // Reveal the actual configuration item through its Shell identity.
            // This opens its parent and selects the file instead of relying on
            // the generic folder-open verb or Explorer's existing location.
            launcher::NativeLaunchAction target;
            target.target = config_path_.wstring();
            const auto result = launcher::reveal_launch_target(target, reinterpret_cast<std::uintptr_t>(window_));
            opened = result.success;
            error = result.error_code;
        } else {
            opened = ShellExecuteExW(&operation) != FALSE;
            if (!opened) { error = GetLastError(); }
        }
        if (opened) {
            if (folder) {
                log_.write("Opened configuration folder: ", utf8(config_directory_.wstring()),
                    "; selected=", utf8(config_path_.wstring()), ".");
            } else {
                log_.write("Opened configuration in associated editor: ", utf8(config_path_.wstring()), ".");
            }
            return;
        }
        if (window_ == nullptr) {
            return;
        }
        if (!folder && (error == ERROR_NO_ASSOCIATION ||
                reinterpret_cast<INT_PTR>(operation.hInstApp) == SE_ERR_ASSOCINCOMPLETE)) {
            // Use Windows' text editor when TOML has no registered application.
            std::array<wchar_t, 32768> directory{};
            const UINT length = GetSystemDirectoryW(directory.data(),
                static_cast<UINT>(directory.size()));
            if (length > 0 && length < directory.size()) {
                const auto editor = std::filesystem::path(directory.data()) / L"notepad.exe";
                const std::wstring arguments = L"\"" + config_path_.wstring() + L"\"";
                operation.lpFile = editor.c_str();
                operation.lpParameters = arguments.c_str();
                if (ShellExecuteExW(&operation)) {
                    log_.write("Opened configuration in Notepad.");
                    return;
                }
                error = GetLastError();
            } else {
                error = ERROR_PATH_NOT_FOUND;
            }
        }
        if (window_ == nullptr) {
            return;
        }
        log_.result("Opening configuration failed", HRESULT_FROM_WIN32(error));
        const std::wstring message = L"Windows could not open " + path.wstring() +
            L".\nError " + std::to_wstring(error) + L".";
        MessageBoxW(window_, message.c_str(), L"Launcher", MB_OK | MB_ICONERROR);
    }

    static LRESULT CALLBACK version_menu_filter(int code, WPARAM word, LPARAM parameter)
    {
        auto* owner = menu_key_target_;
        if (code == MSGF_MENU && owner && owner->active_version_menu_ && !owner->active_version_menu_->tracking_child) {
            const auto* input = reinterpret_cast<const MSG*>(parameter);
            const bool key = input && (input->message == WM_KEYDOWN || input->message == WM_SYSKEYDOWN) &&
                (input->wParam == VK_APPS || (input->wParam == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000) != 0));
            if (key && owner->active_version_menu_->focused_row) {
                if ((input->lParam & (1LL << 30)) == 0) {
                    const auto [menu, position] = *owner->active_version_menu_->focused_row;
                    RECT rectangle{};
                    if (GetMenuItemRect(owner->window_, menu, position, &rectangle)) {
                        try {
                            owner->scenario_checkpoint("keyboard-context-opened");
                            owner->version_row_context_menu(menu, position, POINT{rectangle.right, rectangle.top});
                        }
                        catch (...) {} // Never unwind through a Windows hook callback.
                    }
                }
                return 1;
            }
        }
        return CallNextHookEx(nullptr, code, word, parameter);
    }

    void select_version_menu_row(HMENU menu, UINT command, UINT flags)
    {
        if (!active_version_menu_ || active_version_menu_->tracking_child) { return; }
        active_version_menu_->focused_row.reset();
        if (!menu || (flags & (MF_POPUP | MF_SEPARATOR | MF_DISABLED | MF_GRAYED)) != 0) { return; }
        const int count = GetMenuItemCount(menu);
        for (int position = 0; position < count; ++position) {
            if (GetMenuItemID(menu, position) == command && version_menu_row(menu, static_cast<UINT>(position))) {
                active_version_menu_->focused_row = std::pair{menu, static_cast<UINT>(position)};
                return;
            }
        }
    }

    void version_row_context_menu(HMENU parent, UINT position, POINT screen)
    {
        const auto selected = version_menu_row(parent, position);
        if (!selected || !active_version_menu_ || active_version_menu_->tracking_child) {
            scenario_checkpoint("invalid-version-row");
            return;
        }
        auto& session = *active_version_menu_;
        const auto installation = session.versions.at(*selected);
        const auto action = resolved_item_action(session.item, &installation);
        if (!action) { return; }
        const auto prepared = launcher::prepare_exact_target_action(*action, config_directory_);
        const auto owner = std::make_shared<OwnedPopupMenu>();
        if (!owner->menu) { return; }
        const auto enabled = [](bool available) { return MF_STRING | (available ? 0U : MF_GRAYED); };
        auto extension = installation.executable.extension().wstring();
        CharLowerBuffW(extension.data(), static_cast<DWORD>(extension.size()));
        const bool administrator = prepared.action && (extension == L".exe" || extension == L".com" ||
            extension == L".bat" || extension == L".cmd" || extension == L".lnk");
        bool built = AppendMenuW(owner->menu, enabled(prepared.action.has_value()), kVersionOpenCommand, L"Open") != FALSE;
        built = AppendMenuW(owner->menu, enabled(administrator), kVersionAdministratorCommand, L"Run as administrator") != FALSE && built;
        built = AppendMenuW(owner->menu, MF_STRING, kVersionRevealCommand, L"Reveal in Explorer") != FALSE && built;
        built = AppendMenuW(owner->menu, MF_STRING, kVersionPropertiesCommand, L"Properties") != FALSE && built;
        if (prepared.action) { SetMenuDefaultItem(owner->menu, kVersionOpenCommand, FALSE); }
        decorate_context_menu(owner->menu, {
            {kVersionAdministratorCommand, launcher::MenuIcon::shield},
            {kVersionRevealCommand, launcher::MenuIcon::folder}});
        built = AppendMenuW(owner->menu, MF_SEPARATOR, 0, nullptr) != FALSE && built;
        const bool is_default = session.saved_default && session.normal_action &&
            installation.executable.wstring() == utf16(session.normal_action->target);
        built = AppendMenuW(owner->menu, MF_STRING | (is_default ? MF_CHECKED : 0U), kVersionSetDefaultCommand, L"Set as default") != FALSE && built;
        const auto verbs = std::min<std::size_t>(session.item.context_verbs.size(), kVersionShellFirst - kVersionConfiguredFirst);
        if (verbs != 0) { built = AppendMenuW(owner->menu, MF_SEPARATOR, 0, nullptr) != FALSE && built; }
        for (std::size_t verb = 0; verb < verbs; ++verb) {
            auto label = utf16(session.item.context_verbs[verb].label);
            for (std::size_t ampersand = 0; (ampersand = label.find(L'&', ampersand)) != std::wstring::npos; ampersand += 2) {
                label.insert(ampersand, 1, L'&');
            }
            built = AppendMenuW(owner->menu, enabled(prepared.action.has_value()), kVersionConfiguredFirst + static_cast<UINT>(verb), label.c_str()) != FALSE && built;
        }
        auto native = std::make_shared<LazyShellMenu>();
        native->owner = window_;
        native->target = installation.executable.wstring();
        native->extended = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        native->first_command = kVersionShellFirst;
        native->last_command = kVersionShellLast;
        native->menu = CreatePopupMenu();
        if (native->menu) {
            if (!AppendMenuW(native->menu, MF_STRING | MF_GRAYED, 0, L"Loading Windows commands...") ||
                    !AppendMenuW(owner->menu, MF_POPUP, reinterpret_cast<UINT_PTR>(native->menu), L"Windows commands")) {
                DestroyMenu(native->menu);
                native->menu = nullptr;
                built = false;
            }
        } else { built = false; }
        if (!built) {
            session.limited = true;
            AppendMenuW(owner->menu, MF_STRING | MF_GRAYED, 0, L"Some file commands are unavailable");
            log_.write("Version context menu is limited: native allocation failed.");
        }
        UINT command = 0;
        const auto prior_focus = session.focused_row;
        {
            struct ChildHold final { bool& value; ~ChildHold() { value = false; } } hold{session.tracking_child};
            session.tracking_child = true;
            session.child_menu = owner->menu;
            ShellMenuFrame forwarding(active_shell_frames_, native.get());
            RECT excluded{};
            TPMPARAMS parameters{sizeof(parameters)};
            if (GetMenuItemRect(window_, parent, position, &excluded)) { parameters.rcExclude = excluded; }
            command = TrackPopupMenuEx(owner->menu, TPM_RECURSE | TPM_RETURNCMD | TPM_RIGHTBUTTON,
                screen.x, screen.y, window_, &parameters);
        }
        session.child_menu = nullptr;
        session.focused_row = prior_focus;
        if (command != 0) {
            session.choice = VersionMenuChoice{*selected, command, screen, owner, native};
            EndMenu(); // Child tracking has returned; close the parent before dispatch.
        } else { scenario_checkpoint("child-menu-canceled"); }
    }

    HRESULT invoke_shell_command(LazyShellMenu& frame, UINT command, POINT screen, const launcher::PreparedLaunch& prepared)
    {
        if (!prepared.action || !frame.shell) { return E_INVALIDARG; }
#if LAUNCHER_VERSION_TEST_HOOKS
        if (scenario_) {
            auto verb = utf8(frame.shell->verb(command));
            scenario_->capture_action(prepared, verb.empty() ? "shell-command" : verb, false);
            return S_OK;
        }
#endif
        return frame.shell->invoke(command, screen, prepared.action->arguments, prepared.action->working_directory);
    }

    void open_filesystem_error_log(const std::filesystem::path& journal)
    {
        if (!launcher::AppNotification::valid_journal_path(journal)) { return; }
        auto completion = std::make_shared<LaunchCompletion>();
        completion->label = L"Filesystem error log";
        auto target = filesystem_log_action(journal);
        launch_completions_.push_back(completion);
        try {
            background_.start([completion, target = std::move(target)] {
                try { completion->result = launcher::launch_prepared_action(target); }
                catch (...) { completion->result = {false, ERROR_GEN_FAILURE, L"Windows could not open the filesystem error log."}; }
                completion->ready.store(true, std::memory_order_release);
            });
            log_.write("Filesystem notification clicked; opening error log.");
            scenario_checkpoint("filesystem-notification-open-log");
        } catch (...) {
            completion->result = {false, ERROR_NOT_ENOUGH_MEMORY, L"Cannot start an error-log opening worker."};
            completion->ready.store(true, std::memory_order_release);
            finish_launch();
        }
    }

    void start_properties(const launcher::NativeLaunchAction& target, const std::wstring& label)
    {
        if (smoke_mode_) { smoke_properties_target_ = target.target; return; }
        auto completion = std::make_shared<LaunchCompletion>();
        completion->label = label;
        launch_completions_.push_back(completion);
#if LAUNCHER_VERSION_TEST_HOOKS
        const auto scenario = scenario_;
#endif
        try {
            background_.start([completion, target
#if LAUNCHER_VERSION_TEST_HOOKS
                , scenario
#endif
                ] {
                try {
#if LAUNCHER_VERSION_TEST_HOOKS
                    if (scenario) {
                        launcher::PreparedLaunch captured{target, {true, 0, {}}};
                        scenario->capture_action(captured, "properties", false);
                        completion->result = {true, 0, {}};
                    } else
#endif
                    completion->result = launcher::show_target_properties(target);
                }
                catch (...) { completion->result = {false, ERROR_GEN_FAILURE, L"Windows could not show file properties."}; }
                completion->ready.store(true, std::memory_order_release);
            });
        } catch (...) {
            completion->result = {false, ERROR_NOT_ENOUGH_MEMORY, L"Cannot start a Properties worker."};
            completion->ready.store(true, std::memory_order_release);
            finish_launch();
        }
    }

    void dispatch_version_menu_choice(const VersionMenuSession& session, const std::wstring& label)
    {
        if (!session.choice || session.choice->row >= session.versions.size()) { return; }
        const auto& choice = *session.choice;
        const auto& installation = session.versions.at(choice.row);
        if (choice.command == kVersionSetDefaultCommand) {
            change_default_version(session.source, installation.executable);
            return;
        }
        const auto action = resolved_item_action(session.item, &installation);
        if (!action) { return; }
        const auto prepared = launcher::prepare_exact_target_action(*action, config_directory_);
        const auto row_label = label + L" (" + installation.product_version + L")";
        const auto launch = [&](const std::string& verb, bool reveal = false) {
            start_item_launch(session.item, row_label, verb, reveal, &installation, prepared.action ? &prepared : nullptr);
        };
        const auto properties = [&] {
            launcher::NativeLaunchAction file;
            file.target = installation.executable.wstring();
            start_properties(file, row_label);
        };
        if (choice.command == kVersionOpenCommand) { launch({}); }
        else if (choice.command == kVersionAdministratorCommand) { launch("runas"); }
        else if (choice.command == kVersionRevealCommand) {
            if (prepared.action) { launch({}, true); }
            else {
                launcher::NativeLaunchAction file;
                file.target = installation.executable.wstring();
                const launcher::PreparedLaunch reveal{file, {true, 0, {}}};
                start_item_launch(session.item, row_label, {}, true, &installation, &reveal);
            }
        }
        else if (choice.command == kVersionPropertiesCommand) { properties(); }
        else if (choice.command >= kVersionConfiguredFirst && choice.command < kVersionShellFirst &&
                choice.command - kVersionConfiguredFirst < session.item.context_verbs.size()) {
            launch(session.item.context_verbs[choice.command - kVersionConfiguredFirst].verb);
        } else if (choice.shell && choice.shell->shell && choice.shell->shell->owns(choice.command)) {
            const auto canonical = choice.shell->shell->verb(choice.command);
            if (_wcsicmp(canonical.c_str(), L"open") == 0) { launch({}); }
            else if (_wcsicmp(canonical.c_str(), L"runas") == 0) { launch("runas"); }
            else if (_wcsicmp(canonical.c_str(), L"properties") == 0) { properties(); }
            else if (prepared.action) {
                ShellMenuFrame forwarding(active_shell_frames_, choice.shell.get());
                const HRESULT invoked = invoke_shell_command(*choice.shell, choice.command, choice.screen, prepared);
                if (FAILED(invoked) && invoked != HRESULT_FROM_WIN32(ERROR_CANCELLED)) { log_.result("Windows version command failed", invoked); }
            }
        }
    }

    static void append_activation_commands(HMENU menu, const launcher::Item& item, bool open_available)
    {
        if (item.launch) {
            AppendMenuW(menu, MF_STRING | (open_available ? 0U : MF_GRAYED), kIconOpenCommand, L"Open");
            if (open_available) { SetMenuDefaultItem(menu, kIconOpenCommand, FALSE); }
        }
        if (item.shift_launch) {
            AppendMenuW(menu, MF_STRING, kIconAlternateCommand, L"Alternate");
            if (!item.launch) { SetMenuDefaultItem(menu, kIconAlternateCommand, FALSE); }
        }
    }

    void icon_context_menu(std::size_t index, POINT screen)
    {
        if (index >= icons_.size()) { return; }
        const auto& item = configuration_.tabs[active_tab_].items.at(icons_[index].item_index);
        struct MenuOwner final {
            HMENU handle = CreatePopupMenu();
            ~MenuOwner() { if (handle) { DestroyMenu(handle); } }
        } owner;
        const HMENU menu = owner.handle;
        if (!menu) { return; }
        // Pointer hover belongs to the page, while this highlight belongs to
        // the whole icon-menu chain, including nested version and Shell menus.
        struct MenuHighlight final {
            std::optional<std::size_t>& icon;
            std::optional<std::size_t> previous;
            HWND& window;
            ~MenuHighlight() {
                icon = previous;
                if (window) { InvalidateRect(window, nullptr, FALSE); }
            }
        } highlight{menu_icon_, menu_icon_, window_};
        menu_icon_ = item.launch || item.shift_launch ? std::optional(index) : std::nullopt;
        InvalidateRect(window_, nullptr, FALSE);
        // The page and icon indices remain stable throughout Shell menu callbacks.
        menu_active_ = true;
        pressed_icon_.reset();
        clear_icon_tooltips();
        peek_.set_menu_open(true);
        apply_state();
        sync_timer();
        try {
            if (versioned_item(item)) { request_item_versions(item); }
            const auto action = resolved_item_action(item);
            const auto prepared = prepared_icon_launch(index);
            const auto target = prepared.action ? prepared.action->target : std::wstring{};
            const bool administrator = icons_[index].can_admin;
            const bool reveal = icons_[index].can_reveal;
            const auto enabled = [](bool available) { return MF_STRING | (available ? 0U : MF_GRAYED); };
            const auto separate = [menu] {
                const int count = GetMenuItemCount(menu);
                if (count > 0 && (GetMenuState(menu, static_cast<UINT>(count - 1), MF_BYPOSITION) & MF_SEPARATOR) == 0) {
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                }
            };
            append_activation_commands(menu, item, action.has_value());
            AppendMenuW(menu, enabled(administrator), kIconAdministratorCommand, L"Run as administrator");
            AppendMenuW(menu, enabled(reveal), kIconRevealCommand, L"Reveal in Explorer");
            decorate_context_menu(menu, {
                {kIconAdministratorCommand, launcher::MenuIcon::shield},
                {kIconRevealCommand, launcher::MenuIcon::folder}});
            if (versioned_item(item)) {
                separate();
                AppendMenuW(menu, MF_STRING | (default_item_version(item) ? 0U : MF_CHECKED), kIconLatestCommand, L"Always use latest");
            }
            if (versioned_item(item) && !selected_item_version(item)) {
                separate();
                const auto description = version_description(item);
                AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, description.c_str());
            }
            VersionMenuSession session;
            session.item = item;
            session.source = {active_tab_, icons_[index].item_index};
            session.root = menu;
            const auto installed_versions = append_installed_versions(menu, item, action, &session);
            const auto version_count = installed_versions.size();
            const auto verb_count = std::min(item.context_verbs.size(),
                static_cast<std::size_t>(kIconVersionFirst - kIconConfiguredFirst));
            if (verb_count != 0) { separate(); }
            for (std::size_t verb = 0; verb < verb_count; ++verb) {
                auto label = utf16(item.context_verbs[verb].label);
                for (std::size_t ampersand = 0; (ampersand = label.find(L'&', ampersand)) != std::wstring::npos;
                        ampersand += 2) {
                    label.insert(ampersand, 1, L'&');
                }
                AppendMenuW(menu, MF_STRING, kIconConfiguredFirst + static_cast<UINT>(verb), label.c_str());
            }
            LazyShellMenu lazy{window_, target, nullptr, (GetKeyState(VK_SHIFT) & 0x8000) != 0};
            bool native_attached = false;
            HMENU native_menu = !target.empty() ? CreatePopupMenu() : nullptr;
            if (native_menu) {
                if (AppendMenuW(native_menu, MF_STRING | MF_GRAYED, 0, L"Loading Windows commands...")) {
                    separate();
                    if (!AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(native_menu), L"Windows commands")) {
                        DestroyMenu(native_menu);
                    } else {
                        native_attached = true;
                        lazy.menu = native_menu;
                    }
                } else {
                    DestroyMenu(native_menu);
                }
            }
            ShellMenuFrame forwarding(active_shell_frames_, native_attached ? &lazy : nullptr);
            struct SessionOwner final {
                VersionMenuSession*& active;
                VersionMenuSession* previous;
                HHOOK hook = nullptr;
                Window* previous_target;
                ~SessionOwner() {
                    if (hook) { UnhookWindowsHookEx(hook); }
                    active = previous;
                    menu_key_target_ = previous_target;
                }
            } tracking{active_version_menu_, active_version_menu_, nullptr, menu_key_target_};
            active_version_menu_ = &session;
            menu_key_target_ = this;
            tracking.hook = SetWindowsHookExW(WH_MSGFILTER, version_menu_filter, nullptr, GetCurrentThreadId());
            if (!tracking.hook) { log_.result("Version menu keyboard context unavailable", HRESULT_FROM_WIN32(GetLastError())); }
            std::optional<launcher::PopupMenuDrawing> drawing;
            if (!installed_versions.empty()) {
                drawing.emplace(window_, menu, dpi_, reinterpret_cast<ULONG_PTR>(kInstalledVersionsHeading), lazy.menu);
            }
            SetForegroundWindow(window_);
            const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTBUTTON,
                screen.x, screen.y, window_, nullptr);
            scenario_checkpoint("menu-chain-closed");
            if (window_ != nullptr) {
                if (session.choice) { dispatch_version_menu_choice(session, icons_[index].label); }
                else if (command == kIconLatestCommand) { change_default_version(session.source, std::nullopt); }
                else if (command == kIconOpenCommand) { launch_icon(index, {}, true, false, nullptr, &prepared); }
                else if (command == kIconAlternateCommand) { launch_icon(index, {}, true, false, nullptr, nullptr, true); }
                else if (command == kIconAdministratorCommand && administrator) { launch_icon(index, "runas", true, false, nullptr, &prepared); }
                else if (command == kIconRevealCommand && reveal) { launch_icon(index, {}, true, true, nullptr, &prepared); }
                else if (command >= kIconVersionFirst && command < kIconVersionFirst + version_count) {
                    launch_icon(index, {}, true, false, &installed_versions[command - kIconVersionFirst]);
                }
                else if (command >= kIconConfiguredFirst && command < kIconConfiguredFirst + verb_count) {
                    launch_icon(index, item.context_verbs[command - kIconConfiguredFirst].verb, true, false, nullptr, &prepared);
                } else if (lazy.shell && lazy.shell->owns(command)) {
                    const auto canonical = lazy.shell->verb(command);
                    // These two verbs use Launcher preparation to retain arguments
                    // and shortcut working-directory behavior even in the submenu.
                    if (_wcsicmp(canonical.c_str(), L"open") == 0 || _wcsicmp(canonical.c_str(), L"runas") == 0) {
                        launch_icon(index, utf8(canonical), true, false, nullptr, &prepared);
                    } else {
                        const HRESULT invoked = invoke_shell_command(lazy, command, screen, prepared);
                        if (FAILED(invoked) && invoked != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
                            log_.result("Windows icon command failed", invoked);
                            MessageBoxW(window_, L"Windows could not complete this command.",
                                L"Launcher", MB_OK | MB_ICONERROR);
                        }
                    }
                }
            }
        } catch (const std::exception& error) {
            log_.write("Icon context menu failed: ", error.what());
        }
        if (window_ != nullptr) {
            menu_active_ = false;
            finish_version_discovery();
            peek_.set_menu_open(false);
            pointer_changed(pointer_inside());
            sync_icon_tooltips();
            sync_hover();
            PostMessageW(window_, WM_NULL, 0, 0);
        }
    }

    void hide_to_taskbar()
    {
        if (hidden_to_taskbar_ || taskbar_transition_) { return; }
        finish_drag(false);
        hidden_to_taskbar_ = true;
        taskbar_transition_ = true;
        sync_version_activation();
        menu_active_ = false;
        peek_.set_menu_open(false);
        peek_.pointer_changed(false);
        cancel_motion();
        cancel_timer();
        cancel_gap_tracking();
        cancel_pointer_tracking();
        clear_icon_tooltips();
        // Hide before changing taskbar eligibility so Explorer sees the new style.
        ShowWindow(window_, SW_HIDE);
        const auto extended = GetWindowLongPtrW(window_, GWL_EXSTYLE);
        SetWindowLongPtrW(window_, GWL_EXSTYLE,
            (extended | WS_EX_APPWINDOW) & ~static_cast<LONG_PTR>(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE));
        const auto style = GetWindowLongPtrW(window_, GWL_STYLE);
        SetWindowLongPtrW(window_, GWL_STYLE, style | WS_SYSMENU | WS_MINIMIZEBOX);
        if (!SetWindowPos(window_, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED)) {
            throw std::runtime_error("Cannot minimize Launcher to the taskbar.");
        }
        ShowWindow(window_, SW_SHOWMINNOACTIVE);
        taskbar_transition_ = false;
        log_.write("Launcher hidden to the taskbar.");
    }

    void restore_from_taskbar()
    {
        if (!hidden_to_taskbar_ || taskbar_transition_) { return; }
        taskbar_transition_ = true;
        ShowWindow(window_, SW_HIDE);
        const auto extended = GetWindowLongPtrW(window_, GWL_EXSTYLE);
        SetWindowLongPtrW(window_, GWL_EXSTYLE,
            (extended & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW | WS_EX_TOOLWINDOW)) |
                WS_EX_NOACTIVATE | (test_ui_mode_ ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW));
        const auto style = GetWindowLongPtrW(window_, GWL_STYLE);
        SetWindowLongPtrW(window_, GWL_STYLE, style & ~static_cast<LONG_PTR>(WS_SYSMENU | WS_MINIMIZEBOX));
        if (!SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED)) {
            throw std::runtime_error("Cannot restore the Launcher dock.");
        }
        // Clear native minimization without stealing focus. Synchronous resize
        // messages during this transition must not start another restoration.
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        hidden_to_taskbar_ = false;
        taskbar_transition_ = false;
        peek_ = launcher::PeekController(configuration_.dock.auto_hide);
        if (interaction_hold_) { peek_.set_menu_open(true); }
        refresh_display();
        log_.write("Launcher restored from the taskbar.");
    }

    HRESULT verify_taskbar_hide()
    {
        const auto original_tab = active_tab_;
        const auto dock_style = [this] {
            const auto extended = GetWindowLongPtrW(window_, GWL_EXSTYLE);
            const bool taskbar_style = test_ui_mode_ ? (extended & WS_EX_APPWINDOW) != 0
                : (extended & WS_EX_TOOLWINDOW) != 0 && (extended & WS_EX_APPWINDOW) == 0;
            return !hidden_to_taskbar_ && !IsIconic(window_) && taskbar_style &&
                (extended & (WS_EX_TOPMOST | WS_EX_NOACTIVATE)) == (WS_EX_TOPMOST | WS_EX_NOACTIVATE);
        };
        const auto minimized_style = [this] {
            const auto extended = GetWindowLongPtrW(window_, GWL_EXSTYLE);
            return hidden_to_taskbar_ && IsIconic(window_) && IsWindowVisible(window_) &&
                (extended & WS_EX_APPWINDOW) != 0 &&
                (extended & (WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE)) == 0 &&
                timer_id_ == 0 && !gap_tracking_ && !mouse_tracked_ && !tooltip_active_;
        };
        if (!dock_style()) { return E_FAIL; }
        hide_to_taskbar();
        // Late pointer/timer and fullscreen/display events cannot resurrect it.
        pointer_changed(true);
        sync_timer();
        apply_state(true);
        SendMessageW(window_, WM_TIMER, 100 + timer_generation_, 0);
        SendMessageW(window_, kFullscreenMessage, 0, 0);
        SendMessageW(window_, kReconfigureMessage, 0, 0);
        SendMessageW(window_, kDesktopSwitchMessage, 0, 0);
        if (!minimized_style()) {
            log_.write("Smoke taskbar Hide failed: minimized state/styles did not survive background events.");
            return E_FAIL;
        }
        SendMessageW(window_, WM_SYSCOMMAND, SC_RESTORE, 0);
        const auto expected = configuration_.dock.auto_hide
            ? launcher::DockState::collapsed : launcher::DockState::expanded;
        if (!dock_style() || !verify_native_state(expected) || active_tab_ != original_tab) {
            log_.write("Smoke taskbar restore failed: the dock state/styles or selected tab changed.");
            return E_FAIL;
        }
        // Some Shell restoration paths call ShowWindow instead of SC_RESTORE.
        hide_to_taskbar();
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        if (!dock_style() || !verify_native_state(expected) || active_tab_ != original_tab) {
            log_.write("Smoke direct taskbar restore failed.");
            return E_FAIL;
        }
        log_.write("Smoke taskbar Hide/restore passed: manual hiding, background events, both restore paths, preserved selection.");
        return S_OK;
    }

    void configure_restart_arguments()
    {
        std::wstring arguments;
        if (files_.explicitly_named()) { arguments = L"--config " + win32::quote_command_argument(config_path_.wstring()); }
        if (test_ui_mode_) { arguments += arguments.empty() ? L"--test-ui" : L" --test-ui"; }
#if LAUNCHER_VERSION_TEST_HOOKS
        if (scenario_) {
            if (!arguments.empty()) { arguments += L" "; }
            arguments += L"--version-scenario " + win32::quote_command_argument(scenario_path_.wstring());
        }
#endif
        // Both menu Restart and external updaters use this same normalized
        // argument text. Temporary restart handles and one-shot modes stay out.
        restart_arguments_reply_.emplace(win32::restart_arguments_request, std::move(arguments));
    }

    void start_restart()
    {
        if (restart_worker_.joinable()) { return; }
        const auto executable = executable_path();
        const std::wstring arguments(restart_arguments_reply_->response());
        const auto completion = std::make_shared<RestartCompletion>();
        // BackgroundLifetime outlives Window, and Window joins this jthread
        // before that owner can close the shared event. No HWND crosses here.
        const HANDLE notification = background_.completion_event();
        if (!SetTimer(window_, kRestartCompletionTimer, 100, nullptr)) {
            throw std::runtime_error("Cannot monitor replacement Launcher startup.");
        }
        restart_completion_ = completion;
        try {
            restart_worker_ = std::jthread([completion, executable, arguments, notification](std::stop_token stop) {
                std::uintptr_t cancellation = 0;
                try {
                    completion->result = launcher::restart_launcher(executable, arguments, stop, &cancellation);
                } catch (const std::exception& error) {
                    completion->result = {false, ERROR_GEN_FAILURE, utf16(error.what())};
                }
                completion->cancel = reinterpret_cast<HANDLE>(cancellation);
                completion->ready.store(true, std::memory_order_release);
                if (!SetEvent(notification)) {
                    OutputDebugStringW(L"Launcher: restart completion event failed.\n");
                    TerminateProcess(GetCurrentProcess(), 0xE0000001U);
                    std::abort();
                }
            });
        } catch (...) {
            KillTimer(window_, kRestartCompletionTimer);
            restart_completion_.reset();
            throw;
        }
        log_.write("Restart requested: current dock remains active until replacement initialization succeeds.");
    }

    void finish_restart()
    {
        if (!restart_completion_ || !restart_completion_->ready.load(std::memory_order_acquire)) { return; }
        KillTimer(window_, kRestartCompletionTimer);
        if (restart_worker_.joinable()) { restart_worker_.join(); }
        const auto completion = std::move(restart_completion_);
        const auto result = std::move(completion->result);
        if (result.success) {
            completion->committed = true;
            log_.write("Restart ready: replacement initialized and waits for this process to exit.");
            SendMessageW(window_, WM_CLOSE, 0, 0);
            return;
        }
        log_.result("Restart failed; current dock retained", HRESULT_FROM_WIN32(result.error_code));
        log_.write("Restart failure: ", utf8(result.message));
        MessageBoxW(window_, result.message.c_str(), L"Cannot restart Launcher", MB_OK | MB_ICONERROR);
    }

    HRESULT copy_window_image(ComPtr<IWICBitmap>& image)
    {
        // Copy before the menu opens: it can expand the dock and dispatch timers.
        if (motion_active_ && motion_frame_) {
            return wic_factory_->CreateBitmapFromSourceRect(motion_frame_.Get(), motion_crop_.X, motion_crop_.Y,
                motion_crop_.Width, motion_crop_.Height, image.ReleaseAndGetAddressOf());
        }
        if (!window_bitmap_) { return E_UNEXPECTED; }
        return wic_factory_->CreateBitmapFromSource(window_bitmap_.Get(), WICBitmapCacheOnLoad, image.ReleaseAndGetAddressOf());
    }

    HRESULT verify_snapshot_copy()
    {
        ComPtr<IWICBitmap> snapshot;
        HRESULT result = copy_window_image(snapshot);
        if (FAILED(result)) { return result; }
        UINT width = 0;
        UINT height = 0;
        result = snapshot->GetSize(&width, &height);
        if (FAILED(result)) { return result; }
        RECT client{};
        if (!GetClientRect(window_, &client) || width != static_cast<UINT>(client.right) ||
                height != static_cast<UINT>(client.bottom) || !width || height > UINT_MAX / 4 / width) { return E_FAIL; }
        const UINT size = width * height * 4;
        std::vector<BYTE> copied(size);
        std::vector<BYTE> expected(size);
        result = snapshot->CopyPixels(nullptr, width * 4, size, copied.data());
        if (SUCCEEDED(result)) {
            result = motion_active_ && motion_frame_
                ? motion_frame_->CopyPixels(&motion_crop_, width * 4, size, expected.data())
                : window_bitmap_->CopyPixels(nullptr, width * 4, size, expected.data());
        }
        if (SUCCEEDED(result) && copied != expected) { result = E_FAIL; }
        log_.result("Smoke window snapshot pixels and dimensions", result);
        return result;
    }

    void save_window_snapshot(IWICBitmapSource* image, UINT dpi)
    {
        PWSTR pictures = nullptr;
        HRESULT result = SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &pictures);
        std::filesystem::path written;
        try {
            if (SUCCEEDED(result)) {
                const auto directory = std::filesystem::path(pictures) / L"Launcher";
                SYSTEMTIME time{};
                GetLocalTime(&time);
                result = launcher::save_snapshot_png(wic_factory_.Get(), image, directory, dpi, time, written, files_);
            }
        } catch (const std::exception& error) {
            log_.write("Window snapshot failed: ", error.what());
            result = E_FAIL;
        }
        CoTaskMemFree(pictures);
        if (FAILED(result)) {
            log_.result("Window snapshot failed", result);
            MessageBoxW(window_, L"The window snapshot could not be saved in Pictures\\Launcher. See launcher.log for details.",
                L"Cannot save Launcher snapshot", MB_OK | MB_ICONERROR);
        } else {
            log_.write("Window snapshot saved: ", utf8(written.wstring()));
        }
    }

    void capture_window_snapshot()
    {
        if (hidden_to_taskbar_ || restart_pending_ || !IsWindowVisible(window_)) { return; }
        ComPtr<IWICBitmap> snapshot;
        const HRESULT result = copy_window_image(snapshot);
        if (FAILED(result)) {
            log_.result("Window snapshot copy failed", result);
            return;
        }
        save_window_snapshot(snapshot.Get(), dpi_);
    }

    void decorate_context_menu(HMENU menu, std::initializer_list<std::pair<UINT, launcher::MenuIcon>> icons)
    {
        MENUINFO style{sizeof(style)};
        style.fMask = MIM_STYLE;
        style.dwStyle = MNS_CHECKORBMP;
        SetMenuInfo(menu, &style);
        for (const auto [command, icon] : icons) {
            try {
                MENUITEMINFOW row{sizeof(row)};
                row.fMask = MIIM_BITMAP;
                row.hbmpItem = menu_icons_.get(icon, dpi_);
                if (!SetMenuItemInfoW(menu, command, FALSE, &row)) {
                    log_.result("Set menu icon", HRESULT_FROM_WIN32(GetLastError()));
                }
            } catch (const std::exception& error) {
                log_.write("Menu icon ", std::to_string(command), " unavailable: ", error.what());
            }
        }
    }

    void context_menu(LPARAM position)
    {
        if (interaction_hold_) {
            return;
        }
        POINT screen{};
        if (position == -1) {
            const auto anchor = peek_.state() == launcher::DockState::collapsed
                ? D2D1::Point2F((tabs_[active_tab_].bounds.left + tabs_[active_tab_].bounds.right) / 2.0F,
                    (tabs_[active_tab_].bounds.top + tabs_[active_tab_].bounds.bottom) / 2.0F)
                : D2D1::Point2F(panel_bounds_.left + 6.0F, panel_bounds_.top + 6.0F);
            screen = POINT{pixels(anchor.x * fit_scale_, dpi_) - render_offset_px_.x,
                pixels(anchor.y * fit_scale_, dpi_) - render_offset_px_.y};
            ClientToScreen(window_, &screen);
        } else {
            screen = POINT{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
        }
        POINT client = screen;
        ScreenToClient(window_, &client);
        const float scale = 96.0F / static_cast<float>(dpi_) / fit_scale_;
        const D2D1_POINT_2F point = D2D1::Point2F(
            static_cast<float>(client.x + render_offset_px_.x) * scale,
            static_cast<float>(client.y + render_offset_px_.y) * scale);
        if (peek_.state() == launcher::DockState::expanded) {
            for (std::size_t index = 0; index < icons_.size(); ++index) {
                if (contains(icons_[index].bounds, point)) {
                    icon_context_menu(index, screen);
                    return;
                }
            }
        }
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr) {
            return;
        }
        const bool extended = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        ComPtr<IWICBitmap> snapshot;
        const UINT snapshot_dpi = dpi_;
        if (extended) {
            const HRESULT result = copy_window_image(snapshot);
            if (FAILED(result)) { log_.result("Window snapshot copy failed", result); }
            AppendMenuW(menu, MF_STRING | (snapshot ? 0U : MF_GRAYED), kSnapshotCommand, L"Save window snapshot");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
        AppendMenuW(menu, MF_STRING, kEditConfigurationCommand, L"Edit configuration");
        AppendMenuW(menu, MF_STRING, kConfigurationFolderCommand, L"Open configuration folder");
        AppendMenuW(menu, MF_STRING, kAdjustmentsCommand, L"Adjust sizes...");
        AppendMenuW(menu, MF_STRING, kAboutCommand, L"About…");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kHideCommand, L"Hide");
        AppendMenuW(menu, MF_STRING | (restart_worker_.joinable() ? MF_GRAYED : 0U), kRestartCommand, L"Restart");
        AppendMenuW(menu, MF_STRING, kQuitCommand, L"Quit");
        decorate_context_menu(menu, {
            {kEditConfigurationCommand, launcher::MenuIcon::edit},
            {kConfigurationFolderCommand, launcher::MenuIcon::folder},
            {kAboutCommand, launcher::MenuIcon::information},
            {kQuitCommand, launcher::MenuIcon::quit}});
        // Save screen coordinates before menu-open expands a collapsed dock.
        menu_active_ = true;
        clear_icon_tooltips();
        peek_.set_menu_open(true);
        apply_state();
        sync_timer();
        SetForegroundWindow(window_);
        const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screen.x, screen.y, window_, nullptr);
        DestroyMenu(menu);
        if (window_ == nullptr) {
            return;
        }
        if (command == kHideCommand) {
            hide_to_taskbar();
            return;
        }
        if (command == kQuitCommand) {
            log_.write("Quit selected from dock context menu.");
            DestroyWindow(window_);
        } else {
            if (command == kRestartCommand) {
                try {
                    start_restart();
                } catch (const std::exception& error) {
                    const auto text = utf16(error.what());
                    MessageBoxW(window_, text.c_str(), L"Cannot restart Launcher", MB_OK | MB_ICONERROR);
                }
                if (window_ == nullptr) { return; }
            }
            // Keep the dock revealed while Shell error dialogs run a modal loop.
            if (command == kEditConfigurationCommand || command == kConfigurationFolderCommand) {
                open_configuration(command == kConfigurationFolderCommand);
                if (window_ == nullptr) {
                    return;
                }
            } else if (command == kSnapshotCommand && snapshot) {
                save_window_snapshot(snapshot.Get(), snapshot_dpi);
                if (window_ == nullptr) { return; }
            } else if (command == kAdjustmentsCommand) {
                adjust_sizes();
                if (window_ == nullptr) {
                    return;
                }
            } else if (command == kAboutCommand) {
                const auto update_status = files_.portable() ? L"Last checked for updates: never" : launcher::last_automatic_check_text();
                launcher::AboutReviewStatus review_status(files_);
                if (launcher::show_about(window_, launcher::current_build_info(), [&review_status](HWND dialog) {
                        review_status.attach(dialog);
                    }, [this](HWND dialog) {
                        return launcher::check_for_updates(dialog, files_);
                    }, update_status, installation_status(files_.installation_scope())) == -1) {
                    log_.result("About dialog failed", HRESULT_FROM_WIN32(GetLastError()));
                }
                if (window_ == nullptr) { return; }
            }
            menu_active_ = false;
            peek_.set_menu_open(false);
            pointer_changed(pointer_inside());
            sync_icon_tooltips();
            PostMessageW(window_, WM_NULL, 0, 0);
        }
    }

    LRESULT message(UINT message, WPARAM word, LPARAM parameter)
    {
        if (restart_arguments_reply_) {
            if (const auto reply = restart_arguments_reply_->handle(window_, message, word, parameter)) { return *reply; }
        }
        if (message == WM_INITMENUPOPUP) {
            const auto popup = reinterpret_cast<HMENU>(word);
            if (active_version_menu_) {
                if (popup == active_version_menu_->root) { scenario_checkpoint("root-menu-opened"); }
                if (popup == active_version_menu_->child_menu && active_version_menu_->tracking_child) { scenario_checkpoint("child-menu-opened"); }
            }
            initialize_version_page(popup);
            for (auto frame = active_shell_frames_.rbegin(); frame != active_shell_frames_.rend(); ++frame) {
                if (!*frame || (*frame)->menu != popup || (*frame)->initialized) { continue; }
                auto& lazy = **frame;
                lazy.initialized = true;
                DeleteMenu(lazy.menu, 0, MF_BYPOSITION);
                bool available = false;
                try {
                    // This apartment owns Shell extensions. Entering Windows
                    // commands explicitly is the only operation that binds them.
                    lazy.shell = std::make_unique<launcher::ShellContextMenu>(lazy.owner, lazy.target);
                    available = lazy.shell->available() && SUCCEEDED(lazy.shell->append(lazy.menu,
                        lazy.first_command, lazy.last_command, lazy.extended)) && GetMenuItemCount(lazy.menu) > 0;
                } catch (const std::exception& error) { log_.write("Windows commands unavailable: ", error.what()); }
                if (!available) {
                    while (GetMenuItemCount(lazy.menu) > 0 && DeleteMenu(lazy.menu, 0, MF_BYPOSITION)) {}
                    AppendMenuW(lazy.menu, MF_STRING | MF_GRAYED, 0, L"Windows commands unavailable");
                }
                scenario_checkpoint(lazy.first_command == kIconShellFirst ? "root-shell-opened" : "child-shell-opened");
                break;
            }
        }
        if (message == WM_MENUSELECT) { select_version_menu_row(reinterpret_cast<HMENU>(parameter), LOWORD(word), HIWORD(word)); }
        if (message == WM_MENURBUTTONUP && active_version_menu_) {
            POINT cursor{};
            if (GetCursorPos(&cursor)) {
                try { version_row_context_menu(reinterpret_cast<HMENU>(parameter), static_cast<UINT>(word), cursor); }
                catch (const std::exception& error) { log_.write("Version context menu unavailable: ", error.what()); }
            }
            return 0;
        }
        LRESULT shell_result = 0;
        for (auto frame = active_shell_frames_.rbegin(); frame != active_shell_frames_.rend(); ++frame) {
            if (*frame && (*frame)->shell && (*frame)->shell->handle_message(message, word, parameter, shell_result)) { return shell_result; }
        }
        switch (message) {
        case WM_HOTKEY:
            if (word == kSnapshotHotkey && snapshot_hotkey_registered_) { capture_window_snapshot(); }
            return 0;
        case WM_MOUSEACTIVATE:
            if (hidden_to_taskbar_) { return DefWindowProcW(window_, message, word, parameter); }
            return MA_NOACTIVATE;
        case WM_SYSCOMMAND:
            if ((word & 0xfff0U) == SC_MINIMIZE) {
                hide_to_taskbar();
                return 0;
            }
            if (hidden_to_taskbar_ && ((word & 0xfff0U) == SC_RESTORE || (word & 0xfff0U) == SC_MAXIMIZE)) {
                restore_from_taskbar();
                return 0;
            }
            return DefWindowProcW(window_, message, word, parameter);
        case WM_MOUSEMOVE:
            if (diagnostics_enabled_) { ++diagnostics_.mouse_moves; }
            if (drag_) {
                update_drag();
                return 0;
            }
            track_pointer();
            pointer_changed(pointer_inside());
            update_hover(POINT{GET_X_LPARAM(parameter), GET_Y_LPARAM(parameter)});
            return 0;
        case WM_MOUSELEAVE:
            if (diagnostics_enabled_) { ++diagnostics_.mouse_leaves; }
            mouse_tracked_ = false;
            pressed_icon_.reset();
            // This notification is authoritative for User32's tracking state.
            // Re-arming here after hide/show can immediately queue another leave.
            if (!pointer_inside() && tooltip_window_ != nullptr) {
                SendMessageW(tooltip_window_, TTM_POP, 0, 0);
            }
            pointer_changed(pointer_inside());
            sync_hover();
            return 0;
        case WM_LBUTTONDOWN: {
            pressed_icon_.reset();
            if (tooltip_window_ != nullptr) { SendMessageW(tooltip_window_, TTM_POP, 0, 0); }
            const POINT client{GET_X_LPARAM(parameter), GET_Y_LPARAM(parameter)};
            if (const auto tab = tab_at(client)) {
                select_tab(*tab);
                if (!smoke_mode_) {
                    track_pointer();
                    pointer_changed(pointer_inside());
                }
                return 0;
            }
            if (const auto icon = icon_at(client)) {
                pressed_icon_ = *icon;
                if (!smoke_mode_) { track_pointer(); }
                return 0;
            }
            begin_drag(client);
            return 0;
        }
        case WM_LBUTTONUP: {
            const auto pressed = std::exchange(pressed_icon_, std::nullopt);
            if (drag_) {
                update_drag();
                finish_drag(true);
            } else if (pressed && icon_at(POINT{GET_X_LPARAM(parameter), GET_Y_LPARAM(parameter)}) == pressed) {
                launch_icon(*pressed, {}, false, false, nullptr, nullptr, (word & MK_SHIFT) != 0);
            }
            if (!smoke_mode_) { adopt_background(); }
            return 0;
        }
        case kDesktopSwitchMessage:
            desktop_redraw_message_pending_ = false;
            recover_drawing();
            return 0;
        case WM_KEYDOWN:
            if (word == VK_ESCAPE && drag_) {
                finish_drag(false);
                return 0;
            }
            return DefWindowProcW(window_, message, word, parameter);
        case WM_CAPTURECHANGED:
            if (drag_ && reinterpret_cast<HWND>(parameter) != window_) {
                finish_drag(false);
            }
            return 0;
        case WM_CANCELMODE:
            clear_icon_tooltips();
            finish_drag(false);
            sync_icon_tooltips();
            return 0;
        case WM_NOTIFY: {
            const auto* notification = reinterpret_cast<const NMHDR*>(parameter);
            if (notification == nullptr || notification->hwndFrom != tooltip_window_) {
                return DefWindowProcW(window_, message, word, parameter);
            }
            if (notification->code == TTN_SHOW) {
                if (!tooltip_active_ || !icon_tooltips_permitted()) {
                    clear_icon_tooltips();
                    return 0;
                }
                tooltip_visible_notified_ = true;
                log_.write("Icon label tooltip shown.");
            } else if (notification->code == TTN_POP && tooltip_visible_notified_) {
                tooltip_visible_notified_ = false;
                log_.write("Icon label tooltip hidden.");
            }
            return 0;
        }
        case WM_TIMER:
            if (word == kMotionTimer) {
                if (!hidden_to_taskbar_) { advance_motion(GetTickCount64()); }
                return 0;
            }
            if (word == kRestartCompletionTimer) {
                finish_restart();
                return 0;
            }
            if (word == kUpdaterTimer) {
                if (!restart_pending_) {
                    updater_schedule_.check(window_, kUpdaterTimer, kUpdaterRetryChangedMessage, files_);
                }
                return 0;
            }
            if (hidden_to_taskbar_) { return 0; }
            if (word == kDiagnosticTimer) {
                report_diagnostics();
                return 0;
            }
            if (word == kGapPointerTimer) {
                if (!gap_tracking_) { return 0; }
                if (diagnostics_enabled_) { ++diagnostics_.timers; }
                POINT cursor{};
                if (GetCursorPos(&cursor) && WindowFromPoint(cursor) == window_) {
                    track_pointer();
                }
                pointer_changed(pointer_inside());
                return 0;
            }
            if (diagnostics_enabled_) { ++diagnostics_.timers; }
            if (word == timer_id_ && timer_id_ != 0 &&
                peek_.pending() == timer_pending_) {
                const auto expected = timer_pending_;
                cancel_timer();
                peek_.pointer_changed(pointer_inside());
                if (peek_.pending() == expected) {
                    peek_.timer_elapsed();
                }
                apply_state();
                sync_timer();
            }
            return 0;
        case kFullscreenMessage:
            check_fullscreen();
            return 0;
        case kUpdaterRetryChangedMessage:
            updater_schedule_.retry_changed(window_, kUpdaterTimer);
            return 0;
        case kReconfigureMessage:
            if (diagnostics_enabled_) { ++diagnostics_.reconfigurations; }
            reconfigure_message_pending_ = false;
            if (!display_restart_requested_ && !restart_worker_.joinable()) { refresh_display(); }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            if (diagnostics_enabled_) { ++diagnostics_.paints; }
            PAINTSTRUCT paint{};
            BeginPaint(window_, &paint);
            const HRESULT result = hidden_to_taskbar_ ? S_OK : render();
            EndPaint(window_, &paint);
            if (FAILED(result)) {
                if (diagnostics_enabled_) { ++diagnostics_.paint_failures; }
                log_.result("Paint failed", result);
                if (result == D2DERR_RECREATE_TARGET) {
                    InvalidateRect(window_, nullptr, FALSE);
                }
            }
            return 0;
        }
        case WM_SIZE:
            if (diagnostics_enabled_) { ++diagnostics_.sizes; }
            if (taskbar_transition_ || word == SIZE_MINIMIZED) { return 0; }
            if (hidden_to_taskbar_) {
                restore_from_taskbar();
                return 0;
            }
            // Resize invalidates the backbuffer. Paint a complete frame using
            // the current client rectangle, not potentially stale message data.
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            if (diagnostics_enabled_) { ++diagnostics_.dpi_changes; }
            clear_icon_tooltips();
            dpi_ = static_cast<UINT>(HIWORD(word));
            update_window_icons();
            if (!arranging_ && !reconfigure_message_pending_) {
                reconfigure_message_pending_ = true;
                PostMessageW(window_, kReconfigureMessage, 0, 0);
            }
            return 0;
        }
        case WM_DISPLAYCHANGE:
            // The baseline and configuration are startup inputs. Coalesce these
            // notifications, then use the normal restart after modal UI unwinds.
            if (!smoke_mode_ && !restart_worker_.joinable()) { display_restart_requested_ = true; }
            [[fallthrough]];
        case WM_SETTINGCHANGE:
            if (message == WM_SETTINGCHANGE && word != SPI_SETWORKAREA) {
                return DefWindowProcW(window_, message, word, parameter);
            }
            clear_icon_tooltips();
            if (!reconfigure_message_pending_) {
                reconfigure_message_pending_ = true;
                PostMessageW(window_, kReconfigureMessage, 0, 0);
            }
            return 0;
        case WM_CONTEXTMENU:
            if (hidden_to_taskbar_) { return DefWindowProcW(window_, message, word, parameter); }
            context_menu(parameter);
            return 0;
        case WM_CLOSE:
            KillTimer(window_, kRestartCompletionTimer);
            restart_worker_.request_stop();
            log_.write("Graceful close requested.");
            clear_icon_tooltips();
            if (adjustment_dialog_ != nullptr) {
                EndDialog(adjustment_dialog_, IDCANCEL);
            }
            finish_drag(false);
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            filesystem_notification_.reset();
            if (snapshot_hotkey_registered_) {
                UnregisterHotKey(window_, kSnapshotHotkey);
                snapshot_hotkey_registered_ = false;
            }
            cancel_motion();
            restart_worker_.request_stop();
            KillTimer(window_, kRestartCompletionTimer);
            updater_schedule_.stop();
            KillTimer(window_, kUpdaterTimer);
            background_.stop_admission();
            cancel_version_intents();
            versions_->deactivate();
            shown_version_tab_.reset();
            launch_completions_.clear();
            drag_.reset();
            if (GetCapture() == window_) {
                ReleaseCapture();
            }
            report_diagnostics(true);
            cancel_timer();
            cancel_gap_tracking();
            cancel_pointer_tracking();
            remove_hooks();
            destroy_icon_tooltips();
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY: {
            const HWND old_window = window_;
            window_ = nullptr;
            SetWindowLongPtrW(old_window, GWLP_USERDATA, 0);
            return DefWindowProcW(old_window, message, word, parameter);
        }
        default:
            return DefWindowProcW(window_, message, word, parameter);
        }
    }

    static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM word, LPARAM parameter)
    {
        Window* self;
        if (message == WM_NCCREATE) {
            const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(parameter);
            self = static_cast<Window*>(creation->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        if (!self) {
            return DefWindowProcW(window, message, word, parameter);
        }
        try {
            return self->message(message, word, parameter);
        } catch (const std::exception& error) {
            self->log_.write("Window event failed:");
            self->log_.write(error.what());
        } catch (...) {
            self->log_.write("Window event failed with an unexpected exception.");
        }
        // Never allow a C++ exception to cross the Win32 callback boundary.
        PostMessageW(window, WM_CLOSE, 0, 0);
        return message == WM_NCCREATE ? FALSE : 0;
    }

    HINSTANCE instance_{};
    HWND window_{};
    HICON large_window_icon_{};
    HICON small_window_icon_{};
    launcher::Configuration configuration_;
    launcher::Layout layout_;
    std::size_t active_tab_ = 0;
    launcher::PeekController peek_;
    std::uint64_t launch_reveal_until_ = 0;
    std::filesystem::path config_path_;
    std::filesystem::path config_directory_;
    launcher::FileLookup files_;
    launcher::AutomaticUpdateSchedule updater_schedule_;
    std::filesystem::path runtime_path_;
    std::wstring pending_runtime_error_;
    Log& log_;
    launcher::NativeVersionDiscoveryExecutor& background_;
    std::shared_ptr<FilesystemDiagnostics> filesystem_diagnostics_;
    std::unique_ptr<launcher::AppNotification> filesystem_notification_;
    std::map<launcher::VersionSearchKey, launcher::PerSearchNoticeState> filesystem_notice_states_;
#if LAUNCHER_VERSION_TEST_HOOKS
    std::shared_ptr<launcher::VersionScenarios> scenario_;
    std::filesystem::path scenario_path_;
#endif
    launcher::RuntimeAdjustments runtime_;
    launcher::ActiveVersions active_versions_;
    launcher::EffectiveSettings effective_;
    launcher::NativeDesktopIconSize desktop_icons_;
    std::wstring resolved_font_family_;
    bool typographic_font_family_ = false;
    bool automatic_optical_size_ = false;
    UINT dpi_ = 96;
    float width_dip_ = 0.0F;
    float height_dip_ = 0.0F;
    float parallel_span_dip_ = 0.0F;
    float panel_depth_dip_ = 0.0F;
    float tab_depth_dip_ = 0.0F;
    float tab_slope_dip_ = 8.0F;
    float tab_label_padding_dip_ = 12.0F;
    float drawn_icon_size_dip_ = 0.0F;
    float icon_gap_dip_ = 0.0F;
    float fit_scale_ = 1.0F;
    D2D1_RECT_F panel_bounds_{};
    POINT render_offset_px_{};
    Monitor monitor_;
    launcher::DockPlacement placement_;
    launcher::DockAnimation motion_;
    bool motion_active_ = false;
    ComPtr<IWICBitmap> motion_frame_;
    WICRect motion_crop_{};
    std::optional<launcher::DockState> applied_state_;
    bool smoke_mode_ = false;
    bool snapshot_hotkey_registered_ = false;
    bool restart_pending_ = false;
    std::shared_ptr<RestartCompletion> restart_completion_;
    std::jthread restart_worker_;
    std::optional<win32::WindowTextReply> restart_arguments_reply_;
    bool test_ui_mode_ = false;
    bool diagnostics_enabled_ = false;
    bool diagnostics_reported_ = false;
    bool diagnostics_final_reported_ = false;
    ULONGLONG diagnostics_started_ = 0;
    mutable Diagnostics diagnostics_;
    bool arranging_ = false;
    bool interaction_hold_ = false;
    bool menu_active_ = false;
    bool hidden_to_taskbar_ = false;
    bool taskbar_transition_ = false;
    std::vector<LazyShellMenu*> active_shell_frames_;
    VersionMenuSession* active_version_menu_ = nullptr;
    inline static thread_local Window* menu_key_target_ = nullptr;
    HWND tooltip_window_{};
    std::size_t tooltip_tool_count_ = 0;
    bool tooltip_active_ = false;
    bool tooltip_visible_notified_ = false;
    HWND adjustment_dialog_{};
    std::optional<DragState> drag_;
    std::optional<std::size_t> pressed_icon_;
    std::optional<std::size_t> hovered_icon_;
    std::optional<std::size_t> menu_icon_;
    std::optional<std::size_t> hovered_tab_;
    std::vector<std::shared_ptr<LaunchCompletion>> launch_completions_;
    bool launch_dialog_active_ = false;
    std::size_t smoke_launch_count_ = 0;
    std::size_t smoke_chirp_count_ = 0;
    std::optional<std::pair<std::size_t, std::size_t>> smoke_last_launch_;
    std::optional<launcher::LaunchAction> smoke_cached_launch_;
    std::wstring smoke_cached_launch_label_;
    std::wstring smoke_properties_target_;
    bool mouse_tracked_ = false;
    bool gap_tracking_ = false;
    bool fullscreen_message_pending_ = false;
    bool desktop_redraw_message_pending_ = false;
    bool reconfigure_message_pending_ = false;
    bool display_restart_requested_ = false;
    bool display_layout_failed_ = false;
    UINT_PTR timer_id_ = 0;
    UINT_PTR timer_generation_ = 0;
    launcher::Pending timer_pending_ = launcher::Pending::none;
    HWND initial_foreground_{};
    HWINEVENTHOOK foreground_hook_{};
    HWINEVENTHOOK desktop_hook_{};
    std::array<HWINEVENTHOOK, kFullscreenObjectEvents.size()> object_hooks_{};
    mutable HWND last_display_window_{};
    mutable SkippedOverlay logged_overlay_;
    mutable bool candidate_log_initialized_ = false;
    mutable HWND logged_candidate_{};
    mutable DWORD logged_candidate_process_ = 0;
    mutable bool logged_candidate_fullscreen_ = false;
    mutable bool logged_candidate_maximized_ = false;
    mutable bool logged_candidate_native_bounds_ = false;
    inline static Window* event_target_ = nullptr;
    std::vector<TabVisual> tabs_;
    std::vector<std::size_t> tab_draw_order_;
    std::vector<IconVisual> icons_;
    launcher::IconImages icon_images_;
    launcher::MenuIcons menu_icons_;
    std::map<ItemBinding, ItemPresentationRecord> item_presentations_;
    std::size_t active_presentations_ = 0;
    std::uint64_t admitted_presentation_bytes_ = 0;
    std::uint64_t presentation_visit_ = 0;
    ComPtr<IWICBitmapSource> fallback_item_image_;
    UINT fallback_item_size_ = 0;
    ComPtr<IWICBitmapSource> pending_overlay_image_;
    ComPtr<ID2D1Bitmap> pending_overlay_bitmap_;
    UINT pending_overlay_size_ = 0;
    unsigned pending_overlay_slats_ = 0;
    std::unique_ptr<launcher::VersionDiscoveryService> versions_;
    std::vector<launcher::VersionSearchKey> version_keys_;
    std::vector<std::pair<launcher::VersionSearchKey, std::wstring>> reported_adobe_targets_;
    std::map<launcher::VersionSearchKey, launcher::VersionDiscoveryState> version_presentations_;
    std::map<launcher::VersionSearchKey, std::shared_ptr<const launcher::VersionDiscoveryAttempt>> logged_version_attempts_;
    std::optional<std::size_t> shown_version_tab_;
    bool version_gesture_dispatch_ = false;
    bool version_adopting_ = false;
    bool version_view_rebuilding_ = false;
    std::vector<PendingVersionLaunch> pending_version_launches_;
    ComPtr<ID2D1Factory> d2d_factory_;
    ComPtr<IDWriteFactory> write_factory_;
    ComPtr<IWICImagingFactory> wic_factory_;
    ComPtr<IDWriteTextFormat> text_format_;
    ComPtr<ID2D1RenderTarget> drawing_target_;
    ComPtr<IWICBitmap> preview_bitmap_;
    ComPtr<IWICBitmap> window_bitmap_;
    LayeredFrame layered_frame_;
    std::vector<SurfaceCache> surfaces_;
    std::uint64_t surface_builds_ = 0;
    double surface_build_elapsed_ms_ = 0.0;
    ComPtr<ID2D1SolidColorBrush> outline_;
    ComPtr<ID2D1SolidColorBrush> text_brush_;
};

struct Arguments final {
    bool no_arguments = false;
    bool notification_activated = false;
    bool smoke_test = false;
    bool stop = false;
    bool test_ui = false;
    bool color_demo = false;
    bool adobe_cc_list = false;
    std::filesystem::path adobe_cc_output;
    launcher::RestartHandles restart;
    launcher::UpdateStartupHandles update_startup;
    std::filesystem::path config;
    std::filesystem::path scenario;
    std::filesystem::path preview;
    std::size_t preview_page = 0;
    std::optional<launcher::Edge> preview_edge;
};

Arguments parse_arguments(bool& suppress_dialog)
{
    int count = 0;
    LPWSTR* values = CommandLineToArgvW(GetCommandLineW(), &count);
    if (values == nullptr) {
        throw std::runtime_error("Cannot read the command line.");
    }
    // Discover bounded runs before validating any option. A malformed argument
    // must not leave an unattended export or smoke test waiting on a dialog.
    for (int index = 1; index < count; ++index) {
        const std::wstring_view value(values[index]);
        if (value == L"--smoke-test" || value == L"--stop" || value == L"--render-preview" || value == L"--notification-activated" || value == L"--adobe-cc-list-output" ||
                value == L"--update-ready" || value == L"--update-error"
#if LAUNCHER_VERSION_TEST_HOOKS
                || value == L"--version-scenario"
#endif
                ) {
            suppress_dialog = true;
        }
    }
    Arguments arguments;
    arguments.no_arguments = count == 1;
    bool configuration_supplied = false;
    bool embedding = false;
    try {
        for (int index = 1; index < count; ++index) {
            const std::wstring_view value(values[index]);
            if (value == L"--notification-activated") {
                arguments.notification_activated = true;
            } else if (value == L"-Embedding" || value == L"/Embedding" || value == L"-embedding" || value == L"/embedding") {
                embedding = true;
            } else if (value == L"--smoke-test") {
                arguments.smoke_test = true;
            } else if (value == L"--stop") {
                arguments.stop = true;
            } else if (value == L"--test-ui") {
                arguments.test_ui = true;
            } else if (value == L"--color-demo") {
                arguments.color_demo = true;
            } else if (value == L"--adobe-cc-list") {
                arguments.adobe_cc_list = true;
            } else if (value == L"--adobe-cc-list-output" && index + 1 < count) {
                arguments.adobe_cc_list = true;
                arguments.adobe_cc_output = values[++index];
                if (arguments.adobe_cc_output.empty()) { throw std::runtime_error("AdobeCC output filename must not be empty."); }
                suppress_dialog = true;
#if LAUNCHER_VERSION_TEST_HOOKS
            } else if (value == L"--version-scenario" && index + 1 < count) {
                if (!arguments.scenario.empty()) { throw std::runtime_error("Supply only one version scenario."); }
                arguments.scenario = values[++index];
#endif
            } else if (value == L"--render-preview" && index + 1 < count) {
                arguments.preview = values[++index];
            } else if (value == L"--preview-edge" && index + 1 < count) {
                const std::wstring_view edge(values[++index]);
                if (edge == L"top") { arguments.preview_edge = launcher::Edge::top; }
                else if (edge == L"right") { arguments.preview_edge = launcher::Edge::right; }
                else if (edge == L"bottom") { arguments.preview_edge = launcher::Edge::bottom; }
                else if (edge == L"left") { arguments.preview_edge = launcher::Edge::left; }
                else { throw std::runtime_error("Preview edge must be top, right, bottom, or left."); }
            } else if (value == L"--preview-page" && index + 1 < count) {
                const std::wstring_view page(values[++index]);
                if (page.empty() || !std::all_of(page.begin(), page.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
                    { throw std::runtime_error("Preview page must be a zero-based integer."); }
                arguments.preview_page = std::stoull(std::wstring(page));
            } else if ((value == L"--restart-wait" || value == L"--restart-ready" ||
                        value == L"--restart-cancel" || value == L"--restart-error" ||
                        value == L"--update-ready" || value == L"--update-error") && index + 1 < count) {
                auto& destination = value == L"--restart-wait" ? arguments.restart.parent :
                    value == L"--restart-ready" ? arguments.restart.ready :
                    value == L"--restart-cancel" ? arguments.restart.cancel :
                    value == L"--restart-error" ? arguments.restart.error :
                    value == L"--update-ready" ? arguments.update_startup.ready : arguments.update_startup.error;
                const std::wstring_view handle(values[++index]);
                if (destination != 0 || handle.empty() ||
                        !std::all_of(handle.begin(), handle.end(),
                            [](wchar_t character) { return character >= L'0' && character <= L'9'; })) {
                    throw std::runtime_error("Invalid startup synchronization handle.");
                }
                const auto numeric = std::stoull(std::wstring(handle));
                if (numeric == 0 || numeric > std::numeric_limits<std::uintptr_t>::max()) {
                    throw std::runtime_error("Invalid startup synchronization handle.");
                }
                destination = static_cast<std::uintptr_t>(numeric);
            } else if (value == L"--config" && index + 1 < count) {
                if (configuration_supplied) {
                    throw std::runtime_error("Supply only one configuration path.");
                }
                arguments.config = values[++index];
                configuration_supplied = true;
            } else if ((value.empty() || value.front() != L'-') && !configuration_supplied) {
                arguments.config = values[index];
                configuration_supplied = true;
            } else {
                throw std::runtime_error("Unknown or incomplete command line option: " + utf8(value));
            }
        }
    } catch (...) {
        LocalFree(values);
        throw;
    }
    LocalFree(values);
    if (configuration_supplied && arguments.config.empty()) { throw std::runtime_error("Configuration filename must not be empty."); }
    const bool restart = arguments.restart.parent || arguments.restart.ready || arguments.restart.cancel || arguments.restart.error;
    const bool monitor_startup = arguments.update_startup.ready || arguments.update_startup.error;
    if (monitor_startup && (!arguments.update_startup.ready || !arguments.update_startup.error)) {
        throw std::runtime_error("Update startup requires both synchronization handles.");
    }
    if (monitor_startup && (embedding || restart || arguments.notification_activated || arguments.stop ||
            arguments.smoke_test || arguments.color_demo || arguments.adobe_cc_list || !arguments.preview.empty())) {
        throw std::runtime_error("Update startup requires a normal Launcher launch.");
    }
    if (arguments.adobe_cc_list && (configuration_supplied || arguments.color_demo || arguments.notification_activated ||
            embedding || restart || arguments.stop || arguments.smoke_test || arguments.test_ui || !arguments.scenario.empty() ||
            !arguments.preview.empty() || arguments.preview_edge || arguments.preview_page != 0)) {
        throw std::runtime_error("AdobeCC listing cannot be combined with normal Launcher options.");
    }
    if (arguments.color_demo && (configuration_supplied || arguments.stop || restart)) {
        throw std::runtime_error("--color-demo cannot be combined with a configuration path, stop or restart handoff.");
    }
    if (arguments.notification_activated) {
        if (configuration_supplied || arguments.color_demo || restart || arguments.stop || arguments.smoke_test || arguments.test_ui ||
                !arguments.scenario.empty() || !arguments.preview.empty() || arguments.preview_edge || arguments.preview_page != 0) {
            throw std::runtime_error("Notification activation cannot be combined with normal Launcher options.");
        }
        return arguments;
    }
    if (embedding) { throw std::runtime_error("COM embedding requires notification activation."); }
    if (restart && (!arguments.restart.parent || !arguments.restart.ready || !arguments.restart.cancel || !arguments.restart.error)) {
        throw std::runtime_error("Restart requires all synchronization handles.");
    }
    if (restart && (arguments.stop || arguments.smoke_test || !arguments.preview.empty())) {
        throw std::runtime_error("Restart handoff cannot be combined with stop or smoke-test.");
    }
    if (arguments.preview_edge && arguments.preview.empty() && !arguments.smoke_test) {
        throw std::runtime_error("--preview-edge requires --render-preview or --smoke-test.");
    }
#if LAUNCHER_VERSION_TEST_HOOKS
    if (!arguments.scenario.empty() && (!configuration_supplied || !arguments.config.is_absolute() ||
            !arguments.scenario.is_absolute() || arguments.stop || arguments.smoke_test || !arguments.preview.empty())) {
        throw std::runtime_error("Version scenarios require explicit absolute scenario/configuration paths and cannot use stop, smoke, or preview.");
    }
    if (arguments.scenario.empty()) { throw std::runtime_error("The instrumented build requires --version-scenario and an explicit fixture configuration."); }
#endif
    return arguments;
}

std::filesystem::path write_color_error_report(const launcher::UnknownColorError& error,
    const std::filesystem::path& configuration_path, const launcher::FileLookup& files) {
    const auto directory = user_data_directory();
    std::filesystem::create_directories(directory);
    const auto report = directory / (L"available-colors-" + std::to_wstring(GetCurrentProcessId()) + L".txt");
    std::ostringstream output;
    output << "\xEF\xBB\xBF" << error.what() << "\nSettings: " << utf8(configuration_path.wstring()) << "\n\nLoaded color definitions:\n";
    for (const auto& source : error.sources) { output << "  " << source << '\n'; }
    output << '\n' << launcher::color_library_report(error.name, error.sources);
    files.write_text(report, output.view());

    std::array<wchar_t, 32768> system{};
    const auto length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    if (length == 0 || length >= system.size()) { throw std::runtime_error("Cannot locate Notepad."); }
    const auto editor = std::filesystem::path(system.data()) / L"notepad.exe";
    const std::wstring arguments = L"\"" + report.wstring() + L"\"";
    ComApartment com;
    SHELLEXECUTEINFOW operation{};
    operation.cbSize = sizeof(operation);
    operation.fMask = SEE_MASK_FLAG_NO_UI;
    operation.lpVerb = L"open";
    operation.lpFile = editor.c_str();
    operation.lpParameters = arguments.c_str();
    operation.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&operation)) {
        throw std::runtime_error("Notepad could not open " + utf8(report.wstring()) + ".");
    }
    return report;
}

// This owner is constructed before logging/COM/UI and destroyed after their
// explicit cleanup. A provider may still be blocked in native I/O at that point;
// do not run CRT/static teardown concurrently with such a C++ worker.
class BackgroundLifetime final {
public:
    launcher::NativeVersionDiscoveryExecutor& executor()
    {
        if (!executor_) { executor_ = std::make_unique<launcher::NativeVersionDiscoveryExecutor>(); }
        return *executor_;
    }
    int result(int code) noexcept { exit_code_ = static_cast<unsigned int>(code); return code; }
    ~BackgroundLifetime() noexcept
    {
        if (executor_) { executor_->finish_process_if_busy(exit_code_); }
    }
private:
    std::unique_ptr<launcher::NativeVersionDiscoveryExecutor> executor_;
    unsigned int exit_code_ = 1;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    BackgroundLifetime background;
    std::optional<Log> startup_log;
#if LAUNCHER_VERSION_TEST_HOOKS
    std::shared_ptr<launcher::VersionScenarios> scenario;
    struct ScenarioCleanup final {
        std::shared_ptr<launcher::VersionScenarios>& scenario;
        std::optional<Log>& log;
        ~ScenarioCleanup() noexcept {
            log.reset();
            if (scenario) { try { scenario->checkpoint("owners-cleaned"); } catch (...) {} }
        }
    } scenario_cleanup{scenario, startup_log};
#endif
    bool suppress_dialog = false;
#if LAUNCHER_VERSION_TEST_HOOKS
    // This build exists for unattended, isolated fixtures. Even malformed
    // command lines must not leave an unowned modal failure box behind.
    suppress_dialog = true;
#endif
    bool staged_restart = false;
    bool show_color_report = false;
    bool loading_settings = false;
    bool loading_dock_settings = false;
    bool log_configured = false;
    launcher::RestartHandles restart;
    launcher::UpdateStartupHandles update_startup;
    std::filesystem::path configuration_path;
    std::optional<launcher::FileLookup> file_context;
    try {
        const Arguments arguments = parse_arguments(suppress_dialog);
        if (arguments.notification_activated) { return background.result(activate_filesystem_notification()); }
#if LAUNCHER_VERSION_TEST_HOOKS
        if (!arguments.scenario.empty()) { scenario = launcher::VersionScenarios::load(arguments.scenario, arguments.config); }
#endif
        restart = arguments.restart;
        update_startup = arguments.update_startup;
        staged_restart = restart.parent != 0;
        suppress_dialog = suppress_dialog || arguments.smoke_test || arguments.stop || !arguments.preview.empty() || restart.parent != 0 || !arguments.scenario.empty();
        show_color_report = !arguments.smoke_test && !arguments.stop && arguments.preview.empty() && !arguments.test_ui && arguments.scenario.empty();
        ComApartment com;
        if (arguments.adobe_cc_list) {
            const auto report = launcher::adobe_cc_tab_report(launcher::scan_adobe_cc());
            const auto path = write_adobe_cc_report(report, arguments.adobe_cc_output);
            if (!arguments.adobe_cc_output.empty()) { return background.result(0); }
            const auto result = launcher::launch_prepared_action(filesystem_log_action(path));
            if (!result.success) { throw std::runtime_error("Cannot open the AdobeCC report in Notepad."); }
            return background.result(0);
        }
        const std::filesystem::path executable = executable_path();
        std::optional<launcher::DefaultDrawer> default_drawer;
        const bool default_handoff = arguments.config.empty() && !arguments.test_ui && arguments.scenario.empty() &&
            (staged_restart || update_startup.ready != 0);
        if (arguments.no_arguments || default_handoff) {
            default_drawer.emplace(executable.parent_path());
            if (!staged_restart && !default_drawer->start(arguments.no_arguments)) { return background.result(0); }
        }
        // Buffer startup messages until the configuration selects their destination.
        // A staged replacement keeps buffering until the old process has exited.
        Log& log = startup_log.emplace(user_data_directory() / L"launcher.log");
#if LAUNCHER_VERSION_TEST_HOOKS
        if (scenario) { log.configure(launcher::LoggingOptions{scenario->output_directory() / L"launcher.log"}, scenario->output_directory()); }
#endif
        if (arguments.stop) {
            log.resume();
            return background.result(stop_owned_instances(executable, log));
        }
        if (FAILED(com.result())) {
            log.result("COM initialization failed", com.result());
            throw std::runtime_error("Windows COM initialization failed.");
        }
        const auto user_directory = user_data_directory();
        loading_settings = true;
        const launcher::FileLookup initial_files(executable, user_directory, {}, !arguments.config.empty());
        const auto config = arguments.color_demo ? launcher::resolve_color_demo_configuration(
            executable.parent_path() / L"examples" / L"demo_many_colors.toml", initial_files) :
            launcher::resolve_configuration_path(arguments.config, initial_files, [&](const auto& destination) {
                if (suppress_dialog) { throw launcher::ConfigError("Cannot find Launcher.toml; supply --config or create '" + utf8(destination.wstring()) + "'"); }
                const auto text = L"Launcher.toml was not found.\n\nIf you use a configuration with another name, exit and start Launcher with that filename.\n\nCreate a starter configuration here?\n\n" + destination.wstring();
                return MessageBoxW(nullptr, text.c_str(), L"Launcher — Configuration needed", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
            });
        if (config.empty()) { return background.result(0); }
        file_context.emplace(executable, user_directory, config.parent_path(), !arguments.config.empty() || arguments.color_demo);
        const auto& lookup = *file_context;
        configuration_path = config;
        const auto& build = launcher::current_build_info();
        log.write("Starting Launcher ", utf8(LAUNCHER_VERSION_WSTRING), "; config=", utf8(config.wstring()), ".");
        log.write("File lookup: mode=", lookup.portable() ? "portable" : "installed", "; configuration=", lookup.explicitly_named() ? "explicit" : "default", ".");
        log.write("Build: ", utf8(build.configuration), "; built=", utf8(build.built), "; git=", utf8(build.git_version), "; commit=",
            utf8(build.commit), "; source=", utf8(build.source_state), "; instrumentation=", utf8(build.instrumentation), ".");
        auto files = launcher::load_configuration_files(config, lookup);
        loading_dock_settings = true;
#if LAUNCHER_VERSION_TEST_HOOKS
        if (scenario) { files.configuration.logging.file = scenario->output_directory() / L"launcher.log"; }
#endif
        log.configure(files.configuration.logging, config.parent_path(), lookup);
        log_configured = true;
        if (restart.parent == 0) { log.resume(); }
        for (const auto& source : files.color_sources) { log.write("Color definitions: ", source, "."); }
        for (const auto& source : files.icon_sources) { log.write("Icon file: ", source, "."); }
        for (const auto& warning : files.icon_warnings) { log.write("Icon unavailable; generic fallback: ", warning); }
        if (arguments.preview_edge) { files.configuration.dock.edge = *arguments.preview_edge; }
        Window window(instance, std::move(files.configuration), config, lookup, log, background.executor()
#if LAUNCHER_VERSION_TEST_HOOKS
            , scenario, arguments.scenario
#endif
            );
        const HRESULT result = window.initialize(arguments.smoke_test || !arguments.preview.empty(), arguments.test_ui,
            restart.parent != 0, !arguments.smoke_test && arguments.preview.empty() && arguments.scenario.empty(),
            update_startup.ready != 0);
        loading_settings = false;
        if (FAILED(result)) {
            log.result("Window initialization failed", result);
            throw std::runtime_error("The drawing window could not be initialized.");
        }
        if (!arguments.preview.empty()) {
            return background.result(SUCCEEDED(window.render_preview(arguments.preview, arguments.preview_page)) ? 0 : 1);
        }
        if (arguments.smoke_test) {
            return background.result(SUCCEEDED(window.smoke_test()) ? 0 : 1);
        }
        if (restart.parent != 0) {
            if (default_drawer) { default_drawer->reserve_restart(); }
            const auto completed = launcher::complete_restart_handoff(std::exchange(restart, {}));
            if (!completed.success) { throw std::runtime_error(utf8(completed.message)); }
            if (default_drawer) { default_drawer->finish_restart(); }
            staged_restart = false;
            log.resume();
            log.write("Restart handoff complete: previous Launcher exited; replacement is ready.");
            window.reveal_after_restart();
        }
        window.enable_snapshot_hotkey();
        // Even a fullscreen-suppressed dock renders a first frame. Report
        // readiness from the main pump only, after all startup work returns.
        if (update_startup.ready && !PostThreadMessageW(GetCurrentThreadId(), kUpdateStartupReadyMessage, 0, 0)) {
            throw std::system_error(GetLastError(), std::system_category(), "Cannot queue update startup readiness");
        }
        const std::array waits{background.executor().completion_event(), default_drawer ? default_drawer->reveal_event() : nullptr};
        const DWORD wait_count = default_drawer ? 2 : 1;
        for (;;) {
            const DWORD ready = MsgWaitForMultipleObjectsEx(wait_count, waits.data(), INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (ready == WAIT_FAILED) { throw std::system_error(GetLastError(), std::system_category(), "Windows message wait failed"); }
            if (ready == WAIT_OBJECT_0) { window.collect_background(); }
            if (default_drawer && ready == WAIT_OBJECT_0 + 1) { window.reveal_from_launch(); }
            // Bound each dispatch batch so busy input cannot starve completed
            // searches. A manual-reset completion remains set during modal UI.
            MSG message{};
            for (unsigned int count = 0; count != 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
#if LAUNCHER_VERSION_TEST_HOOKS
                if (scenario) {
                    const auto command = scenario->command(message.message, message.wParam, message.lParam);
                    if (command) {
                        if (command->kind == launcher::VersionScenarioCommandKind::fatal_error) {
                            suppress_dialog = true;
                            throw std::runtime_error("Injected version scenario fatal error.");
                        }
                        window.scenario_control(*command);
                        window.adopt_background();
                        continue;
                    }
                }
#endif
                if (message.message == WM_QUIT) {
                    log.write("Launcher exited normally.");
                    return background.result(static_cast<int>(message.wParam));
                }
                if (update_startup.ready && !message.hwnd && message.message == kUpdateStartupReadyMessage) {
                    const auto completed = launcher::complete_update_startup(std::exchange(update_startup, {}));
                    if (!completed.success) { throw std::runtime_error(utf8(completed.message)); }
                    suppress_dialog = !arguments.scenario.empty();
                    window.show_pending_runtime_error();
                    continue;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
                window.adopt_background();
            }
        }
    } catch (const std::exception& error) {
        const bool monitored = update_startup.ready != 0;
        if (monitored) {
            const auto* settings = loading_settings ? dynamic_cast<const launcher::ConfigError*>(&error) : nullptr;
            std::wstring_view settings_file;
            if (settings && !settings->source_file.empty()) { settings_file = settings->source_file.native(); }
            else if (settings && loading_dock_settings) { settings_file = configuration_path.native(); }
            launcher::report_update_startup_failure(std::exchange(update_startup, {}),
                settings != nullptr, utf16(error.what()), settings_file,
                settings ? settings->source_line : 0, settings ? settings->source_column : 0);
        }
        if (startup_log) {
            // An ordinary launch logs early failures to the default log.
            // A handoff reports them to its supervisor instead of mixing them
            // into the default log when it has no configured destination yet.
            if (!staged_restart && (!monitored || log_configured)) { startup_log->resume(); }
            startup_log->write("Fatal: ", error.what());
        }
        if (restart.parent != 0) {
            launcher::report_restart_failure(std::exchange(restart, {}), utf16(error.what()));
        }
        std::string message = error.what();
        if (const auto* color_error = dynamic_cast<const launcher::UnknownColorError*>(&error); color_error && show_color_report && !monitored) {
            try {
                const auto report = write_color_error_report(*color_error, configuration_path, file_context.value_or(launcher::FileLookup({}, {})));
                if (startup_log) { startup_log->write("Available colors report: ", utf8(report.wstring())); }
                return background.result(1);
            } catch (const std::exception& report_error) {
                message += "\nCould not open the color report: " + std::string(report_error.what());
                if (startup_log) { startup_log->write(message); }
            }
        }
        if (!suppress_dialog) {
            const std::wstring text = utf16(message);
            MessageBoxW(nullptr, text.c_str(), L"Launcher", MB_OK | MB_ICONERROR);
        }
        return background.result(1);
    }
}
