#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/restart.hpp"
#include "win32/window_text_reply.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {
namespace {

struct Handle final {
    HANDLE value = nullptr;
    void close() noexcept { if (value && value != INVALID_HANDLE_VALUE) { CloseHandle(value); } value = nullptr; }
    ~Handle() { close(); }
};

constexpr DWORD kRestartTimeoutMs = 15000;
constexpr DWORD kDiagnosticBytes = 4096;

template<std::size_t Count>
struct InheritedHandles final {
    std::array<Handle, Count> owned{};
    bool valid = true;

    explicit InheritedHandles(const std::array<std::uintptr_t, Count>& values) noexcept {
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (values[index] == 0 || static_cast<std::intptr_t>(values[index]) < 0) { valid = false; continue; }
            bool duplicate = false;
            for (std::size_t previous = 0; previous < index; ++previous) {
                duplicate = duplicate || values[previous] == values[index];
            }
            DWORD flags = 0;
            const auto handle = reinterpret_cast<HANDLE>(values[index]);
            if (duplicate || !GetHandleInformation(handle, &flags)) { valid = false; continue; }
            owned[index].value = handle;
        }
    }
};

void write_diagnostic(HANDLE pipe, std::wstring_view message) noexcept {
    if (!pipe) { return; }
    const auto bytes = static_cast<DWORD>(std::min(message.size(),
        static_cast<std::size_t>(kDiagnosticBytes / sizeof(wchar_t))) * sizeof(wchar_t));
    DWORD written = 0;
    WriteFile(pipe, message.data(), bytes, &written, nullptr);
}

void write_update_diagnostic(HANDLE pipe, bool settings_error, std::wstring_view message,
    std::wstring_view settings_file = {}, std::uint32_t line = 0, std::uint32_t column = 0) noexcept {
    if (!pipe || GetFileType(pipe) != FILE_TYPE_PIPE) { return; }
    // A single bounded write fits in the inherited pipe even if the watchdog
    // waits for process exit before reading. Prefix the existing UTF-16 text
    // with the classification so no interpretation of error text is required.
    const std::wstring_view prefix = settings_error ? L"settings\n" : L"startup\n";
    std::array<wchar_t, kDiagnosticBytes / sizeof(wchar_t)> diagnostic;
    auto next = std::copy(prefix.begin(), prefix.end(), diagnostic.begin());
    constexpr std::wstring_view file_prefix = L"Settings file: ";
    constexpr std::wstring_view separator = L"\n\n";
    constexpr std::size_t minimum_message_room = 512;
    // Older monitors display the optional header as part of the diagnostic.
    // Never send a partial path or accept a line break inside it.
    if (settings_error && !settings_file.empty() &&
            settings_file.size() <= diagnostic.size() - prefix.size() - file_prefix.size() - separator.size() - minimum_message_room &&
            std::ranges::none_of(settings_file, [](wchar_t character) { return character < L' '; })) {
        next = std::copy(file_prefix.begin(), file_prefix.end(), next);
        next = std::copy(settings_file.begin(), settings_file.end(), next);
        next = std::copy(separator.begin(), separator.end(), next);
        if (line && column) {
            // Each uint32_t fits in ten decimal digits, with one separator.
            std::array<char, 21> location;
            const auto delimiter = std::to_chars(location.data(), location.data() + location.size(), line).ptr;
            *delimiter = ':';
            const auto end = std::to_chars(delimiter + 1, location.data() + location.size(), column).ptr;
            constexpr std::wstring_view location_prefix = L"Settings location: ";
            const auto room = static_cast<std::size_t>(diagnostic.end() - next);
            if (room >= location_prefix.size() + static_cast<std::size_t>(end - location.data()) + separator.size() + minimum_message_room) {
                next = std::copy(location_prefix.begin(), location_prefix.end(), next);
                next = std::copy(location.data(), end, next);
                next = std::copy(separator.begin(), separator.end(), next);
            }
        }
    }
    auto length = std::min(message.size(), static_cast<std::size_t>(diagnostic.end() - next));
    if (length != message.size() && message[length - 1] >= 0xd800 && message[length - 1] <= 0xdbff) { --length; }
    next = std::copy_n(message.begin(), length, next);
    write_diagnostic(pipe, std::wstring_view(diagnostic.data(), static_cast<std::size_t>(next - diagnostic.begin())));
}

