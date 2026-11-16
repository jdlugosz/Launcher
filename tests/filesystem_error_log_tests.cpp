#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/filesystem_error_log.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using launcher::TargetScanDiagnostic;
using launcher::TargetScanOperation;

class Fixture final {
public:
    Fixture() : parent_(fs::weakly_canonical(fs::temp_directory_path())) {
        static std::atomic<unsigned> sequence = 0;
        root = parent_ / (L"LauncherFilesystemErrors-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence++));
        REQUIRE(fs::create_directory(root));
    }
    ~Fixture() {
        std::error_code error;
        const auto resolved = fs::weakly_canonical(root, error);
        if (!error && resolved == root && resolved.parent_path() == parent_) { fs::remove_all(root, error); }
    }
    fs::path root;

private:
    fs::path parent_;
};

class NativeHandle final {
public:
    explicit NativeHandle(HANDLE handle) : value_(handle) {}
    ~NativeHandle() { close(); }
    void close() noexcept { if (value_ != INVALID_HANDLE_VALUE) { CloseHandle(value_); value_ = INVALID_HANDLE_VALUE; } }
    HANDLE get() const noexcept { return value_; }

private:
    HANDLE value_;
};

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string utf8(std::wstring_view value) {
    const auto length = static_cast<int>(value.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, nullptr, 0, nullptr, nullptr);
    REQUIRE(size > 0);
    std::string result(static_cast<std::size_t>(size), '\0');
    REQUIRE(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, result.data(), size, nullptr, nullptr) == size);
    return result;
}

std::wstring system_message(DWORD code) {
    std::array<wchar_t, 16384> message{};
    const auto size = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
        message.data(), static_cast<DWORD>(message.size()), nullptr);
    REQUIRE(size > 0);
    std::wstring result(message.data(), size);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) { result.pop_back(); }
    return result;
}

TargetScanDiagnostic native_error(fs::path path, DWORD code, TargetScanOperation operation = TargetScanOperation::probe) {
    return {operation, std::move(path), {static_cast<int>(code), std::system_category()}};
}

bool append(launcher::FilesystemErrorLog& log, const TargetScanDiagnostic& diagnostic) {
    return log.append(std::span<const TargetScanDiagnostic>(&diagnostic, 1));
}

std::size_t occurrences(std::string_view text, std::string_view token) {
    std::size_t count = 0;
    for (std::size_t position = 0; (position = text.find(token, position)) != std::string_view::npos; position += token.size()) { ++count; }
    return count;
}

} // namespace

TEST_CASE("Filesystem errors have self-contained native code and Windows text", "[filesystem-error-log]") {
    Fixture fixture;
    const auto path = fixture.root / L"launcher.filesystem-errors.log";
    launcher::FilesystemErrorLog log(path);
    const std::array diagnostics{
        native_error(L"C:\\Applications\\Old\\app.exe", ERROR_ACCESS_DENIED),
        native_error(L"\\\\server\\share\\Versions\\*", ERROR_BAD_NETPATH, TargetScanOperation::enumerate),
        native_error(L"D:\\Programs\\Busy.exe", ERROR_SHARING_VIOLATION, TargetScanOperation::metadata)};
    REQUIRE(log.append(diagnostics));
    CHECK(log.path() == path);
    const auto text = read(path);
    CHECK(std::count(text.begin(), text.end(), '\n') == 3);
    CHECK(occurrences(text, "\"error_category\":\"Win32\"") == 3);
    CHECK(text.find("\"path\":\"C:\\\\Applications\\\\Old\\\\app.exe\"") != std::string::npos);
    for (const auto& diagnostic : diagnostics) {
        CHECK(text.find("\"error_code\":" + std::to_string(diagnostic.error.value()) + ",") != std::string::npos);
        CHECK(text.find(utf8(system_message(static_cast<DWORD>(diagnostic.error.value())))) != std::string::npos);
    }
    CHECK(std::regex_search(text, std::regex(R"("timestamp":"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z")")));
    CHECK(text.find("thread") == std::string::npos);
    CHECK(text.find("stack") == std::string::npos);
}

