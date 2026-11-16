#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

void number(std::ofstream& output, std::uint32_t value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void text(std::ofstream& output, std::wstring_view value) {
    number(output, static_cast<std::uint32_t>(value.size()));
    output.write(reinterpret_cast<const char*>(value.data()),
        static_cast<std::streamsize>(value.size() * sizeof(wchar_t)));
}

int report(int count, wchar_t** arguments) {
    if (count < 2) {
        return 2;
    }
    try {
        const DWORD length = GetCurrentDirectoryW(0, nullptr);
        if (length == 0) {
            return 3;
        }
        std::wstring directory(length, L'\0');
        const DWORD written = GetCurrentDirectoryW(length, directory.data());
        if (written == 0 || written >= length) {
            return 3;
        }
        directory.resize(written);
        std::wstring executable(32768, L'\0');
        const DWORD executable_size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!executable_size || executable_size >= executable.size()) { return 3; }
        executable.resize(executable_size);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        GetStartupInfoW(&startup);
        const HWND console = GetConsoleWindow();
        DWORD console_process = 0;
        const bool console_attached = GetConsoleProcessList(&console_process, 1) != 0;
        std::ofstream output(std::filesystem::path(arguments[1]),
            std::ios::binary | std::ios::trunc);
        if (!output) {
            return 4;
        }
        number(output, 0x4c41554e); // LAUN: the receipt belongs to our own probe.
        number(output, GetCurrentProcessId());
        number(output, startup.dwFlags);
        number(output, startup.wShowWindow);
        number(output, console_attached ? 1U : 0U);
        number(output, console != nullptr && IsWindowVisible(console) ? 1U : 0U);
        text(output, directory);
        number(output, static_cast<std::uint32_t>(count));
        for (int index = 0; index < count; ++index) {
            text(output, arguments[index]);
        }
        const DWORD environment_size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        std::wstring environment(environment_size, L'\0');
        if (environment_size != 0) {
            const DWORD environment_written = GetEnvironmentVariableW(L"PATH", environment.data(), environment_size);
            if (environment_written >= environment_size) {
                return 6;
            }
            environment.resize(environment_written);
        }
        text(output, environment);
        text(output, executable);
        output.close();
        return output ? 0 : 4;
    } catch (...) {
        return 5;
    }
}

} // namespace

#if LAUNCHER_CONSOLE_PROBE
int wmain(int count, wchar_t** arguments) {
    return report(count, arguments);
}
#else
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr) {
        return 2;
    }
    const int result = report(count, arguments);
    LocalFree(arguments);
    return result;
}
#endif
