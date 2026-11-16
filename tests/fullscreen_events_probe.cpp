#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t probe_class[] = L"Launcher.FullscreenEventsProbe.Window";
constexpr wchar_t launcher_class[] = L"Launcher.DrawingPrototype.Window";
constexpr COLORREF transparent_color = RGB(250, 250, 250);

void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void pump_for(DWORD milliseconds) {
    const auto deadline = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        const auto now = GetTickCount64();
        if (now >= deadline) { return; }
        MsgWaitForMultipleObjectsEx(0, nullptr,
            static_cast<DWORD>(std::min<ULONGLONG>(20, deadline - now)), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (true);
}

template<class Predicate>
void await(Predicate predicate, const char* message) {
    const auto deadline = GetTickCount64() + 5000;
    while (!predicate()) {
        require(GetTickCount64() < deadline, message);
        pump_for(10);
    }
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::uint64_t counter(std::string_view line, std::string_view key) {
    const auto field = std::string("; ") + std::string(key) + '=';
    const auto start = line.find(field);
    require(start != std::string_view::npos, "A required diagnostic counter is missing");
    return std::stoull(std::string(line.substr(start + field.size())));
}

std::string diagnostic(const std::string& log, bool final) {
    const std::string marker = final ? "; final=yes;" : "; final=no;";
    const auto position = log.rfind(marker);
    require(position != std::string::npos, "A required diagnostic report is missing");
    const auto start = log.rfind('\n', position);
    const auto end = log.find('\n', position);
    return log.substr(start == std::string::npos ? 0 : start + 1,
        end == std::string::npos ? end : end - (start == std::string::npos ? 0 : start + 1));
}

struct Display final {
    RECT bounds{};
    RECT work{};
    bool primary = false;
};

BOOL CALLBACK collect_display(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    MONITORINFO information{sizeof(information)};
    if (!GetMonitorInfoW(monitor, &information)) { return FALSE; }
    auto& displays = *reinterpret_cast<std::vector<Display>*>(parameter);
    try {
        displays.push_back({information.rcMonitor, information.rcWork,
            (information.dwFlags & MONITORINFOF_PRIMARY) != 0});
    } catch (...) { return FALSE; }
    return TRUE;
}

struct WindowLookup final {
    DWORD process{};
    HWND window{};
};

BOOL CALLBACK find_launcher(HWND window, LPARAM parameter) {
    auto& lookup = *reinterpret_cast<WindowLookup*>(parameter);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    std::array<wchar_t, 128> name{};
    GetClassNameW(window, name.data(), static_cast<int>(name.size()));
    if (process == lookup.process && std::wstring_view(name.data()) == launcher_class) {
        lookup.window = window;
        return FALSE;
    }
    return TRUE;
}

LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM word, LPARAM parameter) {
    if (message == WM_ERASEBKGND) { return 1; }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT bounds{};
        GetClientRect(window, &bounds);
        SetDCBrushColor(dc, transparent_color);
        FillRect(dc, &bounds, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, word, parameter);
}

struct Fixture final {
    PROCESS_INFORMATION child{};
    HWND launcher{};
    HWND original_foreground = GetForegroundWindow();
    std::vector<HWND> windows;
    std::filesystem::path directory;
    std::filesystem::path log;
    std::ofstream report;

    ~Fixture() {
        for (auto iterator = windows.rbegin(); iterator != windows.rend(); ++iterator) {
            if (IsWindow(*iterator)) { DestroyWindow(*iterator); }
        }
        if (child.hProcess) {
            if (launcher && IsWindow(launcher)) { PostMessageW(launcher, WM_CLOSE, 0, 0); }
            if (WaitForSingleObject(child.hProcess, 5000) != WAIT_OBJECT_0) {
                // Only the exact child created by this fixture is ever terminated.
                TerminateProcess(child.hProcess, 101);
                WaitForSingleObject(child.hProcess, 5000);
            }
            CloseHandle(child.hProcess);
        }
        if (child.hThread) { CloseHandle(child.hThread); }
        if (IsWindow(original_foreground)) { SetForegroundWindow(original_foreground); }
    }

    HWND make_window(RECT bounds, DWORD extended_style = WS_EX_APPWINDOW | WS_EX_TOPMOST,
        DWORD style = WS_POPUP, HWND parent = nullptr) {
        const auto window = CreateWindowExW(extended_style, probe_class, L"Fullscreen event verification",
            style, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
            parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(window != nullptr, "Cannot create a fixture window");
        windows.push_back(window);
        return window;
    }

    std::size_t log_position() const { return read_text(log).size(); }

    void await_candidate(std::size_t position, std::string_view classification, bool visible) {
        const auto process = "; pid=" + std::to_string(GetCurrentProcessId()) + ';';
        const auto classified = "; classification=" + std::string(classification) + ';';
        await([&] {
            const auto text = read_text(log);
            const auto candidate = text.find("Fullscreen candidate: class=Launcher.FullscreenEventsProbe.Window;", position);
            if (candidate == std::string::npos) { return false; }
            const auto end = text.find('\n', candidate);
            const auto line = std::string_view(text).substr(candidate, end - candidate);
            return line.find(process) != std::string_view::npos && line.find(classified) != std::string_view::npos &&
                (IsWindowVisible(launcher) != FALSE) == visible;
        }, "Fullscreen candidate or actual dock suppression did not reach the expected state");
    }

    void pass(std::string_view message) {
        report << "PASS: " << message << '\n';
        report.flush();
    }
};

RECT ordinary_bounds(const RECT& work) {
    return {work.left + 80, work.top + 80, work.left + 440, work.top + 260};
}

void set_bounds(HWND window, const RECT& bounds) {
    require(SetWindowPos(window, HWND_TOPMOST, bounds.left, bounds.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE,
        "Cannot change fixture window geometry");
    DwmFlush();
    NotifyWinEvent(EVENT_OBJECT_LOCATIONCHANGE, window, OBJID_WINDOW, CHILDID_SELF);
}

bool focus(HWND window) {
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    return GetForegroundWindow() == window;
}

int run(const std::filesystem::path& executable, const std::filesystem::path& output) {
    Fixture fixture;
    fixture.directory = std::filesystem::absolute(output) /
        (L"fullscreen-events-" + std::to_wstring(GetCurrentProcessId()) + L'-' + std::to_wstring(GetTickCount64()));
    require(std::filesystem::create_directories(fixture.directory), "Cannot create unique evidence directory");
    fixture.report.open(fixture.directory / L"results.txt", std::ios::binary);
    std::wcout << L"Fullscreen event verification evidence: " << fixture.directory << std::endl;
    fixture.log = fixture.directory / L"launcher.log";
    const auto config = fixture.directory / L"fixture.toml";
    {
        std::ofstream configuration(config, std::ios::binary);
        configuration << "schema_version = 1\n[dock]\nedge = 'top'\ndisplay = 'primary'\n"
            "auto_hide = true\nsuppress_fullscreen = true\n[logging]\nfile = 'launcher.log'\n"
            "[[tabs]]\nname = 'Fullscreen event verification'\n"
            "items = [{label = 'Fixture', icon = {stock = 'folder'}}]\n";
        require(static_cast<bool>(configuration), "Cannot write isolated fixture configuration");
    }
    std::vector<Display> displays;
    require(EnumDisplayMonitors(nullptr, nullptr, collect_display, reinterpret_cast<LPARAM>(&displays)) != FALSE,
        "Cannot enumerate displays");
    const auto primary = std::find_if(displays.begin(), displays.end(), [](const Display& display) { return display.primary; });
    require(primary != displays.end(), "No primary monitor was found");
    WNDCLASSEXW registration{};
    registration.cbSize = sizeof(registration);
    registration.hInstance = GetModuleHandleW(nullptr);
    registration.lpfnWndProc = dispatch;
    registration.lpszClassName = probe_class;
    require(RegisterClassExW(&registration) != 0, "Cannot register fixture window class");

    // An ordinary topmost fixture window makes the initial candidate independent
    // of whichever desktop application happened to be foreground before this run.
    const auto backdrop = fixture.make_window(ordinary_bounds(primary->work));
    ShowWindow(backdrop, SW_SHOWNOACTIVATE);
    const auto main = fixture.make_window(ordinary_bounds(primary->work));
    if (!focus(main)) {
        fixture.report << "Windows denied initial foreground activation; topmost-window checks continue without it.\n";
    }
    auto command = L'"' + executable.wstring() + L"\" --test-ui --config \"" + config.wstring() + L'"';
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
        nullptr, executable.parent_path().c_str(), &startup, &fixture.child) != FALSE,
        "Cannot start the isolated Launcher child");
    await([&] {
        WindowLookup lookup{fixture.child.dwProcessId};
        EnumWindows(find_launcher, reinterpret_cast<LPARAM>(&lookup));
        fixture.launcher = lookup.window;
        return fixture.launcher != nullptr && read_text(fixture.log).find("classification=ordinary;") != std::string::npos;
    }, "Isolated Launcher did not finish startup");
    fixture.await_candidate(0, "ordinary", true);
    pump_for(100);
    // Request the existing --test-ui one-shot report now; normal mode ignores
    // this diagnostic timer ID. There is no new production test message/API.
    DWORD_PTR ignored = 0;
    require(SendMessageTimeoutW(fixture.launcher, WM_TIMER, 50, 0, SMTO_ABORTIFHUNG, 3000, &ignored) != 0,
        "Cannot request the initial diagnostic report");
    const auto before = diagnostic(read_text(fixture.log), false);
    fixture.pass("isolated child configuration/log and ordinary selected-monitor candidate");

    constexpr unsigned burst = 512;
    const auto child = fixture.make_window({0, 0, 60, 40}, 0, WS_CHILD | WS_VISIBLE, main);
    for (unsigned index = 0; index < burst; ++index) {
        NotifyWinEvent(EVENT_OBJECT_FOCUS, main, OBJID_CLIENT, CHILDID_SELF);
        NotifyWinEvent(EVENT_OBJECT_SELECTION, main, OBJID_CLIENT, CHILDID_SELF);
        NotifyWinEvent(EVENT_OBJECT_STATECHANGE, main, OBJID_CLIENT, CHILDID_SELF);
        NotifyWinEvent(EVENT_OBJECT_LOCATIONCHANGE, main, OBJID_CLIENT, CHILDID_SELF);
        NotifyWinEvent(EVENT_OBJECT_LOCATIONCHANGE, child, OBJID_WINDOW, CHILDID_SELF);
    }
    for (unsigned index = 0; index < 64; ++index) {
        NotifyWinEvent(EVENT_OBJECT_HIDE, child, OBJID_WINDOW, CHILDID_SELF);
        NotifyWinEvent(EVENT_OBJECT_DESTROY, child, OBJID_WINDOW, CHILDID_SELF);
        NotifyWinEvent(EVENT_OBJECT_REORDER, child, OBJID_WINDOW, CHILDID_SELF);
    }
    pump_for(300);

    auto position = fixture.log_position();
    set_bounds(main, primary->bounds);
    fixture.await_candidate(position, "fullscreen", false);
    fixture.pass("candidate geometry entering fullscreen suppresses the actual dock");

    const auto overlay = fixture.make_window(primary->bounds,
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_APPWINDOW);
    require(SetLayeredWindowAttributes(overlay, transparent_color, 255, LWA_COLORKEY) != FALSE,
        "Cannot create transparent overlay attributes");
    position = fixture.log_position();
    ShowWindow(overlay, SW_SHOWNOACTIVATE);
    NotifyWinEvent(EVENT_OBJECT_REORDER, GetDesktopWindow(), OBJID_CLIENT, CHILDID_SELF);
    await([&] {
        return read_text(fixture.log).find("reason=color-key", position) != std::string::npos;
    }, "Transparent overlay was not inspected and skipped");
    require(!IsWindowVisible(fixture.launcher), "Transparent overlay incorrectly resumed the dock");
    fixture.pass("transparent topmost overlay preserves the opaque fullscreen window beneath it");

    const auto cover = fixture.make_window(ordinary_bounds(primary->work));
    position = fixture.log_position();
    ShowWindow(cover, SW_SHOWNOACTIVATE);
    fixture.await_candidate(position, "ordinary", true);
    fixture.pass("ordinary topmost cover resumes the dock without requiring foreground change");
    position = fixture.log_position();
    ShowWindow(cover, SW_HIDE);
    fixture.await_candidate(position, "fullscreen", false);
    fixture.pass("hiding the covering candidate exposes fullscreen and suppresses again");

    position = fixture.log_position();
    ShowWindow(main, SW_MINIMIZE);
    await([&] { return IsWindowVisible(fixture.launcher) != FALSE; }, "Minimizing fullscreen did not resume the dock");
    fixture.pass("minimizing the tracked fullscreen candidate resumes the dock");
    position = fixture.log_position();
    ShowWindow(main, SW_RESTORE);
    fixture.await_candidate(position, "fullscreen", false);
    fixture.pass("restoring the fullscreen candidate suppresses the dock");

    const auto other = std::find_if(displays.begin(), displays.end(), [](const Display& display) { return !display.primary; });
    if (other != displays.end()) {
        const auto other_window = fixture.make_window(ordinary_bounds(other->work));
        const bool foreground_granted = focus(other_window);
        if (!foreground_granted) {
            NotifyWinEvent(EVENT_SYSTEM_FOREGROUND, other_window, OBJID_WINDOW, CHILDID_SELF);
        }
        pump_for(150);
        require(!IsWindowVisible(fixture.launcher), "Foreground on another monitor resumed the selected-monitor dock");
        fixture.pass(foreground_granted
            ? "foreground on another monitor preserves selected-monitor fullscreen suppression"
            : "synthetic foreground event for another monitor preserves selected-monitor fullscreen suppression");
        if (!foreground_granted) {
            fixture.report << "NOT EXERCISED: actual cross-monitor foreground activation (denied by Windows).\n";
        }
        position = fixture.log_position();
        set_bounds(main, ordinary_bounds(other->work));
        await([&] { return IsWindowVisible(fixture.launcher) != FALSE; },
            "Moving the fullscreen candidate off the selected monitor did not resume the dock");
        fixture.pass("moving the tracked candidate off the selected monitor resumes the dock");
        position = fixture.log_position();
        set_bounds(main, primary->bounds);
        fixture.await_candidate(position, "fullscreen", false);
        ShowWindow(other_window, SW_HIDE);
    } else {
        fixture.report << "NOT EXERCISED: second-monitor foreground and candidate-move filtering (one attached monitor).\n";
    }

    position = fixture.log_position();
    ShowWindow(main, SW_HIDE);
    await([&] { return IsWindowVisible(fixture.launcher) != FALSE; }, "Hiding fullscreen did not resume the dock");
    fixture.pass("hiding the tracked fullscreen candidate resumes the dock");
    position = fixture.log_position();
    ShowWindow(main, SW_SHOWNOACTIVATE);
    fixture.await_candidate(position, "fullscreen", false);
    fixture.pass("showing fullscreen suppresses the dock");
    position = fixture.log_position();
    DestroyWindow(main);
    await([&] { return IsWindowVisible(fixture.launcher) != FALSE; }, "Destroying fullscreen did not resume the dock");
    fixture.pass("destroyed candidate is handled after its HWND becomes unavailable");

    PostMessageW(fixture.launcher, WM_CLOSE, 0, 0);
    await([&] { return WaitForSingleObject(fixture.child.hProcess, 0) == WAIT_OBJECT_0; }, "Launcher did not close cleanly");
    DWORD exit = 1;
    require(GetExitCodeProcess(fixture.child.hProcess, &exit) && exit == 0, "Launcher child failed during shutdown");
    const auto after = diagnostic(read_text(fixture.log), true);
    std::uint64_t subscribed = 0;
    for (const auto key : {"foreground", "location", "show", "hide", "destroy", "reorder"}) {
        subscribed += counter(after, key);
    }
    require(counter(after, "winevents") == subscribed,
        "Unsubscribed focus/selection/state events reached the fullscreen callback");
    require(counter(after, "location") - counter(before, "location") >= 2 * burst,
        "Synthetic object/child location events were not delivered; routing evidence is inconclusive");
    const auto filtered = counter(after, "fullscreen-filtered") - counter(before, "fullscreen-filtered");
    const auto coalesced = counter(after, "fullscreen-coalesced") - counter(before, "fullscreen-coalesced");
    require(filtered + coalesced >= 2 * burst + 3 * 64,
        "Synthetic client/child events were not rejected or coalesced before fullscreen evaluation");
    const auto scans = counter(after, "fullscreen-scans") - counter(before, "fullscreen-scans");
    require(scans < 100, "Irrelevant event burst caused excessive fullscreen enumeration");
    fixture.pass("focus/selection/state subscriptions excluded; child/client object burst causes no enumeration storm");
    fixture.report << "Synthetic excluded events=" << 3 * burst << "; synthetic child/client location events=" << 2 * burst
        << "; observed fullscreen scans after initial report=" << scans << ".\n"
        << "Initial: " << before << "\nFinal: " << after << '\n';
    fixture.report.flush();
    std::wcout << L"Fullscreen event verification passed. Evidence: " << fixture.directory << L'\n';
    return 0;
}

} // namespace

int wmain(int count, wchar_t** arguments) {
    if (count != 3) {
        std::cerr << "Usage: LauncherFullscreenEventsProbe.exe <Launcher.exe> <evidence-directory>\n";
        return 2;
    }
    try {
        return run(std::filesystem::absolute(arguments[1]), arguments[2]);
    } catch (const std::exception& error) {
        std::cerr << "Fullscreen event verification failed: " << error.what() << '\n';
        return 1;
    }
}