TEST_CASE("Filesystem journal preserves full Unicode paths and escaping", "[filesystem-error-log]") {
    Fixture fixture;
    const auto journal = fixture.root / L"errors.log";
    launcher::FilesystemErrorLog log(journal);
    std::wstring long_path = L"C:\\资料\\";
    for (int index = 0; index < 2000; ++index) { long_path += L"版本资料"; }
    long_path += L"\\应用😀.exe";
    REQUIRE(long_path.size() > 8000);
    REQUIRE(append(log, native_error(long_path, ERROR_READ_FAULT)));
    auto text = read(journal);
    auto complete_path = utf8(L"\"path\":\"C:\\\\资料\\\\");
    for (int index = 0; index < 2000; ++index) { complete_path += utf8(L"版本资料"); }
    complete_path += utf8(L"\\\\应用😀.exe\"");
    CHECK(text.find(complete_path) != std::string::npos);
    CHECK(occurrences(text, utf8(L"版本资料")) == 2000);

    std::wstring strange = L"C:\\quote\"\\line\r\n\t";
    strange += L'\0';
    strange += static_cast<wchar_t>(0x1b);
    strange += static_cast<wchar_t>(0x85);
    strange += static_cast<wchar_t>(0xd800);
    strange += L'x';
    strange += static_cast<wchar_t>(0xdc00);
    REQUIRE(append(log, native_error(strange, ERROR_CRC)));
    text = read(journal);
    CHECK(text.find(R"("path":"C:\\quote\"\\line\u000d\u000a\u0009\u0000\u001b\u0085\ud800x\udc00")") != std::string::npos);
    CHECK(std::count(text.begin(), text.end(), '\n') == 2);
    CHECK(text.find('\r') == std::string::npos);
    CHECK(text.find('\0') == std::string::npos);
}

TEST_CASE("Filesystem journal separates generic errors from internal diagnostics", "[filesystem-error-log]") {
    Fixture fixture;
    const auto path = fixture.root / L"errors.log";
    launcher::FilesystemErrorLog log(path);
    REQUIRE(log.append({}));
    CHECK_FALSE(fs::exists(path));
    class InternalCategory final : public std::error_category {
    public:
        const char* name() const noexcept override { return "launcher.internal"; }
        std::string message(int) const override { return "internal test"; }
    } internal_category;
    const auto generic = std::make_error_code(std::errc::permission_denied);
    const std::array diagnostics{
        TargetScanDiagnostic{TargetScanOperation::metadata, L"C:\\generic.exe", generic},
        native_error(L"C:\\normalization", ERROR_INVALID_NAME, TargetScanOperation::normalize),
        native_error(L"C:\\budget", ERROR_NOT_ENOUGH_MEMORY, TargetScanOperation::budget),
        TargetScanDiagnostic{TargetScanOperation::probe, L"C:\\custom", {7, internal_category}},
        TargetScanDiagnostic{TargetScanOperation::probe, L"C:\\limited", generic, launcher::TargetScanLimit::entries},
        native_error(L"C:\\success", ERROR_SUCCESS)};
    REQUIRE(log.append(diagnostics));
    const auto text = read(path);
    CHECK(std::count(text.begin(), text.end(), '\n') == 1);
    CHECK(text.find("\"error_category\":\"generic\"") != std::string::npos);
    CHECK(text.find("\"error_code\":" + std::to_string(generic.value()) + ",") != std::string::npos);
    CHECK(text.find(generic.message()) != std::string::npos);
    CHECK(text.find("Win32") == std::string::npos);
    CHECK(text.find("custom") == std::string::npos);
    CHECK(text.find("limited") == std::string::npos);

    REQUIRE(append(log, native_error(L"C:\\unknown.exe", 1234567890)));
    CHECK(read(path).find("Windows has no message text for error 1234567890.") != std::string::npos);
    REQUIRE(append(log, native_error(L"C:\\high-code.exe", 0xffffffffu)));
    CHECK(read(path).find("\"error_code\":4294967295,") != std::string::npos);
}

TEST_CASE("Filesystem journal keeps complete oversized records through rotation and retention", "[filesystem-error-log]") {
    Fixture fixture;
    const auto path = fixture.root / L"errors.log";
    auto backup = path;
    backup += L".1";
    launcher::FilesystemErrorLog log(path, 256);
    const auto long_name = std::wstring(1200, L'资');
    REQUIRE(append(log, native_error(L"C:\\first-" + long_name, ERROR_ACCESS_DENIED)));
    CHECK(fs::file_size(path) > 256);
    CHECK_FALSE(fs::exists(backup));
    const auto first = read(path);
    REQUIRE(append(log, native_error(L"C:\\second-" + long_name, ERROR_READ_FAULT)));
    CHECK(read(backup) == first);
    const auto second = read(path);
    CHECK(std::count(second.begin(), second.end(), '\n') == 1);
    REQUIRE(append(log, native_error(L"C:\\third-" + long_name, ERROR_CRC)));
    CHECK(read(backup) == second);
    const auto third = read(path);
    CHECK(third.find("third-") != std::string::npos);
    CHECK(std::count(third.begin(), third.end(), '\n') == 1);
    std::vector<fs::path> archives;
    for (const auto& entry : fs::directory_iterator(fixture.root)) {
        if (entry.path().filename().native().starts_with(L"errors.log.1.")) { archives.push_back(entry.path()); }
    }
    REQUIRE(archives.size() == 1);
    CHECK(read(archives.front()) == first);

    const auto expired = fs::file_time_type::clock::now() - std::chrono::hours(49);
    fs::last_write_time(archives.front(), expired);
    fs::last_write_time(backup, expired);
    REQUIRE(append(log, native_error(L"C:\\fourth-" + long_name, ERROR_NOT_READY)));
    CHECK_FALSE(fs::exists(archives.front()));
    CHECK(read(backup) == third);
    const auto fourth = read(path);
    CHECK(fourth.find("fourth-") != std::string::npos);
    CHECK(std::count(fourth.begin(), fourth.end(), '\n') == 1);
}

TEST_CASE("Filesystem journal retries unavailable destinations without losing existing records", "[filesystem-error-log]") {
    Fixture fixture;
    const auto blocker = fixture.root / L"unavailable";
    { std::ofstream output(blocker); output << "not a directory"; }
    const auto path = blocker / L"errors.log";
    launcher::FilesystemErrorLog log(path);
    const auto diagnostic = native_error(L"C:\\retry.exe", ERROR_NOT_READY);
    CHECK_FALSE(append(log, diagnostic));
    REQUIRE(fs::remove(blocker));
    REQUIRE(append(log, diagnostic));
    const auto first = read(path);
    NativeHandle lock(CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    REQUIRE(lock.get() != INVALID_HANDLE_VALUE);
    CHECK_FALSE(append(log, diagnostic));
    lock.close();
    CHECK(read(path) == first);
    REQUIRE(append(log, diagnostic));
    CHECK(occurrences(read(path), "\"timestamp\":") == 2);

    launcher::FilesystemErrorLog rotating(path, 1);
    auto backup = path;
    backup += L".1";
    REQUIRE(fs::create_directory(backup));
    { std::ofstream output(backup / L"keep.txt"); output << "keep"; }
    const auto before_rotation = read(path);
    CHECK_FALSE(append(rotating, diagnostic));
    CHECK(read(path) == before_rotation);
    REQUIRE(fs::remove(backup / L"keep.txt"));
    REQUIRE(fs::remove(backup));
    REQUIRE(append(rotating, diagnostic));
    CHECK(read(backup) == before_rotation);
    CHECK(occurrences(read(path), "\"timestamp\":") == 1);
}

TEST_CASE("Filesystem journal serializes shared worker records", "[filesystem-error-log]") {
    Fixture fixture;
    const auto path = fixture.root / L"errors.log";
    launcher::FilesystemErrorLog log(path);
    std::atomic<bool> succeeded = true;
    std::vector<std::jthread> workers;
    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&, worker] {
            for (int record = 0; record < 16; ++record) {
                if (!append(log, native_error(L"C:\\worker-" + std::to_wstring(worker) + L"-record-" +
                    std::to_wstring(record) + L".exe", ERROR_ACCESS_DENIED))) { succeeded = false; }
            }
        });
    }
    workers.clear();
    REQUIRE(succeeded.load());
    const auto text = read(path);
    CHECK(std::count(text.begin(), text.end(), '\n') == 128);
    for (int worker = 0; worker < 8; ++worker) {
        for (int record = 0; record < 16; ++record) {
            CHECK(occurrences(text, "worker-" + std::to_string(worker) + "-record-" + std::to_string(record) + ".exe") == 1);
        }
    }
}

