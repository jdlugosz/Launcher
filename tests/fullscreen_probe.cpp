#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t window_class[] = L"Launcher.FullscreenProbe.Window";
constexpr UINT_PTR close_timer = 1;
constexpr COLORREF overlay_color_key = RGB(250, 250, 250);

struct Options {
    std::wstring display = L"primary";
    UINT duration_ms = 2500;
    bool other_display = false;
    bool cover = false;
    bool color_key_overlay = false;
    std::filesystem::path inspect_output;
};

Options parse_options() {
    int count = 0;
    auto** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr) {
        throw std::runtime_error("Cannot parse arguments");
    }
    Options options;
    try {
        for (int index = 1; index < count; ++index) {
            const std::wstring_view argument(arguments[index]);
            if (argument == L"--display" && index + 1 < count) {
                options.display = arguments[++index];
            } else if (argument == L"--duration-ms" && index + 1 < count) {
                const auto* value = arguments[++index];
                wchar_t* end = nullptr;
                const unsigned long duration = std::wcstoul(value, &end, 10);
                if (end == value || *end != L'\0' || duration < 1000 || duration > 10000) {
                    throw std::runtime_error("Duration must be 1000..10000 milliseconds");
                }
                options.duration_ms = static_cast<UINT>(duration);
            } else if (argument == L"--focus-other-display") {
                options.other_display = true;
            } else if (argument == L"--cover") {
                options.cover = true;
            } else if (argument == L"--color-key-overlay") {
                options.color_key_overlay = true;
            } else if (argument == L"--inspect-windows" && index + 1 < count) {
                options.inspect_output = arguments[++index];
                if (options.inspect_output.empty()) {
                    throw std::runtime_error("Inspection needs an output path");
                }
            } else {
                throw std::runtime_error("Unknown or incomplete argument");
            }
        }
        if (options.other_display && options.cover) {
            throw std::runtime_error("The two focus modes are mutually exclusive");
        }
        if (options.color_key_overlay && (options.other_display || options.cover)) {
            throw std::runtime_error("The overlay mode cannot be combined with focus modes");
        }
        if (!options.inspect_output.empty() &&
            (options.other_display || options.cover || options.color_key_overlay)) {
            throw std::runtime_error("Inspection cannot be combined with probe window modes");
        }
    } catch (...) {
        LocalFree(arguments);
        throw;
    }
    LocalFree(arguments);
    return options;
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int length = static_cast<int>(value.size());
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), length, nullptr, 0, nullptr, nullptr);
    if (needed == 0) {
        return "unavailable";
    }
    std::string converted(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
        converted.data(), needed, nullptr, nullptr);
    return converted;
}

struct ProcessName {
    std::wstring basename;
    DWORD error = 0;
};

ProcessName process_name(DWORD process) {
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    if (handle == nullptr) {
        return {L"unavailable", GetLastError()};
    }
    std::array<wchar_t, 32768> path{};
    DWORD length = static_cast<DWORD>(path.size());
    const BOOL queried = QueryFullProcessImageNameW(handle, 0, path.data(), &length);
    const DWORD error = queried ? 0 : GetLastError();
    CloseHandle(handle);
    if (!queried) {
        return {L"unavailable", error};
    }
    const std::wstring_view full_path(path.data(), length);
    const auto separator = full_path.find_last_of(L"\\/");
    return {std::wstring(separator == std::wstring_view::npos
        ? full_path : full_path.substr(separator + 1)), 0};
}

void write_rect(std::ostream& output, const RECT& rectangle) {
    output << rectangle.left << ',' << rectangle.top << ','
           << rectangle.right << ',' << rectangle.bottom;
}

struct Inspection {
    std::ofstream output;
    HWND foreground = GetForegroundWindow();
    std::size_t rank = 0;
    std::size_t recorded = 0;
    bool foreground_recorded = false;
    bool failed = false;
};

