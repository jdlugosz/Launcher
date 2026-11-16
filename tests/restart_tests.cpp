#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/restart.hpp"
#include "launcher/settings.hpp"
#include "win32/window_text_reply.hpp"

#include <catch2/catch_test_macros.hpp>

#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct TestHandle final {
    HANDLE value = nullptr;
    ~TestHandle() { if (value && value != INVALID_HANDLE_VALUE) { CloseHandle(value); } }
};

std::filesystem::path test_directory() {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    REQUIRE(length > 0);
    REQUIRE(length < executable.size());
    executable.resize(length);
    return std::filesystem::path(executable).parent_path();
}

std::wstring probe_argument(std::wstring_view value) {
    // The owned temporary filenames and event names contain no quote characters.
    REQUIRE(value.find(L'"') == std::wstring_view::npos);
    return L"\"" + std::wstring(value) + L"\"";
}

HANDLE duplicate(HANDLE handle) {
    HANDLE result = nullptr;
    REQUIRE(DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &result, 0, FALSE,
        DUPLICATE_SAME_ACCESS));
    return result;
}

bool closed(std::uintptr_t handle) {
    DWORD flags = 0;
    return !GetHandleInformation(reinterpret_cast<HANDLE>(handle), &flags) &&
        GetLastError() == ERROR_INVALID_HANDLE;
}

struct HandoffChannels final {
    TestHandle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    TestHandle cancel{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    TestHandle read;
    launcher::RestartHandles child;

    explicit HandoffChannels(HANDLE parent) {
        REQUIRE(ready.value != nullptr);
        REQUIRE(cancel.value != nullptr);
        HANDLE write = nullptr;
        REQUIRE(CreatePipe(&read.value, &write, nullptr, 0));
        child = {reinterpret_cast<std::uintptr_t>(duplicate(parent)),
            reinterpret_cast<std::uintptr_t>(duplicate(ready.value)),
            reinterpret_cast<std::uintptr_t>(duplicate(cancel.value)), reinterpret_cast<std::uintptr_t>(write)};
    }
};

struct TestProcess final {
    PROCESS_INFORMATION information{};
    bool suspended = false;

    ~TestProcess() {
        if (suspended && information.hThread) { ResumeThread(information.hThread); }
        if (information.hProcess) {
            // These handles refer exclusively to processes created by this test.
            if (WaitForSingleObject(information.hProcess, 2000) == WAIT_TIMEOUT) {
                TerminateProcess(information.hProcess, 99);
                WaitForSingleObject(information.hProcess, 2000);
            }
            CloseHandle(information.hProcess);
        }
        if (information.hThread) { CloseHandle(information.hThread); }
    }
};

struct RestartFixture final {
    std::filesystem::path directory;
    std::filesystem::path configuration;
    std::filesystem::path receipt;
    std::wstring event_prefix;

    RestartFixture() {
        static std::atomic<unsigned> sequence = 0;
        const auto unique = std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence.fetch_add(1));
        directory = std::filesystem::temp_directory_path() / (L"Launcher restart 資料 " + unique);
        REQUIRE(std::filesystem::create_directory(directory));
        configuration = directory / L"fixture with spaces.toml";
        receipt = directory / L"parent receipt.bin";
        event_prefix = L"Local\\LauncherRestartTest-" + unique;
    }

    ~RestartFixture() {
        std::error_code ignored;
        std::filesystem::remove(receipt, ignored);
        std::filesystem::remove(configuration, ignored);
        std::filesystem::remove(launcher::runtime_settings_path(configuration), ignored);
        std::filesystem::remove(directory / L"restart.log", ignored);
        std::filesystem::remove(directory / L"colors-palette-fixture.toml", ignored);
        std::filesystem::remove(directory, ignored);
    }

    void write(std::string_view source) {
        std::ofstream output(configuration, std::ios::binary | std::ios::trunc);
        output.write(source.data(), static_cast<std::streamsize>(source.size()));
        output.close();
        REQUIRE(output);
    }

    std::string read() const {
        std::ifstream input(configuration, std::ios::binary);
        REQUIRE(input);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
};

HWND process_window(DWORD process, const wchar_t* class_name) {
    struct Search { DWORD process; const wchar_t* class_name; HWND window = nullptr; } search{process, class_name};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto& target = *reinterpret_cast<Search*>(parameter);
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process != target.process) { return TRUE; }
        wchar_t name[128]{};
        const int length = GetClassNameW(window, name, 128);
        if (length > 0 && std::wstring_view(name, static_cast<std::size_t>(length)) == target.class_name) {
            target.window = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

bool responsive(HWND window) {
    DWORD_PTR result = 0;
    return window && SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result);
}

struct UpdateStartupSession final {
    TestHandle ready;
    TestHandle error_read;
    TestProcess process;

    explicit UpdateStartupSession(const RestartFixture& fixture)
        : UpdateStartupSession(test_directory() / L"Launcher.exe", fixture.configuration) {}

    UpdateStartupSession(const std::filesystem::path& executable, const std::filesystem::path& configuration) {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        ready.value = CreateEventW(&security, TRUE, FALSE, nullptr);
        REQUIRE(ready.value != nullptr);
        TestHandle error_write;
        REQUIRE(CreatePipe(&error_read.value, &error_write.value, &security, 4096));
        REQUIRE(SetHandleInformation(error_read.value, HANDLE_FLAG_INHERIT, 0));
        auto command = probe_argument(executable.wstring());
        if (!configuration.empty()) { command += L" --config " + probe_argument(configuration.wstring()); }
        command += L" --update-ready " + std::to_wstring(reinterpret_cast<std::uintptr_t>(ready.value)) +
            L" --update-error " + std::to_wstring(reinterpret_cast<std::uintptr_t>(error_write.value));
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        REQUIRE(bytes > 0);
        std::vector<std::byte> storage(bytes);
        struct Attributes final {
            LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
            ~Attributes() { if (value) { DeleteProcThreadAttributeList(value); } }
        } attributes;
        const auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        REQUIRE(InitializeProcThreadAttributeList(list, 1, 0, &bytes));
        attributes.value = list;
        HANDLE inherited[]{ready.value, error_write.value};
        REQUIRE(UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited, sizeof(inherited), nullptr, nullptr));
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = list;
        REQUIRE(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process.information));
    }
};