std::wstring read_diagnostic(HANDLE pipe) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) { return {}; }
    std::wstring message(kDiagnosticBytes / sizeof(wchar_t), L'\0');
    DWORD read = 0;
    if (!ReadFile(pipe, message.data(), std::min(available, kDiagnosticBytes), &read, nullptr)) { return {}; }
    message.resize(read / sizeof(wchar_t));
    return message;
}

struct Attributes final {
    std::vector<std::byte> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
    ~Attributes() { if (value) { DeleteProcThreadAttributeList(value); } }
};

LaunchResult failure(DWORD error, std::wstring_view message) {
    return {false, error, std::wstring(message) + L"\nWindows error " + std::to_wstring(error) + L"."};
}

} // namespace

std::wstring restart_command_line(const std::filesystem::path& executable, std::wstring_view arguments, RestartHandles handles) {
    if (arguments.find(L'\0') != arguments.npos) { throw std::invalid_argument("Restart arguments cannot contain NUL."); }
    auto command = win32::quote_command_argument(executable.wstring()) + L" --restart-wait " + std::to_wstring(handles.parent) +
        L" --restart-ready " + std::to_wstring(handles.ready) +
        L" --restart-cancel " + std::to_wstring(handles.cancel) +
        L" --restart-error " + std::to_wstring(handles.error);
    if (!arguments.empty()) { command += L" "; command += arguments; }
    return command;
}

LaunchResult restart_launcher(const std::filesystem::path& executable, std::wstring_view arguments,
        std::stop_token stop, std::uintptr_t* cancellation_handle) {
    if (cancellation_handle) { *cancellation_handle = 0; }
    const auto executable_text = executable.wstring();
    if (!executable.is_absolute() || executable_text.find(L'\0') != std::wstring::npos || arguments.find(L'\0') != arguments.npos) {
        return failure(ERROR_INVALID_PARAMETER, L"Restart needs an absolute executable path and argument text without NUL characters.");
    }
    Handle parent{OpenProcess(SYNCHRONIZE, TRUE, GetCurrentProcessId())};
    if (!parent.value) { return failure(GetLastError(), L"Cannot prepare the restart handoff."); }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle ready{CreateEventW(&security, TRUE, FALSE, nullptr)};
    Handle cancel{CreateEventW(&security, TRUE, FALSE, nullptr)};
    Handle error_read;
    Handle error_write;
    if (!ready.value || !cancel.value ||
            !CreatePipe(&error_read.value, &error_write.value, &security, kDiagnosticBytes) ||
            !SetHandleInformation(error_read.value, HANDLE_FLAG_INHERIT, 0)) {
        return failure(GetLastError(), L"Cannot prepare restart synchronization.");
    }
    std::stop_callback cancellation(stop, [event = cancel.value] { SetEvent(event); });
    if (stop.stop_requested()) { return failure(ERROR_CANCELLED, L"Restart was cancelled."); }

    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (size == 0) { return failure(GetLastError(), L"Cannot prepare restart inheritance."); }
    Attributes attributes;
    attributes.storage.resize(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.storage.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) {
        return failure(GetLastError(), L"Cannot prepare restart inheritance.");
    }
    attributes.value = list;
    std::array inherited{parent.value, ready.value, cancel.value, error_write.value};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited.data(), inherited.size() * sizeof(HANDLE), nullptr, nullptr)) {
        return failure(GetLastError(), L"Cannot restrict restart handle inheritance.");
    }
    auto command = restart_command_line(executable, arguments,
        {reinterpret_cast<std::uintptr_t>(parent.value), reinterpret_cast<std::uintptr_t>(ready.value),
         reinterpret_cast<std::uintptr_t>(cancel.value), reinterpret_cast<std::uintptr_t>(error_write.value)});
    if (command.size() >= 32767) {
        return failure(ERROR_FILENAME_EXCED_RANGE, L"Restart command is too long.");
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = list;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable_text.c_str(), command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process)) {
        return failure(GetLastError(), L"Windows could not start the replacement process.");
    }
    Handle child_process{process.hProcess};
    Handle child_thread{process.hThread};
    error_write.close(); // Only the child may keep the diagnostic writer alive.
    const std::array wait{cancel.value, child_process.value, ready.value};
    const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(wait.size()), wait.data(), FALSE, kRestartTimeoutMs);
    if (result == WAIT_OBJECT_0 + 2) {
        if (cancellation_handle) {
            *cancellation_handle = reinterpret_cast<std::uintptr_t>(cancel.value);
            cancel.value = nullptr;
        }
        return {true, 0, {}};
    }
    const DWORD wait_error = result == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
    SetEvent(cancel.value);
    if (result == WAIT_OBJECT_0 + 1) {
        const auto detail = read_diagnostic(error_read.value);
        return failure(ERROR_APP_INIT_FAILURE, detail.empty()
            ? L"The replacement process exited before initialization completed." : detail);
    }
    if (result == WAIT_OBJECT_0) { return failure(ERROR_CANCELLED, L"Restart was cancelled."); }
    return failure(result == WAIT_TIMEOUT ? ERROR_TIMEOUT : wait_error,
        L"The replacement process did not finish initializing. The current process is still running.");
}

