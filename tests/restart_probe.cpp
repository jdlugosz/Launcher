#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/restart.hpp"
#include "win32/window_text_reply.hpp"

#include <tlhelp32.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

struct Handle final {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) { CloseHandle(value); } }
};

struct RetainedCancellation final {
    HANDLE value = nullptr;
    ~RetainedCancellation() { abandon(); }

    void abandon() noexcept {
        if (value) { SetEvent(value); CloseHandle(value); value = nullptr; }
    }

    void commit() noexcept {
        if (value) { CloseHandle(value); value = nullptr; }
    }
};

DWORD owned_child(const std::filesystem::path& executable) {
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.value == INVALID_HANDLE_VALUE) { return 0; }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD found = 0;
    if (!Process32FirstW(snapshot.value, &entry)) { return 0; }
    do {
        if (entry.th32ParentProcessID != GetCurrentProcessId()) { continue; }
        Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)};
        if (!process.value) { continue; }
        std::wstring path(32768, L'\0');
        DWORD length = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(process.value, 0, path.data(), &length)) { continue; }
        path.resize(length);
        const auto expected = executable.wstring();
        if (CompareStringOrdinal(path.c_str(), static_cast<int>(path.size()), expected.c_str(),
                static_cast<int>(expected.size()), TRUE) != CSTR_EQUAL) { continue; }
        if (found != 0) { return 0; }
        found = entry.th32ProcessID;
    } while (Process32NextW(snapshot.value, &entry));
    return found;
}

void number(std::ofstream& output, std::uint32_t value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

bool receipt(const std::filesystem::path& path, const launcher::LaunchResult& result, DWORD child) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) { return false; }
    number(output, 0x52535450); // RSTP: our dedicated restart parent probe.
    number(output, GetCurrentProcessId());
    number(output, child);
    number(output, result.success ? 1U : 0U);
    number(output, result.error_code);
    number(output, static_cast<std::uint32_t>(result.message.size()));
    output.write(reinterpret_cast<const char*>(result.message.data()),
        static_cast<std::streamsize>(result.message.size() * sizeof(wchar_t)));
    output.close();
    return static_cast<bool>(output);
}

int run(int count, wchar_t** arguments, HINSTANCE instance) {
    if (count != 7) { return 2; }
    Handle release{OpenEventW(SYNCHRONIZE, FALSE, arguments[4])};
    Handle reported{OpenEventW(EVENT_MODIFY_STATE, FALSE, arguments[5])};
    Handle abort_request{OpenEventW(SYNCHRONIZE, FALSE, arguments[6])};
    if (!release.value || !reported.value || !abort_request.value) { return 3; }
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"LauncherRestartParentProbe";
    if (!RegisterClassW(&window_class)) { return 4; }
    const HWND window = CreateWindowW(window_class.lpszClassName, L"Launcher restart parent probe",
        WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    if (!window) { return 4; }
    const std::filesystem::path executable(arguments[1]);
    std::uintptr_t cancellation = 0;
    const auto normal_arguments = L"--config " + win32::quote_command_argument(arguments[2]) + L" --test-ui";
    const auto result = launcher::restart_launcher(executable, normal_arguments, {}, &cancellation);
    RetainedCancellation retained{reinterpret_cast<HANDLE>(cancellation)};
    if (result.success && !retained.value) { DestroyWindow(window); return 7; }
    if (!receipt(arguments[3], result, result.success ? owned_child(executable) : 0) ||
            !SetEvent(reported.value)) {
        DestroyWindow(window);
        return 5;
    }
    // The controller decides when this parent exits. Pump after the restart
    // result so the failure test can prove that the parent remains usable.
    bool aborted = false;
    const std::array events{abort_request.value, release.value};
    for (;;) {
        const DWORD event_count = aborted ? 1U : 2U;
        const auto handles = aborted ? events.data() + 1 : events.data();
        const DWORD wait = MsgWaitForMultipleObjects(event_count, handles, FALSE, 20000, QS_ALLINPUT);
        if (!aborted && wait == WAIT_OBJECT_0) {
            retained.abandon();
            aborted = true;
            continue;
        }
        if (wait == WAIT_OBJECT_0 + (aborted ? 0U : 1U)) { retained.commit(); break; }
        if (wait != WAIT_OBJECT_0 + event_count) { DestroyWindow(window); return 6; }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    DestroyWindow(window);
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) { return 2; }
    const int result = run(count, arguments, instance);
    LocalFree(arguments);
    return result;
}