struct ParentReceipt {
    DWORD parent = 0;
    DWORD child = 0;
    bool success = false;
    DWORD error = 0;
    std::wstring message;
};

ParentReceipt read_receipt(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input);
    const auto number = [&input] {
        std::uint32_t value = 0;
        input.read(reinterpret_cast<char*>(&value), sizeof(value));
        REQUIRE(input);
        return value;
    };
    REQUIRE(number() == 0x52535450);
    ParentReceipt result;
    result.parent = number();
    result.child = number();
    result.success = number() != 0;
    result.error = number();
    const auto length = number();
    REQUIRE(length <= 16384);
    result.message.resize(length);
    input.read(reinterpret_cast<char*>(result.message.data()), static_cast<std::streamsize>(length * sizeof(wchar_t)));
    REQUIRE(input);
    return result;
}

struct RestartSession final {
    TestHandle release;
    TestHandle reported;
    TestHandle abort;
    TestHandle child;
    TestProcess parent;
    std::filesystem::path executable = test_directory() / L"Launcher.exe";
    std::vector<HANDLE> successors;

    explicit RestartSession(const RestartFixture& fixture) {
        const auto release_name = fixture.event_prefix + L"-release";
        const auto reported_name = fixture.event_prefix + L"-reported";
        const auto abort_name = fixture.event_prefix + L"-abort";
        release.value = CreateEventW(nullptr, TRUE, FALSE, release_name.c_str());
        reported.value = CreateEventW(nullptr, TRUE, FALSE, reported_name.c_str());
        abort.value = CreateEventW(nullptr, TRUE, FALSE, abort_name.c_str());
        REQUIRE(release.value != nullptr);
        REQUIRE(reported.value != nullptr);
        REQUIRE(abort.value != nullptr);
        const auto probe = test_directory() / L"LauncherRestartProbe.exe";
        auto command = probe_argument(probe.wstring()) + L" " + probe_argument(executable.wstring()) + L" " +
            probe_argument(fixture.configuration.wstring()) + L" " + probe_argument(fixture.receipt.wstring()) + L" " +
            probe_argument(release_name) + L" " + probe_argument(reported_name) + L" " + probe_argument(abort_name);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        REQUIRE(CreateProcessW(probe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
            &startup, &parent.information));
    }

    void find_successors() {
        if (!child.value) { return; }
        std::vector<HANDLE> parents{child.value};
        parents.insert(parents.end(), successors.begin(), successors.end());
        for (std::size_t index = 0; index < parents.size(); ++index) {
            const DWORD parent_id = GetProcessId(parents[index]);
            FILETIME parent_created{}, parent_exited{}, kernel{}, user{};
            if (!GetProcessTimes(parents[index], &parent_created, &parent_exited, &kernel, &user)) { continue; }
            TestHandle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
            PROCESSENTRY32W entry{sizeof(entry)};
            if (snapshot.value == INVALID_HANDLE_VALUE || !Process32FirstW(snapshot.value, &entry)) { continue; }
            do {
                if (entry.th32ParentProcessID != parent_id) { continue; }
                bool known = false;
                for (const HANDLE process : parents) { known |= GetProcessId(process) == entry.th32ProcessID; }
                if (known) { continue; }
                HANDLE candidate = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE,
                    FALSE, entry.th32ProcessID);
                if (!candidate) { continue; }
                wchar_t path[32768]{};
                DWORD length = 32768;
                FILETIME created{}, exited{};
                const bool owned = QueryFullProcessImageNameW(candidate, 0, path, &length) &&
                    CompareStringOrdinal(path, static_cast<int>(length), executable.c_str(), -1, TRUE) == CSTR_EQUAL &&
                    GetProcessTimes(candidate, &created, &exited, &kernel, &user) &&
                    CompareFileTime(&created, &parent_created) >= 0 &&
                    (!(parent_exited.dwLowDateTime || parent_exited.dwHighDateTime) || CompareFileTime(&created, &parent_exited) <= 0);
                if (owned) { successors.push_back(candidate); parents.push_back(candidate); }
                else { CloseHandle(candidate); }
            } while (Process32NextW(snapshot.value, &entry));
        }
    }

    ~RestartSession() {
        // Find a child even if an assertion interrupted reading the receipt.
        // Parent ID, executable path and creation time protect unrelated copies.
        if (!child.value && parent.information.hProcess) {
            TestHandle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (snapshot.value != INVALID_HANDLE_VALUE && Process32FirstW(snapshot.value, &entry)) {
                do {
                    if (entry.th32ParentProcessID != parent.information.dwProcessId) { continue; }
                    HANDLE candidate = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE,
                        FALSE, entry.th32ProcessID);
                    if (!candidate) { continue; }
                    wchar_t path[32768]{};
                    DWORD length = 32768;
                    FILETIME parent_created{}, child_created{}, exited{}, kernel{}, user{};
                    const bool owned = QueryFullProcessImageNameW(candidate, 0, path, &length) &&
                        CompareStringOrdinal(path, static_cast<int>(length), executable.c_str(), -1, TRUE) == CSTR_EQUAL &&
                        GetProcessTimes(parent.information.hProcess, &parent_created, &exited, &kernel, &user) &&
                        GetProcessTimes(candidate, &child_created, &exited, &kernel, &user) &&
                        CompareFileTime(&child_created, &parent_created) >= 0;
                    if (owned) { child.value = candidate; break; }
                    CloseHandle(candidate);
                } while (Process32NextW(snapshot.value, &entry));
            }
        }
        if (release.value) { SetEvent(release.value); }
        if (parent.information.hProcess) { WaitForSingleObject(parent.information.hProcess, 2000); }
        const auto close = [](HANDLE process_handle) {
            const DWORD process = GetProcessId(process_handle);
            if (const HWND window = process_window(process, L"Launcher.DrawingPrototype.Window")) {
                PostMessageW(window, WM_CLOSE, 0, 0);
            }
            if (WaitForSingleObject(process_handle, 2000) == WAIT_TIMEOUT) {
                TerminateProcess(process_handle, 99);
                WaitForSingleObject(process_handle, 2000);
            }
        };
        if (child.value) {
            find_successors();
            close(child.value);
            find_successors();
            for (std::size_t index = 0; index < successors.size(); ++index) {
                close(successors[index]);
                find_successors();
            }
        }
        for (const HANDLE successor : successors) { CloseHandle(successor); }
    }
};

} // namespace