TEST_CASE("Filesystem journal rejects ambiguous configuration without touching disk", "[filesystem-error-log]") {
    Fixture fixture;
    CHECK_THROWS_AS(launcher::FilesystemErrorLog(fs::path(L"relative-errors.log")), std::invalid_argument);
    CHECK_THROWS_AS(launcher::FilesystemErrorLog(fixture.root / L"errors.log", 0), std::invalid_argument);
    CHECK(fs::is_empty(fixture.root));
}

TEST_CASE("Filesystem journal never appends through another search root", "[filesystem-error-log]") {
    Fixture fixture;
    const auto program = fixture.root / L"portable";
    const auto user = fixture.root / L"user";
    REQUIRE(fs::create_directory(program));
    REQUIRE(fs::create_directory(user));
    for (const auto& directory : {program, user}) {
        std::ofstream output(directory / L"errors.log", std::ios::binary);
        output << "preserve this journal";
    }
    const launcher::FileLookup files(program / L"Launcher.exe", user);
    const auto destination = fixture.root / L"chosen" / L"errors.log";
    launcher::FilesystemErrorLog log(destination, 256 * 1024, files);
    CHECK_FALSE(fs::exists(destination.parent_path()));
    REQUIRE(append(log, native_error(L"C:\\exact.exe", ERROR_READ_FAULT)));
    CHECK(read(destination).find("exact.exe") != std::string::npos);
    CHECK(read(program / L"errors.log") == "preserve this journal");
    CHECK(read(user / L"errors.log") == "preserve this journal");
}