LaunchResult complete_restart_handoff(RestartHandles handles) {
    InheritedHandles inherited(std::array{handles.parent, handles.ready, handles.cancel, handles.error});
    if (!inherited.valid) { return failure(ERROR_INVALID_HANDLE, L"Invalid restart synchronization handles."); }
    const auto complete = [&] {
        if (WaitForSingleObject(inherited.owned[2].value, 0) == WAIT_OBJECT_0) {
            return failure(ERROR_CANCELLED, L"Restart was cancelled.");
        }
        if (!SetEvent(inherited.owned[1].value)) { return failure(GetLastError(), L"Cannot signal restart readiness."); }
        // A cancelled replacement must never activate when the retained parent
        // eventually quits, even if both events have become signalled.
        const std::array wait{inherited.owned[2].value, inherited.owned[0].value};
        const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(wait.size()), wait.data(), FALSE, INFINITE);
        if (result == WAIT_OBJECT_0 + 1) { return LaunchResult{true, 0, {}}; }
        return failure(result == WAIT_OBJECT_0 ? ERROR_CANCELLED : GetLastError(),
            L"The replacement process could not complete the restart handoff.");
    };
    const auto result = complete();
    if (!result.success) { write_diagnostic(inherited.owned[3].value, result.message); }
    return result;
}

void report_restart_failure(RestartHandles handles, std::wstring_view message) noexcept {
    InheritedHandles inherited(std::array{handles.parent, handles.ready, handles.cancel, handles.error});
    write_diagnostic(inherited.owned[3].value, message);
}

LaunchResult complete_update_startup(UpdateStartupHandles handles) {
    InheritedHandles inherited(std::array{handles.ready, handles.error});
    const auto pipe = inherited.owned[1].value;
    if (!inherited.valid || GetFileType(pipe) != FILE_TYPE_PIPE) {
        auto result = failure(ERROR_INVALID_HANDLE, L"Launcher received invalid update startup handles.");
        write_update_diagnostic(pipe, false, result.message);
        return result;
    }
    if (!SetEvent(inherited.owned[0].value)) {
        auto result = failure(GetLastError(), L"Launcher could not signal that update startup completed.");
        write_update_diagnostic(pipe, false, result.message);
        return result;
    }
    return {true, 0, {}};
}

void report_update_startup_failure(UpdateStartupHandles handles, bool settings_error, std::wstring_view message,
    std::wstring_view settings_file, std::uint32_t line, std::uint32_t column) noexcept {
    InheritedHandles inherited(std::array{handles.ready, handles.error});
    write_update_diagnostic(inherited.owned[1].value, settings_error, message, settings_file, line, column);
}

} // namespace launcher