TEST_CASE("Restart preserves quoted Unicode configuration and UI mode", "[restart]") {
    const std::filesystem::path executable = L"C:\\Program Files\\Długosz\\Launcher\\Launcher-WIP.exe";
    const std::filesystem::path configuration = LR"(C:\資料\folder "quoted"\ending\)";
    for (const bool test_ui : {false, true}) {
        auto normal_arguments = L"--config " + win32::quote_command_argument(configuration.wstring());
        if (test_ui) { normal_arguments += L" --test-ui"; }
        const auto command = launcher::restart_command_line(executable, normal_arguments, {42, 43, 44, 45});
        int count = 0;
        wchar_t** arguments = CommandLineToArgvW(command.c_str(), &count);
        REQUIRE(arguments != nullptr);
        struct ArgumentOwner { wchar_t** value; ~ArgumentOwner() { LocalFree(value); } } owner{arguments};
        REQUIRE(count == (test_ui ? 12 : 11));
        REQUIRE(std::wstring(arguments[0]) == executable.wstring());
        REQUIRE(std::wstring(arguments[1]) == L"--restart-wait");
        REQUIRE(std::wstring(arguments[2]) == L"42");
        REQUIRE(std::wstring(arguments[3]) == L"--restart-ready");
        REQUIRE(std::wstring(arguments[4]) == L"43");
        REQUIRE(std::wstring(arguments[5]) == L"--restart-cancel");
        REQUIRE(std::wstring(arguments[6]) == L"44");
        REQUIRE(std::wstring(arguments[7]) == L"--restart-error");
        REQUIRE(std::wstring(arguments[8]) == L"45");
        REQUIRE(std::wstring(arguments[9]) == L"--config");
        REQUIRE(std::wstring(arguments[10]) == configuration.wstring());
        if (test_ui) { REQUIRE(std::wstring(arguments[11]) == L"--test-ui"); }
    }
}

