#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "win32/window_text_reply.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

struct Handle final {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) { CloseHandle(value); } }
};

struct Process final {
    PROCESS_INFORMATION information{};
    ~Process() {
        if (information.hProcess) {
            if (WaitForSingleObject(information.hProcess, 2500) == WAIT_TIMEOUT) {
                // Only this fixture's freshly created subprocess is affected.
                TerminateProcess(information.hProcess, 99);
                WaitForSingleObject(information.hProcess, 1000);
            }
            CloseHandle(information.hProcess);
        }
        if (information.hThread) { CloseHandle(information.hThread); }
    }
};

std::filesystem::path probe_path() {
    std::wstring text(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, text.data(), static_cast<DWORD>(text.size()));
    REQUIRE(length > 0);
    REQUIRE(length < text.size());
    text.resize(length);
    return std::filesystem::path(text).parent_path() / L"LauncherWindowTextReplyProbe.exe";
}

HWND process_window(DWORD process) {
    struct Search { DWORD process; HWND result = nullptr; } search{process};
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(data);
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        wchar_t type[128]{};
        if (process == search.process && GetClassNameW(window, type, 128) &&
                std::wstring_view(type) == L"Dlugosz.WindowTextReply.Probe.v1") {
            search.result = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.result;
}

struct Probe final {
    Process process;
    HWND window = nullptr;
    std::wstring request;

    explicit Probe(std::wstring_view mode, std::wstring_view response = L"", std::wstring_view name = win32::restart_arguments_request) : request(name) {
        static std::atomic<unsigned> sequence{0};
        const auto event_name = L"Local\\WindowTextReply-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence.fetch_add(1));
        Handle ready{CreateEventW(nullptr, TRUE, FALSE, event_name.c_str())};
        REQUIRE(ready.value != nullptr);
        const auto executable = probe_path();
        auto command = win32::quote_command_argument(executable.wstring());
        for (const auto argument : {name, mode, response, std::wstring_view(event_name)}) {
            command += L" " + win32::quote_command_argument(argument);
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        REQUIRE(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process.information));
        const std::array events{ready.value, process.information.hProcess};
        REQUIRE(WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, 5000) == WAIT_OBJECT_0);
        window = process_window(process.information.dwProcessId);
        REQUIRE(window != nullptr);
    }

    ~Probe() { if (window) { PostMessageW(window, WM_CLOSE, 0, 0); } }

    std::wstring query(DWORD timeout = 5000) const { return win32::query_window_text(window, request, timeout); }
};

DWORD query_error(const Probe& probe, DWORD timeout = 5000) {
    try { static_cast<void>(probe.query(timeout)); }
    catch (const std::system_error& error) { return static_cast<DWORD>(error.code().value()); }
    FAIL("The window text query unexpectedly succeeded.");
}

} // namespace

TEST_CASE("Window text replies marshal configured Unicode across processes", "[window-text-reply]") {
    const std::wstring first = L"--config \"C:\\資料\\two docks\\右.toml\" --label \"🛠 café\"";
    Probe server(L"reply", first);
    REQUIRE(server.query() == first);
    REQUIRE(server.query() == first); // A new HWND/cookie must work after the first receiver is gone.
    Probe other(L"reply", L"a different application response", L"WindowTextReply.Test.Other.v1");
    REQUIRE(other.query() == L"a different application response");
    REQUIRE(server.query() == first);
}

TEST_CASE("Window text replies preserve empty and maximum-length text", "[window-text-reply]") {
    SECTION("Empty") { Probe server(L"empty"); REQUIRE(server.query().empty()); }
    SECTION("Maximum") {
        Probe server(L"max");
        REQUIRE(server.query() == std::wstring(win32::window_text_reply_limit - 1, L'x'));
    }
}

TEST_CASE("Window text queries reject malformed and unrelated responses", "[window-text-reply]") {
    for (const auto mode : {L"malformed", L"no-nul", L"embedded-nul", L"oversized", L"wrong-cookie", L"wrong-source", L"no-reply"}) {
        CAPTURE(mode);
        Probe server(mode);
        REQUIRE(query_error(server) == ERROR_INVALID_DATA);
    }
}

TEST_CASE("Window text queries distinguish unsupported requests and timeout", "[window-text-reply]") {
    SECTION("Unsupported") { Probe server(L"unsupported"); REQUIRE(query_error(server) == ERROR_NOT_SUPPORTED); }
    SECTION("Timeout") { Probe server(L"delay", L"late reply"); REQUIRE(query_error(server, 100) == ERROR_TIMEOUT); }
    SECTION("Destroyed during query") { Probe server(L"destroy"); REQUIRE(query_error(server) == ERROR_INVALID_WINDOW_HANDLE); }
    SECTION("Missing window") {
        REQUIRE_THROWS_AS(win32::query_window_text(nullptr, win32::restart_arguments_request), std::system_error);
    }
}

TEST_CASE("Window text replies validate configuration and ignore unrelated messages", "[window-text-reply]") {
    REQUIRE_THROWS_AS(win32::WindowTextReply(L"", L"text"), std::invalid_argument);
    REQUIRE_THROWS_AS(win32::WindowTextReply(std::wstring_view(L"bad\0name", 8), L"text"), std::invalid_argument);
    REQUIRE_THROWS_AS(win32::WindowTextReply(win32::restart_arguments_request, std::wstring(L"bad\0text", 8)), std::invalid_argument);
    REQUIRE_THROWS_AS(win32::WindowTextReply(win32::restart_arguments_request, std::wstring(win32::window_text_reply_limit, L'x')), std::invalid_argument);
    const win32::WindowTextReply reply(win32::restart_arguments_request, L"configured once");
    REQUIRE(reply.response() == L"configured once");
    REQUIRE_FALSE(reply.handle(nullptr, WM_NULL, 0, 0).has_value());
    const auto request = RegisterWindowMessageW(win32::restart_arguments_request);
    REQUIRE(reply.handle(nullptr, request, 0, 0) == LRESULT{-1});
}

TEST_CASE("Command arguments quote with Windows parsing rules", "[window-text-reply]") {
    const std::array<std::wstring_view, 7> arguments{L"", L"plain", L"two words", L"資料", L"C:\\trailing\\", L"a\"b", L"slashes\\\\\"and quote"};
    std::wstring command = L"program";
    for (const auto argument : arguments) { command += L" " + win32::quote_command_argument(argument); }
    int count = 0;
    auto** parsed = CommandLineToArgvW(command.c_str(), &count);
    REQUIRE(parsed != nullptr);
    struct Arguments final { wchar_t** value; ~Arguments() { LocalFree(value); } } owned{parsed};
    REQUIRE(static_cast<std::size_t>(count) == arguments.size() + 1);
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        REQUIRE(std::wstring_view(parsed[index + 1]) == arguments[index]);
    }
    REQUIRE_THROWS_AS(win32::quote_command_argument(std::wstring_view(L"bad\0arg", 7)), std::invalid_argument);
}