BOOL CALLBACK inspect_window(HWND window, LPARAM parameter) {
    auto& inspection = *reinterpret_cast<Inspection*>(parameter);
    const auto rank = inspection.rank++;
    try {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        std::array<wchar_t, 256> name{};
        GetClassNameW(window, name.data(), static_cast<int>(name.size()));
        const bool foreground = window == inspection.foreground;
        const bool taskbar = std::wcscmp(name.data(), L"Shell_TrayWnd") == 0 ||
            std::wcscmp(name.data(), L"Shell_SecondaryTrayWnd") == 0;
        const bool visible = IsWindowVisible(window) != FALSE;
        if (!foreground && !taskbar && !visible) {
            return TRUE;
        }
        const auto executable = process_name(process);
        const bool ruler = visible && _wcsicmp(executable.basename.c_str(), L"aruler.exe") == 0;
        if (!foreground && !taskbar && !ruler) {
            return TRUE;
        }
        const auto style = static_cast<DWORD_PTR>(GetWindowLongPtrW(window, GWL_STYLE));
        const auto extended_style = static_cast<DWORD_PTR>(GetWindowLongPtrW(window, GWL_EXSTYLE));
        RECT native{};
        const BOOL native_ok = GetWindowRect(window, &native);
        const DWORD native_error = native_ok ? 0 : GetLastError();
        RECT frame{};
        const HRESULT frame_result = DwmGetWindowAttribute(window,
            DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame));
        DWORD cloaked = 0;
        const HRESULT cloaked_result = DwmGetWindowAttribute(window,
            DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        COLORREF color_key = 0;
        BYTE alpha = 0;
        DWORD layer_flags = 0;
        const BOOL layer_ok = GetLayeredWindowAttributes(window, &color_key, &alpha, &layer_flags);
        const DWORD layer_error = layer_ok ? 0 : GetLastError();
        RECT region_bounds{};
        int region_type = ERROR;
        int region_box_type = ERROR;
        const HRGN region = CreateRectRgn(0, 0, 0, 0);
        if (region != nullptr) {
            region_type = GetWindowRgn(window, region);
            if (region_type != ERROR) {
                region_box_type = GetRgnBox(region, &region_bounds);
            }
            DeleteObject(region);
        }
        std::ostringstream record;
        record << "z-rank=" << rank << "; foreground=" << foreground
            << "; taskbar=" << taskbar << "; ruler=" << ruler
            << "; hwnd=0x" << std::hex << reinterpret_cast<std::uintptr_t>(window)
            << "; owner=0x" << reinterpret_cast<std::uintptr_t>(GetWindow(window, GW_OWNER))
            << "; style=0x" << style << "; exstyle=0x" << extended_style << std::dec
            << "; pid=" << process << "; executable=" << utf8(executable.basename)
            << "; executable-error=" << executable.error << "; class=" << utf8(name.data())
            << "; visible=" << visible << "; iconic=" << (IsIconic(window) != FALSE)
            << "; zoomed=" << (IsZoomed(window) != FALSE)
            << "; native-ok=" << native_ok << "; native-error=" << native_error << "; native=";
        write_rect(record, native);
        record << "; dwm-hr=0x" << std::hex << static_cast<unsigned long>(frame_result)
            << std::dec << "; dwm=";
        write_rect(record, frame);
        record << "; cloaked=" << cloaked << "; cloaked-hr=0x" << std::hex
            << static_cast<unsigned long>(cloaked_result) << std::dec
            << "; layered=" << ((extended_style & WS_EX_LAYERED) != 0)
            << "; transparent=" << ((extended_style & WS_EX_TRANSPARENT) != 0)
            << "; noactivate=" << ((extended_style & WS_EX_NOACTIVATE) != 0)
            << "; layer-ok=" << layer_ok << "; layer-error=" << layer_error
            << "; layer-flags=0x" << std::hex << layer_flags
            << "; color-key=0x" << color_key << std::dec
            << "; alpha=" << static_cast<unsigned>(alpha)
            << "; region-type=" << region_type << "; region-box-type=" << region_box_type
            << "; region=";
        write_rect(record, region_bounds);
        inspection.output << record.str() << '\n';
        ++inspection.recorded;
        inspection.foreground_recorded |= foreground;
    } catch (...) {
        inspection.failed = true;
        return FALSE;
    }
    return TRUE;
}