TEST_CASE("Restart rejects invalid handoffs and missing executables", "[restart]") {
    REQUIRE_FALSE(launcher::complete_restart_handoff({}).success);
    REQUIRE_FALSE(launcher::complete_restart_handoff({std::numeric_limits<std::uintptr_t>::max(), 0, 0, 0}).success);
    REQUIRE_FALSE(launcher::restart_launcher(L"Launcher.exe", L"--config C:\\test.toml").success);
    const std::wstring invalid(L"--option first\0hidden", 21);
    REQUIRE(launcher::restart_launcher(L"C:\\Launcher.exe", invalid).error_code == ERROR_INVALID_PARAMETER);
    const auto absent = std::filesystem::temp_directory_path() /
        (L"Launcher-no-executable-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()) + L".exe");
    REQUIRE_FALSE(std::filesystem::exists(absent));
    const auto result = launcher::restart_launcher(absent, L"--config " + win32::quote_command_argument((absent.parent_path() / L"test.toml").wstring()));
    REQUIRE_FALSE(result.success);
    REQUIRE(result.error_code != 0);
    const auto implicit = launcher::restart_launcher(absent, {});
    REQUIRE_FALSE(implicit.success);
    REQUIRE(implicit.error_code != ERROR_INVALID_PARAMETER);
}

TEST_CASE("Restart preserves implicit configuration lookup without adding a config argument", "[restart][file-lookup]") {
    const std::filesystem::path executable = L"C:\\Portable Launcher\\Launcher.exe";
    const std::vector<std::wstring> extra{L"--fixture-option", L"fixture value"};
    for (const bool test_ui : {false, true}) {
        std::wstring normal_arguments = test_ui ? L"--test-ui " : L"";
        normal_arguments += win32::quote_command_argument(extra[0]) + L" " + win32::quote_command_argument(extra[1]);
        const auto command = launcher::restart_command_line(executable, normal_arguments, {42, 43, 44, 45});
        int count = 0;
        wchar_t** arguments = CommandLineToArgvW(command.c_str(), &count);
        REQUIRE(arguments != nullptr);
        struct ArgumentOwner { wchar_t** value; ~ArgumentOwner() { LocalFree(value); } } owner{arguments};
        const int offset = test_ui ? 10 : 9;
        REQUIRE(count == offset + 2);
        REQUIRE(std::wstring(arguments[0]) == executable.wstring());
        REQUIRE(std::wstring(arguments[1]) == L"--restart-wait");
        REQUIRE(std::wstring(arguments[2]) == L"42");
        REQUIRE(std::wstring(arguments[3]) == L"--restart-ready");
        REQUIRE(std::wstring(arguments[4]) == L"43");
        REQUIRE(std::wstring(arguments[5]) == L"--restart-cancel");
        REQUIRE(std::wstring(arguments[6]) == L"44");
        REQUIRE(std::wstring(arguments[7]) == L"--restart-error");
        REQUIRE(std::wstring(arguments[8]) == L"45");
        if (test_ui) { REQUIRE(std::wstring(arguments[9]) == L"--test-ui"); }
        REQUIRE(std::wstring(arguments[offset]) == extra[0]);
        REQUIRE(std::wstring(arguments[offset + 1]) == extra[1]);
        for (int index = 0; index < count; ++index) { REQUIRE(std::wstring(arguments[index]) != L"--config"); }
    }
}

TEST_CASE("Restart preserves synthesized argument text through Windows command-line parsing", "[restart]") {
    const std::filesystem::path executable = L"C:\\Program Files\\Launcher\\Launcher.exe";
    const std::filesystem::path configuration = L"C:\\fixture\\test.toml";
    const std::vector<std::wstring> extra{L"--fixture-option", LR"(C:\資料 & percent%\quoted "name"\)",
        L"--an-option is part of this one value", L"", L"a\\\"b\\"};
    auto normal_arguments = L"--config " + win32::quote_command_argument(configuration.wstring());
    for (const auto& argument : extra) { normal_arguments += L" " + win32::quote_command_argument(argument); }
    const auto command = launcher::restart_command_line(executable, normal_arguments, {42, 43, 44, 45});
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(command.c_str(), &count);
    REQUIRE(arguments);
    struct ArgumentOwner { wchar_t** value; ~ArgumentOwner() { LocalFree(value); } } owner{arguments};
    REQUIRE(static_cast<std::size_t>(count) == 11 + extra.size());
    for (std::size_t index = 0; index < extra.size(); ++index) { CHECK(std::wstring(arguments[11 + index]) == extra[index]); }
    const std::wstring invalid(L"first\0hidden", 12);
    REQUIRE_THROWS_AS(launcher::restart_command_line(executable, invalid, {}), std::invalid_argument);
    const auto rejected = launcher::restart_launcher(executable, invalid);
    CHECK_FALSE(rejected.success);
    CHECK(rejected.error_code == ERROR_INVALID_PARAMETER);
}

TEST_CASE("Restart wait pins the parent process until exit and closes its handle", "[restart]") {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    REQUIRE(length > 0);
    REQUIRE(length < executable.size());
    executable.resize(length);
    const auto probe = std::filesystem::path(executable).parent_path() / L"LauncherLaunchGuiProbe.exe";
    struct Receipt final {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            (L"Launcher-restart-receipt-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(GetTickCount64()) + L".bin");
        ~Receipt() { std::error_code error; std::filesystem::remove(path, error); }
    } receipt;
    struct Child final {
        PROCESS_INFORMATION process{};
        ~Child() {
            if (process.hThread) { ResumeThread(process.hThread); }
            if (process.hProcess) {
                WaitForSingleObject(process.hProcess, 5000);
                CloseHandle(process.hProcess);
            }
            if (process.hThread) { CloseHandle(process.hThread); }
        }
    } child;
    auto command = L"\"" + probe.wstring() + L"\" \"" + receipt.path.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    REQUIRE(CreateProcessW(probe.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_SUSPENDED, nullptr, nullptr, &startup, &child.process));
    HandoffChannels channels(child.process.hProcess);
    const auto inherited = channels.child;
    std::promise<launcher::LaunchResult> completion;
    auto completed = completion.get_future();
    std::thread waiter([&completion, inherited] {
        completion.set_value(launcher::complete_restart_handoff(inherited));
    });
    const DWORD ready = WaitForSingleObject(channels.ready.value, 5000);
    const auto before_exit = completed.wait_for(std::chrono::milliseconds(40));
    const DWORD resumed = ResumeThread(child.process.hThread);
    // This process is the dedicated test probe, never a user application.
    if (resumed == static_cast<DWORD>(-1)) { TerminateProcess(child.process.hProcess, 99); }
    waiter.join();
    const auto result = completed.get();
    REQUIRE(ready == WAIT_OBJECT_0);
    REQUIRE(before_exit == std::future_status::timeout);
    REQUIRE(resumed != static_cast<DWORD>(-1));
    REQUIRE(result.success);
    REQUIRE(closed(inherited.parent));
    REQUIRE(closed(inherited.ready));
    REQUIRE(closed(inherited.cancel));
    REQUIRE(closed(inherited.error));
    REQUIRE(std::filesystem::exists(receipt.path));
}

TEST_CASE("A canceled restart child does not wait for the parent to exit", "[restart]") {
    RestartFixture fixture;
    const auto probe = test_directory() / L"LauncherLaunchGuiProbe.exe";
    TestProcess parent;
    auto command = probe_argument(probe.wstring()) + L" " + probe_argument(fixture.receipt.wstring());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    REQUIRE(CreateProcessW(probe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
        nullptr, nullptr, &startup, &parent.information));
    parent.suspended = true;
    HandoffChannels channels(parent.information.hProcess);
    const auto inherited = channels.child;
    std::promise<launcher::LaunchResult> completion;
    auto completed = completion.get_future();
    std::thread waiter([&completion, inherited] {
        completion.set_value(launcher::complete_restart_handoff(inherited));
    });
    const DWORD ready = WaitForSingleObject(channels.ready.value, 5000);
    const BOOL canceled = SetEvent(channels.cancel.value);
    const auto before_parent_exit = completed.wait_for(std::chrono::seconds(2));
    const DWORD parent_state = WaitForSingleObject(parent.information.hProcess, 0);
    const DWORD resumed = ResumeThread(parent.information.hThread);
    parent.suspended = false;
    if (resumed == static_cast<DWORD>(-1)) { TerminateProcess(parent.information.hProcess, 99); }
    waiter.join();
    const auto result = completed.get();
    REQUIRE(ready == WAIT_OBJECT_0);
    REQUIRE(canceled);
    REQUIRE(before_parent_exit == std::future_status::ready);
    REQUIRE(parent_state == WAIT_TIMEOUT);
    REQUIRE_FALSE(result.success);
    REQUIRE(result.error_code == ERROR_CANCELLED);
    REQUIRE(closed(inherited.parent));
    REQUIRE(closed(inherited.ready));
    REQUIRE(closed(inherited.cancel));
    REQUIRE(closed(inherited.error));
}

TEST_CASE("A replacement syntax error preserves its parent process and configuration", "[restart][restart-process]") {
    RestartFixture fixture;
    const std::string source = "# Preserve the broken fixture for correction\n[[tabs]\n";
    fixture.write(source);
    RestartSession session(fixture);
    REQUIRE(WaitForSingleObject(session.reported.value, 20000) == WAIT_OBJECT_0);
    const auto receipt = read_receipt(fixture.receipt);
    REQUIRE(receipt.parent == session.parent.information.dwProcessId);
    REQUIRE_FALSE(receipt.success);
    REQUIRE(receipt.error != 0);
    REQUIRE(receipt.message.find(L"TOML") != std::wstring::npos);
    REQUIRE(receipt.child == 0);
    REQUIRE(WaitForSingleObject(session.parent.information.hProcess, 0) == WAIT_TIMEOUT);
    REQUIRE(responsive(process_window(receipt.parent, L"LauncherRestartParentProbe")));
    REQUIRE(fixture.read() == source);
    REQUIRE(SetEvent(session.release.value));
    REQUIRE(WaitForSingleObject(session.parent.information.hProcess, 5000) == WAIT_OBJECT_0);
    DWORD exit = 99;
    REQUIRE(GetExitCodeProcess(session.parent.information.hProcess, &exit));
    REQUIRE(exit == 0);
}

TEST_CASE("Monitored update startup reports a usable window or a typed settings error", "[update-startup][restart-process]") {
    RestartFixture fixture;
    const auto failure_diagnostic = [&] {
        UpdateStartupSession session(fixture);
        REQUIRE(WaitForSingleObject(session.process.information.hProcess, 15000) == WAIT_OBJECT_0);
        REQUIRE(WaitForSingleObject(session.ready.value, 0) == WAIT_TIMEOUT);
        DWORD exit = 0;
        REQUIRE(GetExitCodeProcess(session.process.information.hProcess, &exit));
        REQUIRE(exit != 0);
        wchar_t buffer[2048]{};
        DWORD bytes = 0;
        REQUIRE(ReadFile(session.error_read.value, buffer, sizeof(buffer), &bytes, nullptr));
        REQUIRE(bytes % sizeof(wchar_t) == 0);
        return std::wstring(buffer, bytes / sizeof(wchar_t));
    };
    SECTION("the main message pump signals readiness") {
        fixture.write(R"(schema_version = 1
[logging]
file = 'restart.log'
[dock]
edge = 'top'
auto_hide = true
[grid]
columns = 2
rows = 1
[[tabs]]
name = 'Update fixture'
color = 'red'
items = [{ label = 'Folder', icon = { stock = 'folder' } }]
)");
        UpdateStartupSession session(fixture);
        const HANDLE handles[]{session.process.information.hProcess, session.ready.value};
        REQUIRE(WaitForMultipleObjects(2, handles, FALSE, 15000) == WAIT_OBJECT_0 + 1);
        const HWND window = process_window(session.process.information.dwProcessId, L"Launcher.DrawingPrototype.Window");
        REQUIRE(responsive(window));
        REQUIRE(PostMessageW(window, WM_CLOSE, 0, 0));
        REQUIRE(WaitForSingleObject(session.process.information.hProcess, 5000) == WAIT_OBJECT_0);
        DWORD exit = 99;
        REQUIRE(GetExitCodeProcess(session.process.information.hProcess, &exit));
        REQUIRE(exit == 0);
    }
    SECTION("invalid TOML exits before readiness and provides the settings diagnostic") {
        const std::string source = "# Preserve the broken fixture for correction\nbroken = [";
        fixture.write(source);
        const auto diagnostic = failure_diagnostic();
        CAPTURE(diagnostic);
        REQUIRE(diagnostic.starts_with(L"settings\nSettings file: " + fixture.configuration.wstring() +
            L"\n\nSettings location: 2:11\n\nInvalid TOML:"));
        REQUIRE(fixture.read() == source);
    }
    SECTION("a broken palette identifies the secondary file") {
        fixture.write(R"(schema_version = 1
colors = 'palette-fixture'
[[tabs]]
name = 'Update fixture'
)");
        const auto palette = fixture.directory / L"colors-palette-fixture.toml";
        std::ofstream output(palette, std::ios::binary);
        output << "broken = [";
        output.close();
        REQUIRE(output);
        const auto diagnostic = failure_diagnostic();
        CAPTURE(diagnostic);
        REQUIRE(diagnostic.starts_with(L"settings\nSettings file: " + palette.wstring() +
            L"\n\nSettings location: 1:11\n\nInvalid TOML:"));
    }
    SECTION("an error without source coordinates still provides the settings file") {
        fixture.write("schema_version = 'invalid'\n");
        const auto diagnostic = failure_diagnostic();
        REQUIRE(diagnostic == L"settings\nSettings file: " + fixture.configuration.wstring() +
            L"\n\nconfiguration: 'schema_version' must be 1");
    }
}

TEST_CASE("A ready replacement stays hidden until its parent exits then serves native messages", "[restart][restart-process]") {
    RestartFixture fixture;
    const std::string source = R"(schema_version = 1
[logging]
file = 'restart.log'
[dock]
edge = 'top'
auto_hide = false
suppress_fullscreen = false
[grid]
columns = 2
rows = 1
icon_size_dip = 32
icon_spacing_px = 8
[[tabs]]
name = 'Restart fixture'
color = 'red'
items = [{ label = 'Folder', icon = { stock = 'folder' } }]
)";
    fixture.write(source);
    RestartSession session(fixture);
    REQUIRE(WaitForSingleObject(session.reported.value, 20000) == WAIT_OBJECT_0);
    const auto receipt = read_receipt(fixture.receipt);
    CAPTURE(receipt.message);
    REQUIRE(receipt.success);
    REQUIRE(receipt.error == 0);
    REQUIRE(receipt.parent == session.parent.information.dwProcessId);
    REQUIRE(receipt.child != 0);
    session.child.value = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE,
        FALSE, receipt.child);
    REQUIRE(session.child.value != nullptr);
    REQUIRE(WaitForSingleObject(session.parent.information.hProcess, 0) == WAIT_TIMEOUT);
    REQUIRE(WaitForSingleObject(session.child.value, 0) == WAIT_TIMEOUT);
    const HWND hidden = process_window(receipt.child, L"Launcher.DrawingPrototype.Window");
    REQUIRE(hidden != nullptr);
    REQUIRE_FALSE(IsWindowVisible(hidden));
    REQUIRE_FALSE(std::filesystem::exists(fixture.directory / L"restart.log"));
    REQUIRE(responsive(process_window(receipt.parent, L"LauncherRestartParentProbe")));
    REQUIRE(SetEvent(session.release.value));
    REQUIRE(WaitForSingleObject(session.parent.information.hProcess, 5000) == WAIT_OBJECT_0);
    bool visible = false;
    const auto started = GetTickCount64();
    while (GetTickCount64() - started < 5000) {
        const HWND window = process_window(receipt.child, L"Launcher.DrawingPrototype.Window");
        if (window && IsWindowVisible(window) && responsive(window)) { visible = true; break; }
        if (WaitForSingleObject(session.child.value, 20) == WAIT_OBJECT_0) { break; }
    }
    REQUIRE(visible);
    const auto expected_arguments = L"--config " + win32::quote_command_argument(fixture.configuration.wstring()) + L" --test-ui";
    CHECK(win32::query_window_text(process_window(receipt.child, L"Launcher.DrawingPrototype.Window"),
        win32::restart_arguments_request) == expected_arguments);
    REQUIRE(fixture.read() == source);
    REQUIRE(PostMessageW(process_window(receipt.child, L"Launcher.DrawingPrototype.Window"), WM_CLOSE, 0, 0));
    REQUIRE(WaitForSingleObject(session.child.value, 5000) == WAIT_OBJECT_0);
    DWORD exit = 99;
    REQUIRE(GetExitCodeProcess(session.child.value, &exit));
    REQUIRE(exit == 0);
    std::ifstream log(fixture.directory / L"restart.log", std::ios::binary);
    REQUIRE(log);
    const std::string messages{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
    REQUIRE(messages.find("Starting Launcher ") != std::string::npos);
    REQUIRE(messages.find("Restart handoff complete:") != std::string::npos);
}

TEST_CASE("Display change notifications reload once and preserve relative runtime settings", "[restart][restart-process]") {
    RestartFixture fixture;
    const std::string source = R"(schema_version = 1
[logging]
file = 'restart.log'
[dock]
edge = 'top'
auto_hide = false
suppress_fullscreen = false
[grid]
columns = 2
rows = 1
[[tabs]]
name = 'Display reload fixture'
items = [{ label = 'Folder', icon = { stock = 'folder' } }]
)";
    const std::string runtime_source = R"([adjustments]
text_scale = 1.1
icon_scale = 0.625
[grid]
icon_spacing_px = 9
[dock]
position = 0.31
[appearance]
thickness = 2.0
)";
    fixture.write(source);
    const auto runtime = launcher::runtime_settings_path(fixture.configuration);
    {
        std::ofstream output(runtime, std::ios::binary);
        output.write(runtime_source.data(), static_cast<std::streamsize>(runtime_source.size()));
        output.close();
        REQUIRE(output);
    }
    RestartSession session(fixture);
    REQUIRE(WaitForSingleObject(session.reported.value, 20000) == WAIT_OBJECT_0);
    const auto receipt = read_receipt(fixture.receipt);
    CAPTURE(receipt.message);
    REQUIRE(receipt.success);
    session.child.value = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE, FALSE, receipt.child);
    REQUIRE(session.child.value != nullptr);
    REQUIRE(SetEvent(session.release.value));
    REQUIRE(WaitForSingleObject(session.parent.information.hProcess, 5000) == WAIT_OBJECT_0);
    HWND original = nullptr;
    const auto reveal_started = GetTickCount64();
    do {
        original = process_window(receipt.child, L"Launcher.DrawingPrototype.Window");
        if (original && IsWindowVisible(original) && responsive(original)) { break; }
        WaitForSingleObject(session.child.value, 20);
    } while (GetTickCount64() - reveal_started < 5000);
    REQUIRE(original != nullptr);
    REQUIRE(IsWindowVisible(original));
    REQUIRE(responsive(original));
    const auto expected_arguments = L"--config " + win32::quote_command_argument(fixture.configuration.wstring()) + L" --test-ui";
    CHECK(win32::query_window_text(original, win32::restart_arguments_request) == expected_arguments);
    DEVMODEW display{};
    display.dmSize = static_cast<WORD>(sizeof(display));
    REQUIRE(EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &display));
    for (unsigned index = 0; index < 3; ++index) {
        REQUIRE(PostMessageW(original, WM_DISPLAYCHANGE, display.dmBitsPerPel, MAKELPARAM(display.dmPelsWidth, display.dmPelsHeight)));
    }
    const auto reload_started = GetTickCount64();
    do {
        session.find_successors();
        if (WaitForSingleObject(session.child.value, 20) == WAIT_OBJECT_0) { break; }
    } while (GetTickCount64() - reload_started < 15000);
    session.find_successors();
    REQUIRE(WaitForSingleObject(session.child.value, 0) == WAIT_OBJECT_0);
    DWORD exit = 99;
    REQUIRE(GetExitCodeProcess(session.child.value, &exit));
    CHECK(exit == 0);
    REQUIRE(session.successors.size() == 1);
    const HANDLE replacement = session.successors.front();
    const DWORD replacement_pid = GetProcessId(replacement);
    CHECK(replacement_pid != receipt.child);
    HWND restored = nullptr;
    const auto restored_started = GetTickCount64();
    do {
        restored = process_window(replacement_pid, L"Launcher.DrawingPrototype.Window");
        if (restored && IsWindowVisible(restored) && responsive(restored)) { break; }
        WaitForSingleObject(replacement, 20);
    } while (GetTickCount64() - restored_started < 5000);
    REQUIRE(restored != nullptr);
    REQUIRE(IsWindowVisible(restored));
    REQUIRE(responsive(restored));
    CHECK(win32::query_window_text(restored, win32::restart_arguments_request) == expected_arguments);
    REQUIRE(PostMessageW(restored, WM_CLOSE, 0, 0));
    REQUIRE(WaitForSingleObject(replacement, 5000) == WAIT_OBJECT_0);
    REQUIRE(GetExitCodeProcess(replacement, &exit));
    CHECK(exit == 0);
    CHECK(fixture.read() == source);
    std::ifstream saved(runtime, std::ios::binary);
    REQUIRE(saved);
    CHECK(std::string(std::istreambuf_iterator<char>(saved), std::istreambuf_iterator<char>()) == runtime_source);
    std::ifstream log(fixture.directory / L"restart.log", std::ios::binary);
    REQUIRE(log);
    const auto path_bytes = fixture.configuration.u8string();
    const std::string expected_config(path_bytes.begin(), path_bytes.end());
    const auto pid_marker = " pid=" + std::to_string(replacement_pid) + " ";
    unsigned total_starts = 0, fresh_starts = 0, fresh_baselines = 0, fresh_handoffs = 0;
    for (std::string line; std::getline(log, line);) {
        if (line.find("Starting Launcher ") != std::string::npos) { ++total_starts; }
        if (line.find(pid_marker) == std::string::npos) { continue; }
        if (line.find("Starting Launcher ") != std::string::npos && line.find("; config=" + expected_config + ".") != std::string::npos) { ++fresh_starts; }
        if (line.find("Desktop icon baseline: ") != std::string::npos) { ++fresh_baselines; }
        if (line.find("Restart handoff complete:") != std::string::npos) { ++fresh_handoffs; }
    }
    CHECK(total_starts == 2);
    CHECK(fresh_starts == 1);
    CHECK(fresh_baselines == 1);
    CHECK(fresh_handoffs == 1);
}