int inspect_windows(const std::filesystem::path& output) {
    try {
        Inspection inspection;
        inspection.output.open(output, std::ios::binary | std::ios::trunc);
        if (!inspection.output) {
            return 1;
        }
        inspection.output << "Native read-only window inspection; no titles or full executable paths.\n"
            << "foreground-hwnd=0x" << std::hex
            << reinterpret_cast<std::uintptr_t>(inspection.foreground) << std::dec << '\n';
        const BOOL enumerated = EnumWindows(inspect_window, reinterpret_cast<LPARAM>(&inspection));
        inspection.output << "summary: visited=" << inspection.rank
            << "; recorded=" << inspection.recorded
            << "; foreground-recorded=" << inspection.foreground_recorded
            << "; enum-ok=" << enumerated << "; failed=" << inspection.failed << '\n';
        inspection.output.flush();
        return enumerated && !inspection.failed && inspection.output ? 0 : 1;
    } catch (...) {
        return 1;
    }
}

struct Display {
    std::wstring name;
    RECT bounds{};
    RECT work{};
    bool primary = false;
};

struct Inventory {
    std::vector<Display> displays;
    bool failed = false;
};

BOOL CALLBACK enumerate_display(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto& inventory = *reinterpret_cast<Inventory*>(parameter);
    MONITORINFOEXW information{};
    information.cbSize = sizeof(information);
    if (!GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&information))) {
        inventory.failed = true;
        return FALSE;
    }
    try {
        inventory.displays.push_back({information.szDevice, information.rcMonitor,
            information.rcWork, (information.dwFlags & MONITORINFOF_PRIMARY) != 0});
    } catch (...) {
        inventory.failed = true;
        return FALSE;
    }
    return TRUE;
}

struct Context {
    HWND fullscreen{};
    HWND ordinary{};
    bool closing = false;
    bool quit_posted = false;
    bool color_key_overlay = false;
};

void close_windows(Context& context) noexcept {
    if (context.closing) {
        return;
    }
    context.closing = true;
    if (context.ordinary != nullptr && IsWindow(context.ordinary)) {
        DestroyWindow(context.ordinary);
    }
    context.ordinary = nullptr;
    if (context.fullscreen != nullptr && IsWindow(context.fullscreen)) {
        KillTimer(context.fullscreen, close_timer);
        DestroyWindow(context.fullscreen);
    }
    context.fullscreen = nullptr;
    if (!context.quit_posted) {
        context.quit_posted = true;
        PostQuitMessage(0);
    }
}

LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM word, LPARAM parameter) {
    auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        context = static_cast<Context*>(reinterpret_cast<CREATESTRUCTW*>(parameter)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
    }
    if (context != nullptr) {
        if (message == WM_CLOSE || (message == WM_KEYDOWN && word == VK_ESCAPE) ||
            (message == WM_TIMER && word == close_timer)) {
            close_windows(*context);
            return 0;
        }
        if (message == WM_ERASEBKGND) {
            return 1;
        }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            const bool fullscreen = (GetWindowLongPtrW(window, GWL_STYLE) & WS_POPUP) != 0;
            const bool overlay = fullscreen && context->color_key_overlay;
            RECT label = client;
            if (overlay) {
                SetDCBrushColor(dc, overlay_color_key);
                FillRect(dc, &client, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
                const LONG center_x = client.right / 2;
                const LONG center_y = client.bottom / 2;
                label = {center_x - 120, center_y - 25, center_x + 120, center_y + 25};
                FillRect(dc, &label, GetSysColorBrush(COLOR_HIGHLIGHT));
            } else {
                FillRect(dc, &client,
                    fullscreen ? reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))
                               : GetSysColorBrush(COLOR_WINDOW));
            }
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, fullscreen ? RGB(255, 255, 255) : RGB(0, 0, 0));
            auto previous_font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
            DrawTextW(dc, overlay ? L"Color-key overlay probe - Esc closes"
                : fullscreen ? L"Launcher full-screen probe - Esc closes"
                             : L"Ordinary foreground probe - Esc closes",
                -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, previous_font);
            EndPaint(window, &paint);
            return 0;
        }
    }
    return DefWindowProcW(window, message, word, parameter);
}