TEST_CASE("A ready replacement can be abandoned while its parent remains usable", "[restart][restart-process]") {
    RestartFixture fixture;
    const std::string source = "[logging]\nfile = 'restart.log'\n[dock]\nauto_hide = false\n[[tabs]]\nname = 'Cancel ready child'\nitems = []\n";
    fixture.write(source);
    RestartSession session(fixture);
    REQUIRE(WaitForSingleObject(session.reported.value, 20000) == WAIT_OBJECT_0);
    const auto receipt = read_receipt(fixture.receipt);
    CAPTURE(receipt.message);
    REQUIRE(receipt.success);
    REQUIRE(receipt.error == 0);
    REQUIRE(receipt.parent == session.parent.information.dwProcessId);
    REQUIRE(receipt.child != 0);
    session.child.value = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE,
        FALSE, receipt.child);
    REQUIRE(session.child.value != nullptr);
    const HWND hidden = process_window(receipt.child, L"Launcher.DrawingPrototype.Window");
    REQUIRE(hidden != nullptr);
    REQUIRE_FALSE(IsWindowVisible(hidden));
    REQUIRE(WaitForSingleObject(session.parent.information.hProcess, 0) == WAIT_TIMEOUT);
    REQUIRE(SetEvent(session.abort.value));
    REQUIRE(WaitForSingleObject(session.child.value, 5000) == WAIT_OBJECT_0);
    DWORD child_exit = 0;
    REQUIRE(GetExitCodeProcess(session.child.value, &child_exit));
    REQUIRE(child_exit == 1);
    REQUIRE_FALSE(std::filesystem::exists(fixture.directory / L"restart.log"));
    REQUIRE(WaitForSingleObject(session.parent.information.hProcess, 0) == WAIT_TIMEOUT);
    REQUIRE(responsive(process_window(receipt.parent, L"LauncherRestartParentProbe")));
    REQUIRE(fixture.read() == source);
    REQUIRE(SetEvent(session.release.value));
    REQUIRE(WaitForSingleObject(session.parent.information.hProcess, 5000) == WAIT_OBJECT_0);
    DWORD parent_exit = 99;
    REQUIRE(GetExitCodeProcess(session.parent.information.hProcess, &parent_exit));
    REQUIRE(parent_exit == 0);
}