bool focus_own_window(HWND window) noexcept {
    ShowWindow(window, SW_SHOW);
    SetActiveWindow(window);
    SetForegroundWindow(window);
    SetFocus(window);
    return GetForegroundWindow() == window;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    Options options;
    try {
        options = parse_options();
    } catch (...) {
        return 2;
    }
    if (!options.inspect_output.empty()) {
        return inspect_windows(options.inspect_output);
    }
    Inventory inventory;
    if (!EnumDisplayMonitors(nullptr, nullptr, enumerate_display,
            reinterpret_cast<LPARAM>(&inventory)) || inventory.failed) {
        return 1;
    }
    const auto selected = std::find_if(inventory.displays.begin(), inventory.displays.end(),
        [&](const Display& display) {
            return _wcsicmp(options.display.c_str(), L"primary") == 0
                ? display.primary : _wcsicmp(display.name.c_str(), options.display.c_str()) == 0;
        });
    if (selected == inventory.displays.end()) {
        return 2;
    }
    auto ordinary_display = selected;
    if (options.other_display) {
        ordinary_display = std::find_if(inventory.displays.begin(), inventory.displays.end(),
            [&](const Display& display) { return display.name != selected->name; });
        if (ordinary_display == inventory.displays.end()) {
            return 2;
        }
    }
    WNDCLASSEXW registration{};
    registration.cbSize = sizeof(registration);
    registration.lpfnWndProc = dispatch;
    registration.hInstance = instance;
    registration.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    registration.lpszClassName = window_class;
    if (RegisterClassExW(&registration) == 0) {
        return 1;
    }
    Context context;
    context.color_key_overlay = options.color_key_overlay;
    const auto& bounds = selected->bounds;
    const DWORD extended_style = WS_EX_APPWINDOW |
        (options.color_key_overlay ? WS_EX_LAYERED | WS_EX_TOPMOST : 0);
    context.fullscreen = CreateWindowExW(extended_style, window_class,
        L"Launcher full-screen probe", WS_POPUP, bounds.left, bounds.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, &context);
    int result = 0;
    if (context.fullscreen == nullptr) {
        result = 1;
    } else if (options.color_key_overlay &&
        !SetLayeredWindowAttributes(context.fullscreen, overlay_color_key, 255, LWA_COLORKEY)) {
        result = 1;
    } else if (!focus_own_window(context.fullscreen)) {
        result = 3;
    } else if (options.other_display || options.cover) {
        const auto& work = ordinary_display->work;
        const int width = static_cast<int>(std::min<LONG>(480, work.right - work.left));
        const int height = static_cast<int>(std::min<LONG>(180, work.bottom - work.top));
        context.ordinary = CreateWindowExW(WS_EX_APPWINDOW, window_class,
            L"Launcher ordinary foreground probe", WS_OVERLAPPEDWINDOW,
            work.left + (work.right - work.left - width) / 2,
            work.top + (work.bottom - work.top - height) / 2, width, height,
            nullptr, nullptr, instance, &context);
        if (context.ordinary == nullptr) {
            result = 1;
        } else if (!focus_own_window(context.ordinary)) {
            result = 3;
        }
    }
    if (result == 0 && SetTimer(context.fullscreen, close_timer, options.duration_ms, nullptr) == 0) {
        result = 1;
    }
    if (result == 0) {
        MSG message{};
        int status = 0;
        while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (status == -1) {
            result = 1;
        }
    }
    close_windows(context);
    UnregisterClassW(window_class, instance);
    return result;
}