TEST_CASE("A plain launch reveals the one default drawer for its installation", "[default-drawer][restart-process]") {
    RestartFixture fixture;
    fixture.configuration = fixture.directory / L"Launcher.toml";
    fixture.write(R"(schema_version = 1
[logging]
file = 'restart.log'
[dock]
edge = 'top'
auto_hide = true
hover_delay_ms = 5000
hide_delay_ms = 5000
reveal_duration_ms = 0
suppress_fullscreen = false
[[tabs]]
name = 'Default drawer fixture'
items = [{ label = 'Folder', icon = { stock = 'folder' } }]
)");
    struct CopiedExecutable final {
        std::filesystem::path path;
        ~CopiedExecutable() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } executable{fixture.directory / L"Launcher.exe"};
    REQUIRE(std::filesystem::copy_file(test_directory() / L"Launcher.exe", executable.path));

    const auto start = [&](TestProcess& process, std::wstring arguments = {}, bool suspended = false) {
        std::wstring command = probe_argument(executable.path.wstring());
        if (!arguments.empty()) { command += L" " + arguments; }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        REQUIRE(CreateProcessW(executable.path.c_str(), command.data(), nullptr, nullptr, FALSE,
            suspended ? CREATE_SUSPENDED : 0, nullptr, fixture.directory.c_str(), &startup, &process.information));
        process.suspended = suspended;
    };
    const auto wait_for_window = [](const TestProcess& process) {
        const auto start = GetTickCount64();
        do {
            const HWND window = process_window(process.information.dwProcessId, L"Launcher.DrawingPrototype.Window");
            if (window && IsWindowVisible(window) && responsive(window)) { return window; }
            if (WaitForSingleObject(process.information.hProcess, 20) == WAIT_OBJECT_0) { break; }
        } while (GetTickCount64() - start < 10000);
        return static_cast<HWND>(nullptr);
    };
    const auto require_clean_exit = [](const TestProcess& process) {
        REQUIRE(WaitForSingleObject(process.information.hProcess, 8000) == WAIT_OBJECT_0);
        DWORD exit = 99;
        REQUIRE(GetExitCodeProcess(process.information.hProcess, &exit));
        REQUIRE(exit == 0);
    };
    const auto close = [&](const TestProcess& process, HWND window) {
        REQUIRE(PostMessageW(window, WM_CLOSE, 0, 0));
        require_clean_exit(process);
    };

    // Setup supplies readiness handles even when it relaunches the default drawer.
    UpdateStartupSession setup(executable.path, {});
    const TestProcess& owner = setup.process;
    const HANDLE startup[]{owner.information.hProcess, setup.ready.value};
    REQUIRE(WaitForMultipleObjects(2, startup, FALSE, 15000) == WAIT_OBJECT_0 + 1);
    const HWND original = wait_for_window(owner);
    REQUIRE(original != nullptr);
    RECT collapsed{};
    REQUIRE(GetWindowRect(original, &collapsed));
    REQUIRE(responsive(original));
    DWORD_PTR ignored = 0;
    REQUIRE(SendMessageTimeoutW(original, WM_SYSCOMMAND, SC_MINIMIZE, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &ignored));
    REQUIRE(IsIconic(original));

    TestProcess later;
    start(later);
    require_clean_exit(later);
    const auto reveal_started = GetTickCount64();
    bool expanded = false;
    do {
        RECT bounds{};
        expanded = IsWindowVisible(original) && !IsIconic(original) && GetWindowRect(original, &bounds) &&
            bounds.bottom - bounds.top > collapsed.bottom - collapsed.top;
        if (expanded) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (GetTickCount64() - reveal_started < 3000);
    REQUIRE(expanded);
    REQUIRE(process_window(owner.information.dwProcessId, L"Launcher.DrawingPrototype.Window") == original);

    TestProcess explicit_config;
    start(explicit_config, L"--config " + probe_argument(fixture.configuration.wstring()));
    const HWND independent = wait_for_window(explicit_config);
    REQUIRE(independent != nullptr);
    REQUIRE(independent != original);
    CHECK(win32::query_window_text(independent, win32::restart_arguments_request) ==
        L"--config " + win32::quote_command_argument(fixture.configuration.wstring()));
    close(explicit_config, independent);
    close(owner, original);

    TestProcess first;
    TestProcess second;
    start(first, {}, true);
    start(second, {}, true);
    REQUIRE(ResumeThread(first.information.hThread) == 1);
    first.suspended = false;
    REQUIRE(ResumeThread(second.information.hThread) == 1);
    second.suspended = false;
    const HANDLE processes[]{first.information.hProcess, second.information.hProcess};
    const DWORD exited = WaitForMultipleObjects(2, processes, FALSE, 12000);
    REQUIRE((exited == WAIT_OBJECT_0 || exited == WAIT_OBJECT_0 + 1));
    const bool first_exited = exited == WAIT_OBJECT_0;
    require_clean_exit(first_exited ? first : second);
    const auto& survivor = first_exited ? second : first;
    const HWND remaining = wait_for_window(survivor);
    REQUIRE(remaining != nullptr);
    close(survivor, remaining);
}
