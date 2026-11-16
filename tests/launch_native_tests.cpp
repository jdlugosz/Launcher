#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/launch.hpp"
#include "launcher/target_version_scan.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

std::string utf8(std::wstring_view source) {
    if (source.empty()) {
        return {};
    }
    const int length = static_cast<int>(source.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        source.data(), length, nullptr, 0, nullptr, nullptr);
    if (size == 0) {
        throw std::runtime_error("Cannot convert test Unicode string");
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source.data(), length,
            result.data(), size, nullptr, nullptr) == 0) {
        throw std::runtime_error("Cannot convert test Unicode string");
    }
    return result;
}

fs::path test_executable_directory() {
    std::wstring value(32768, L'\0');
    const DWORD written = GetModuleFileNameW(nullptr, value.data(),
        static_cast<DWORD>(value.size()));
    if (written == 0 || written >= value.size()) {
        throw std::runtime_error("Cannot find test executable");
    }
    value.resize(written);
    return fs::path(value).parent_path();
}

class Fixture final {
public:
    Fixture() : parent_(fs::weakly_canonical(fs::temp_directory_path())) {
        static std::atomic<unsigned> sequence = 0;
        root_ = parent_ / (L"LauncherLaunchTests-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence++));
        if (!fs::create_directory(root_)) {
            throw std::runtime_error("Cannot create launch test directory");
        }
        fs::create_directory(root_ / L"Probe folder 资料");
        fs::create_directory(root_ / L"Working folder 资料");
    }
    ~Fixture() {
        std::error_code error;
        const auto resolved = fs::weakly_canonical(root_, error);
        if (!error && resolved == root_ && resolved.parent_path() == parent_) {
            fs::remove_all(root_, error);
        }
    }
    const fs::path& directory() const { return root_; }
    fs::path path(std::wstring_view name) const { return (root_ / name).lexically_normal(); }
    fs::path file(std::wstring_view name) const {
        const auto destination = path(name);
        fs::create_directories(destination.parent_path());
        std::ofstream output(destination, std::ios::binary);
        output << "A target-version fixture without executable version resources.";
        output.close();
        if (!output) { throw std::runtime_error("Cannot create launch target-version fixture"); }
        return destination;
    }
    fs::path copy_probe(bool console) const {
        const auto source = test_executable_directory() / (console ?
            L"LauncherLaunchConsoleProbe.exe" : L"LauncherLaunchGuiProbe.exe");
        const auto destination = path(console ?
            L"Probe folder 资料/Console probe.exe" : L"Probe folder 资料/Gui probe.exe");
        if (!fs::copy_file(source, destination)) {
            throw std::runtime_error("Cannot copy launch test helper");
        }
        return destination;
    }
private:
    fs::path parent_;
    fs::path root_;
};

void resource_word(std::vector<BYTE>& bytes, WORD value) {
    bytes.push_back(static_cast<BYTE>(value));
    bytes.push_back(static_cast<BYTE>(value >> 8));
}

void resource_alignment(std::vector<BYTE>& bytes) { while (bytes.size() % 4) { bytes.push_back(0); } }

std::vector<BYTE> version_block(std::wstring_view key, WORD type, std::span<const BYTE> value, WORD value_length,
        std::initializer_list<std::vector<BYTE>> children = {}) {
    std::vector<BYTE> bytes;
    resource_word(bytes, 0); // Length is filled after its children are appended.
    resource_word(bytes, value_length);
    resource_word(bytes, type);
    for (const auto unit : key) { resource_word(bytes, static_cast<WORD>(unit)); }
    resource_word(bytes, 0);
    resource_alignment(bytes);
    bytes.insert(bytes.end(), value.begin(), value.end());
    for (const auto& child : children) {
        resource_alignment(bytes);
        bytes.insert(bytes.end(), child.begin(), child.end());
    }
    if (bytes.size() > 65'535) { throw std::runtime_error("Owned version block exceeds its WORD length"); }
    bytes[0] = static_cast<BYTE>(bytes.size());
    bytes[1] = static_cast<BYTE>(bytes.size() >> 8);
    return bytes;
}

std::vector<BYTE> label_resource(std::wstring_view description, unsigned translation_count) {
    std::vector<BYTE> text;
    for (const auto unit : description) { resource_word(text, static_cast<WORD>(unit)); }
    resource_word(text, 0);
    const auto field = version_block(L"FileDescription", 1, text, static_cast<WORD>(description.size() + 1));
    const auto table = version_block(L"040904b0", 1, {}, 0, {field});
    const auto strings = version_block(L"StringFileInfo", 1, {}, 0, {table});
    std::vector<BYTE> translations;
    for (unsigned index = 0; index < translation_count; ++index) {
        resource_word(translations, 0x0409);
        resource_word(translations, 0x04b0);
    }
    const auto translation = version_block(L"Translation", 0, translations, static_cast<WORD>(translations.size()));
    const auto variables = version_block(L"VarFileInfo", 1, {}, 0, {translation});
    VS_FIXEDFILEINFO fixed{};
    fixed.dwSignature = 0xfeef04bd;
    fixed.dwStrucVersion = 0x00010000;
    fixed.dwFileVersionMS = fixed.dwProductVersionMS = 0x00010000;
    fixed.dwFileFlagsMask = VS_FFI_FILEFLAGSMASK;
    fixed.dwFileOS = VOS_NT_WINDOWS32;
    fixed.dwFileType = VFT_APP;
    std::vector<BYTE> fixed_bytes(sizeof(fixed));
    std::memcpy(fixed_bytes.data(), &fixed, sizeof(fixed));
    return version_block(L"VS_VERSION_INFO", 0, fixed_bytes, static_cast<WORD>(sizeof(fixed)), {strings, variables});
}

void install_label_resource(const fs::path& path, std::vector<BYTE>& bytes) {
    const HANDLE update = BeginUpdateResourceW(path.c_str(), TRUE);
    if (!update) { throw std::runtime_error("Cannot open owned label fixture resources"); }
    if (!UpdateResourceW(update, RT_VERSION, MAKEINTRESOURCEW(1), 0x0409, bytes.data(), static_cast<DWORD>(bytes.size()))) {
        EndUpdateResourceW(update, TRUE);
        throw std::runtime_error("Cannot write owned label fixture version resource");
    }
    if (!EndUpdateResourceW(update, FALSE)) { throw std::runtime_error("Cannot finish owned label fixture resources"); }
}

class TemporaryEnvironment final {
public:
    TemporaryEnvironment(std::wstring name, const std::wstring& value) : name_(std::move(name)) {
        SetLastError(ERROR_SUCCESS);
        const DWORD count = GetEnvironmentVariableW(name_.c_str(), nullptr, 0);
        if (count != 0) {
            previous_.resize(count);
            const DWORD written = GetEnvironmentVariableW(name_.c_str(), previous_.data(), count);
            if (written >= count) {
                throw std::runtime_error("Test environment changed while saving it");
            }
            previous_.resize(written);
            existed_ = true;
        } else if (GetLastError() == ERROR_SUCCESS) {
            existed_ = true;
        }
        if (!SetEnvironmentVariableW(name_.c_str(), value.c_str())) {
            throw std::runtime_error("Cannot set test environment variable");
        }
    }
    ~TemporaryEnvironment() {
        SetEnvironmentVariableW(name_.c_str(), existed_ ? previous_.c_str() : nullptr);
    }
private:
    std::wstring name_;
    std::wstring previous_;
    bool existed_ = false;
};

class AppPathRegistration final {
public:
    explicit AppPathRegistration(std::wstring executable) : executable_(std::move(executable)),
        path_(L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + executable_) {
        DWORD disposition = 0;
        const LSTATUS created = RegCreateKeyExW(HKEY_CURRENT_USER, path_.c_str(), 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key_, &disposition);
        if (created != ERROR_SUCCESS) {
            throw std::runtime_error("Cannot create test App Paths registration");
        }
        if (disposition != REG_CREATED_NEW_KEY) {
            RegCloseKey(key_);
            key_ = nullptr;
            throw std::runtime_error("Refusing to modify an existing App Paths registration");
        }
        owned_ = true;
    }
    ~AppPathRegistration() {
        if (key_ != nullptr) {
            RegCloseKey(key_);
        }
        if (owned_) {
            // Delete only the newly created leaf; never enumerate or recurse.
            RegDeleteKeyExW(HKEY_CURRENT_USER, path_.c_str(), 0, 0);
        }
    }
    const std::wstring& executable() const { return executable_; }
    void target(const std::wstring& value, DWORD type) const {
        if (RegSetValueExW(key_, nullptr, 0, type, reinterpret_cast<const BYTE*>(value.c_str()),
                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
            throw std::runtime_error("Cannot write owned App Paths test value");
        }
    }
    void path_environment(const std::wstring& value) const {
        if (RegSetValueExW(key_, L"Path", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
            throw std::runtime_error("Cannot write owned App Paths test PATH");
        }
    }
private:
    std::wstring executable_;
    std::wstring path_;
    HKEY key_ = nullptr;
    bool owned_ = false;
};

struct Receipt {
    DWORD process = 0;
    DWORD startup_flags = 0;
    DWORD show = 0;
    bool console_attached = false;
    bool console_visible = false;
    fs::path directory;
    std::vector<std::wstring> arguments;
    std::wstring path_environment;
    fs::path executable;
};

std::uint32_t number(std::ifstream& input) {
    std::uint32_t value = 0;
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) {
        throw std::runtime_error("Probe receipt is incomplete");
    }
    return value;
}

std::wstring text(std::ifstream& input) {
    const auto size = number(input);
    if (size > 32768) {
        throw std::runtime_error("Probe receipt string is too long");
    }
    std::wstring value(size, L'\0');
    input.read(reinterpret_cast<char*>(value.data()),
        static_cast<std::streamsize>(value.size() * sizeof(wchar_t)));
    if (!input) {
        throw std::runtime_error("Probe receipt is incomplete");
    }
    return value;
}

Receipt read_receipt(const fs::path& path) {
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        try {
            std::ifstream input(path, std::ios::binary);
            if (input && number(input) == 0x4c41554e) {
                Receipt result;
                result.process = number(input);
                result.startup_flags = number(input);
                result.show = number(input);
                result.console_attached = number(input) != 0;
                result.console_visible = number(input) != 0;
                result.directory = text(input);
                const auto count = number(input);
                if (count > 64) {
                    throw std::runtime_error("Too many probe receipt arguments");
                }
                for (std::uint32_t index = 0; index < count; ++index) {
                    result.arguments.push_back(text(input));
                }
                result.path_environment = text(input);
                result.executable = text(input);
                // Waiting belongs only to the test driver, never the launcher.
                if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, result.process)) {
                    const DWORD wait = WaitForSingleObject(process, 5000);
                    CloseHandle(process);
                    if (wait != WAIT_OBJECT_0) {
                        throw std::runtime_error("Launch probe did not exit");
                    }
                }
                return result;
            }
        } catch (const std::runtime_error&) {
            // A probe may still be publishing its short receipt.
        }
        Sleep(10);
    }
    throw std::runtime_error("Timed out waiting for launch probe receipt");
}

std::string quoted(const fs::path& path) {
    return "\"" + utf8(path.wstring()) + "\"";
}

bool path_has_directory(std::wstring_view path, std::wstring_view expected) noexcept {
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const auto separator = path.find(L';', begin);
        const auto end = separator == std::wstring_view::npos ? path.size() : separator;
        auto directory = path.substr(begin, end - begin);
        const auto first = directory.find_first_not_of(L" \t");
        if (first != std::wstring_view::npos) {
            const auto last = directory.find_last_not_of(L" \t");
            directory = directory.substr(first, last - first + 1);
            if (directory.size() >= 2 && directory.front() == L'"' && directory.back() == L'"') {
                directory.remove_prefix(1);
                directory.remove_suffix(1);
            }
            if (directory.size() == expected.size() && CompareStringOrdinal(directory.data(),
                    static_cast<int>(directory.size()), expected.data(),
                    static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL) {
                return true;
            }
        }
        if (separator == std::wstring_view::npos) {
            break;
        }
        begin = separator + 1;
    }
    return false;
}

void launch_windows_baseline(const std::wstring& executable,
    const fs::path& directory, const fs::path& receipt) {
    const std::wstring arguments = L"\"" + receipt.wstring() + L"\"";
    const HRESULT initialized = CoInitializeEx(nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    REQUIRE(SUCCEEDED(initialized));
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    execute.lpVerb = L"open";
    execute.lpFile = executable.c_str();
    execute.lpParameters = arguments.c_str();
    execute.lpDirectory = directory.c_str();
    execute.nShow = SW_SHOWNORMAL;
    const BOOL started = ShellExecuteExW(&execute);
    if (execute.hProcess != nullptr) {
        CloseHandle(execute.hProcess);
    }
    CoUninitialize();
    REQUIRE(started != FALSE);
}

void shortcut(const fs::path& link, const fs::path& target,
    const fs::path& directory, const std::wstring& arguments) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    REQUIRE(SUCCEEDED(initialized));
    {
        ComPtr<IShellLinkW> shell_link;
        REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(shell_link.GetAddressOf()))));
        REQUIRE(SUCCEEDED(shell_link->SetPath(target.c_str())));
        REQUIRE(SUCCEEDED(shell_link->SetWorkingDirectory(directory.c_str())));
        REQUIRE(SUCCEEDED(shell_link->SetArguments(arguments.c_str())));
        ComPtr<IPersistFile> file;
        REQUIRE(SUCCEEDED(shell_link.As(&file)));
        REQUIRE(SUCCEEDED(file->Save(link.c_str(), TRUE)));
    }
    CoUninitialize();
}

} // namespace

TEST_CASE("Shell targets resolve relative to the selected configuration", "[launch][native]") {
    const Fixture fixture;
    launcher::LaunchAction action;
    action.target = "Probe folder 资料/Gui probe.exe";
    action.arguments = "\"a b\" & %literal%";
    auto prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    REQUIRE(prepared.action);
    CHECK(prepared.action->target == fixture.path(L"Probe folder 资料/Gui probe.exe").wstring());
    CHECK(prepared.action->working_directory == fixture.path(L"Probe folder 资料").wstring());
    CHECK(prepared.action->arguments == L"\"a b\" & %literal%");
    action.working_directory = "Working folder 资料";
    prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    CHECK(prepared.action->working_directory == fixture.path(L"Working folder 资料").wstring());

    action.working_directory.clear();
    for (const std::string target : {"https://example.invalid/test", "mailto:test@example.invalid",
            "shell:AppsFolder", "::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"}) {
        action.target = target;
        prepared = launcher::prepare_launch_action(action, fixture.directory());
        REQUIRE(prepared.result.success);
        CHECK(utf8(prepared.action->target) == target);
        CHECK(prepared.action->working_directory == fixture.directory().wstring());
    }
    const auto missing = "LauncherUnresolved-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    for (const auto& target : {missing, missing + ".exe"}) {
        action.target = target;
        const auto unresolved = launcher::prepare_launch_action(action, fixture.directory());
        CHECK_FALSE(unresolved.result.success);
        CHECK(unresolved.result.error_code == ERROR_FILE_NOT_FOUND);
        CHECK_FALSE(unresolved.action);
    }
    action.target = "Utility.lnk";
    prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    CHECK(prepared.action->target == fixture.path(L"Utility.lnk").wstring());
    CHECK(prepared.action->working_directory.empty());
}

TEST_CASE("Shell launch preparation validates text and preserves handler boundaries", "[launch][native]") {
    const Fixture fixture;
    launcher::LaunchAction action;
    action.target = "Example.exe";
    action.handler = "adobe";
    CHECK(launcher::prepare_launch_action(action, fixture.directory()).result.error_code == ERROR_NOT_SUPPORTED);
    action.handler = "shell";
    action.target = std::string("abc\0def", 7);
    CHECK(launcher::prepare_launch_action(action, fixture.directory()).result.error_code == ERROR_INVALID_PARAMETER);
    action.target = std::string("\xff", 1);
    CHECK(launcher::prepare_launch_action(action, fixture.directory()).result.error_code == ERROR_NO_UNICODE_TRANSLATION);
    action.target = "C:relative.exe";
    CHECK(launcher::prepare_launch_action(action, fixture.directory()).result.error_code == ERROR_BAD_PATHNAME);
    action.target = "Example.exe";
    action.arguments = std::string("abc\0def", 7);
    CHECK(launcher::prepare_launch_action(action, fixture.directory()).result.error_code == ERROR_INVALID_PARAMETER);
}

TEST_CASE("Wildcard shell preparation selects the newest target and preserves launch options", "[launch][native][target-versions]") {
    const Fixture fixture;
    fixture.file(L"Tools 5.9/Tool.exe");
    const auto newest = fixture.file(L"Tools 5.10/Tool.exe");
    fixture.file(L"Tools 99.0/Unrelated.exe");
    launcher::LaunchAction action;
    action.target = "Tools */Tool.exe";
    action.arguments = "\"document with spaces\" --literal=%WINDIR%";
    action.working_directory = "Working folder 资料";
    action.show = launcher::LaunchShow::minimized;
    const auto pattern = launcher::version_target_pattern(action, fixture.directory());
    REQUIRE(pattern);
    CHECK(*pattern == fixture.path(L"Tools */Tool.exe"));
    auto prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    REQUIRE(prepared.action);
    CHECK(prepared.action->target == newest.wstring());
    CHECK(prepared.action->arguments == L"\"document with spaces\" --literal=%WINDIR%");
    CHECK(prepared.action->working_directory == fixture.path(L"Working folder 资料").wstring());
    CHECK(prepared.action->show == launcher::LaunchShow::minimized);

    action.working_directory.clear();
    prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    REQUIRE(prepared.action);
    CHECK(prepared.action->working_directory == newest.parent_path().wstring());

    fixture.file(L"Standalone 5.9.pdf");
    const auto standalone = fixture.file(L"Standalone 5.10.pdf");
    action.target = "Standalone *.pdf";
    prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    REQUIRE(prepared.action);
    CHECK(prepared.action->target == standalone.wstring());
}

TEST_CASE("Wildcard shortcuts retain the selected shortcut arguments and Start in", "[launch][native][target-versions]") {
    const Fixture fixture;
    const auto probe = fixture.copy_probe(false);
    const auto old_link = fixture.path(L"Version 5.9/Utility.lnk");
    const auto newest_link = fixture.path(L"Version 5.10/Utility.lnk");
    REQUIRE(fs::create_directory(old_link.parent_path()));
    REQUIRE(fs::create_directory(newest_link.parent_path()));
    const auto receipt = fixture.path(L"Newest shortcut receipt.bin");
    const auto working = fixture.path(L"Working folder 资料");
    shortcut(old_link, probe, probe.parent_path(), L"\"" + fixture.path(L"Wrong shortcut receipt.bin").wstring() + L"\"");
    shortcut(newest_link, probe, working, L"\"" + receipt.wstring() + L"\" \"newest stored argument\"");
    launcher::LaunchAction action;
    action.target = "Version */Utility.lnk";
    const auto prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    REQUIRE(prepared.action);
    CHECK(prepared.action->target == newest_link.wstring());
    CHECK(prepared.action->working_directory.empty());
    const auto launched = launcher::launch_action(action, fixture.directory());
    INFO(utf8(launched.message));
    REQUIRE(launched.success);
    const auto report = read_receipt(receipt);
    CHECK(report.directory == working);
    REQUIRE(report.arguments.size() == 3);
    CHECK(report.arguments[2] == L"newest stored argument");
}

TEST_CASE("Wildcard paths expand macros and environment once and preserve literal percent choices", "[launch][native][target-versions][percent]") {
    const Fixture fixture;
    const auto root = fixture.path(L"Versions %LAUNCHER_VERSION_WORD%");
    const auto old = fixture.file(L"Versions %LAUNCHER_VERSION_WORD%/App 5.9/%LAUNCHER_VERSION_WORD%.pdf");
    const auto newest = fixture.file(L"Versions %LAUNCHER_VERSION_WORD%/App 5.10/%LAUNCHER_VERSION_WORD%.pdf");
    const TemporaryEnvironment root_variable(L"LAUNCHER_VERSION_ROOT", root.wstring());
    const TemporaryEnvironment word_variable(L"LAUNCHER_VERSION_WORD", L"must-not-replace-the-filename");
    const auto configuration = launcher::parse_configuration(R"([paths]
P = '%LAUNCHER_VERSION_ROOT%'
[[tabs]]
name = 'Versions'
items = [{target='$P/App */$/LAUNCHER_VERSION_WORD%.pdf'}]
)");
    const auto& action = *configuration.tabs[0].items[0].launch;
    const auto pattern = launcher::version_target_pattern(action, fixture.directory());
    REQUIRE(pattern);
    CHECK(*pattern == root / L"App *" / L"%LAUNCHER_VERSION_WORD%.pdf");
    const auto prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    REQUIRE(prepared.action);
    CHECK(prepared.action->target == newest.wstring());

    auto pinned = action;
    pinned.target = utf8(old.wstring());
    CHECK_FALSE(launcher::version_target_pattern(pinned, fixture.directory()));
    const auto older = launcher::prepare_launch_action(pinned, fixture.directory());
    REQUIRE(older.result.success);
    REQUIRE(older.action);
    CHECK(older.action->target == old.wstring());
}

TEST_CASE("Wildcard target classification preserves virtual targets and reports absent files", "[launch][native][target-versions]") {
    const Fixture fixture;
    launcher::LaunchAction action;
    for (const std::string target : {"https://example.invalid/search?query=*", "mailto:test@example.invalid?subject=*",
            "shell:AppsFolder", "::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"}) {
        action.target = target;
        CHECK_FALSE(launcher::version_target_pattern(action, fixture.directory()));
        const auto prepared = launcher::prepare_launch_action(action, fixture.directory());
        REQUIRE(prepared.result.success);
        REQUIRE(prepared.action);
        CHECK(utf8(prepared.action->target) == target);
    }
    action.handler = "adobeCC";
    action.target = "Anything *";
    CHECK_FALSE(launcher::version_target_pattern(action, fixture.directory()));
    action.handler = "shell";
    action.target = "Missing */Tool.exe";
    REQUIRE(launcher::version_target_pattern(action, fixture.directory()));
    const auto missing = launcher::prepare_launch_action(action, fixture.directory());
    CHECK_FALSE(missing.result.success);
    CHECK_FALSE(missing.action);
    CHECK(missing.result.error_code == ERROR_FILE_NOT_FOUND);
    CHECK_FALSE(missing.result.message.empty());
}

TEST_CASE("Shell launch uses target and working-directory environment expanded when loading configuration", "[launch][native][percent]") {
    const Fixture fixture;
    const std::wstring variable = L"LAUNCHER_TEST_DIR_" + std::to_wstring(GetCurrentProcessId());
    const auto configuration = [&] {
        const TemporaryEnvironment environment(variable, fixture.directory().wstring());
        return launcher::parse_configuration("[[tabs]]\nname='Test'\nitems=[{target='%" + utf8(variable) +
            "%/Probe folder 资料/Gui probe.exe',working_directory='%" + utf8(variable) + "%/Working folder 资料'}]\n");
    }();
    const auto& action = *configuration.tabs[0].items[0].launch;
    CHECK(action.target == utf8(fixture.directory().wstring()) + "/Probe folder 资料/Gui probe.exe");
    CHECK(action.working_directory == utf8(fixture.directory().wstring()) + "/Working folder 资料");
    const auto prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    REQUIRE(prepared.action);
    CHECK(prepared.action->target == fixture.path(L"Probe folder 资料/Gui probe.exe").wstring());
    CHECK(prepared.action->working_directory == fixture.path(L"Working folder 资料").wstring());
}

TEST_CASE("Environment expansion precedes dollar-symbol decoding on the complete substituted text", "[config][paths][expansion-order]") {
    const TemporaryEnvironment before(L"LAUNCHER_ORDER_$$", L"before-dollar-decoding");
    const TemporaryEnvironment after(L"LAUNCHER_ORDER_$", L"wrong-order");
    const auto config = launcher::parse_configuration(R"([paths]
P = '%LAUNCHER_ORDER_'
[[tabs]]
name = 'Order'
items = [
 { target = '$P$$%\$/WINDIR%\$$/', icon = {file='%LAUNCHER_ORDER_$$%\$/WINDIR%.dll', index=0}, working_directory='%LAUNCHER_ORDER_$$%\$/WINDIR%', arguments='--raw %WINDIR% $$/' },
]
)");
    const auto& item = config.tabs[0].items[0];
    REQUIRE(item.launch);
    CHECK(item.launch->target == R"(before-dollar-decoding\%WINDIR%\$/)");
    CHECK(item.launch->working_directory == R"(before-dollar-decoding\%WINDIR%)");
    CHECK(item.launch->arguments == "--raw %WINDIR% $/");
    const auto& icon = std::get<launcher::IconGroup>(item.icon_source);
    CHECK(icon.path == fs::path(L"before-dollar-decoding\\%WINDIR%.dll"));
    const auto plain = launcher::parse_configuration("[[tabs]]\nname='Order'\nitems=[{target='%LAUNCHER_ORDER_$$%'}]\n");
    CHECK(plain.tabs[0].items[0].launch->target == "before-dollar-decoding");
}

TEST_CASE("Environment replacements reach the final symbol pass without recursive expansion", "[config][paths][expansion-order]") {
    const TemporaryEnvironment nested(L"LAUNCHER_ORDER_NESTED", L"must-not-expand");
    const TemporaryEnvironment text(L"LAUNCHER_ORDER_TEXT", LR"(%LAUNCHER_ORDER_NESTED%\$/WINDIR%\$$/\$P)");
    const auto config = launcher::parse_configuration(R"([paths]
P = 'must-not-substitute'
[[tabs]]
name = 'Order'
items = [{target='%LAUNCHER_ORDER_TEXT%'}]
)");
    const auto& action = *config.tabs[0].items[0].launch;
    CHECK(action.target == R"(%LAUNCHER_ORDER_NESTED%\%WINDIR%\$/\$P)");
    const Fixture fixture;
    const auto prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    REQUIRE(prepared.action);
    CHECK(prepared.action->target == fixture.path(L"%LAUNCHER_ORDER_NESTED%\\%WINDIR%\\$/\\$P").wstring());
}

TEST_CASE("Literal percent targets survive macros environment expansion and Shell dispatch", "[launch][native][percent]") {
    const Fixture fixture;
    const auto root = fixture.path(L"资料 %LAUNCHER_PERCENT_WORD% root");
    const auto folder = root / L"预算$Cash$$";
    const auto working = root / L"Working %LAUNCHER_PERCENT_WORD%";
    REQUIRE(fs::create_directories(folder));
    REQUIRE(fs::create_directory(working));
    const auto probe = folder / L"%LAUNCHER_PERCENT_WORD% Gui.exe";
    REQUIRE(fs::copy_file(fixture.copy_probe(false), probe));
    const auto receipt = fixture.path(L"Literal percent receipt.bin");
    const TemporaryEnvironment root_variable(L"LAUNCHER_PERCENT_ROOT", root.wstring());
    const TemporaryEnvironment word_variable(L"LAUNCHER_PERCENT_WORD", L"must-not-replace-the-filename");
    const std::string source = R"([paths]
P = '%LAUNCHER_PERCENT_ROOT%\预算$Cash$$$$'
[[tabs]]
name = 'Test'
[[tabs.items]]
target = '$P\$/LAUNCHER_PERCENT_WORD% Gui.exe'
working_directory = '%LAUNCHER_PERCENT_ROOT%\Working $/LAUNCHER_PERCENT_WORD%'
show = 'hidden'
arguments = ')" + quoted(receipt) + R"( $/LAUNCHER_PERCENT_WORD$/ $$/'
)";
    const auto configuration = launcher::parse_configuration(source);
    const auto& action = *configuration.tabs[0].items[0].launch;
    const auto prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    REQUIRE(prepared.action);
    CHECK(prepared.action->target == probe.wstring());
    CHECK(prepared.action->working_directory == working.wstring());
    const auto launched = launcher::launch_action(action, fixture.directory());
    INFO(utf8(launched.message));
    REQUIRE(launched.success);
    const auto report = read_receipt(receipt);
    CHECK(report.directory == working);
    REQUIRE(report.arguments.size() == 4);
    CHECK(report.arguments[2] == L"%LAUNCHER_PERCENT_WORD%");
    CHECK(report.arguments[3] == L"$/");
}

TEST_CASE("Display labels read a renamed executable description through resolved paths", "[launch][native][display-label]") {
    const Fixture fixture;
    const auto probe = fixture.copy_probe(false);
    const auto receipt = fixture.path(L"Label lookup must not launch.bin");
    launcher::LaunchAction action;
    action.target = utf8(fs::relative(probe, fixture.directory()).wstring());
    action.arguments = quoted(receipt);
    action.working_directory = "Working folder 资料";
    action.show = launcher::LaunchShow::minimized;
    const auto prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    REQUIRE(prepared.action);
    const auto before = *prepared.action;
    CHECK(launcher::launch_display_label(*prepared.action) == L"Launcher");
    CHECK(prepared.action->target == before.target);
    CHECK(prepared.action->arguments == before.arguments);
    CHECK(prepared.action->working_directory == before.working_directory);
    CHECK(prepared.action->show == before.show);
    CHECK(prepared.action->shell_namespace == before.shell_namespace);
    CHECK_FALSE(fs::exists(receipt));

    const std::wstring variable = L"LAUNCHER_LABEL_DIR_" + std::to_wstring(GetCurrentProcessId());
    const TemporaryEnvironment environment(variable, fixture.directory().wstring());
    const auto configuration = launcher::parse_configuration("[[tabs]]\nname='Test'\nitems=[{target='%" + utf8(variable) +
        "%/Probe folder 资料/Gui probe.exe'}]\n");
    const auto expanded = launcher::prepare_launch_action(*configuration.tabs[0].items[0].launch, fixture.directory());
    REQUIRE(expanded.result.success);
    REQUIRE(expanded.action);
    CHECK(launcher::launch_display_label(*expanded.action) == L"Launcher");
    CHECK_FALSE(fs::exists(receipt));
}

TEST_CASE("Exact catalog preparation preserves launch options without requiring the file", "[launch][exact-target]") {
    const Fixture fixture;
    const auto nonexistent = fixture.path(L"Not installed 资料\\Selected %literal% program.exe");
    launcher::LaunchAction action;
    action.target = utf8(nonexistent.wstring());
    action.arguments = "\"literal arguments 资料\" %UNCHANGED%";
    action.show = launcher::LaunchShow::minimized;
    std::wstring expected_directory = nonexistent.parent_path().wstring();
    SECTION("An observed executable keeps its selected path and parent") {}
    SECTION("A configured relative working directory is resolved lexically beside the configuration") {
        action.working_directory = "Working folder 资料/../Selected working folder";
        expected_directory = fixture.path(L"Selected working folder").wstring();
    }
    SECTION("A shortcut keeps an omitted Start in directory empty") {
        action.target = utf8(fixture.path(L"Not installed 资料\\Selected shortcut.LNK").wstring());
        expected_directory.clear();
    }
    SECTION("A configured working directory retains literal percent text") {
        action.working_directory = "Working %UNCHANGED%";
        expected_directory = fixture.path(L"Working %UNCHANGED%").wstring();
    }
    const auto prepared = launcher::prepare_exact_target_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    REQUIRE(prepared.action);
    CHECK(utf8(prepared.action->target) == action.target);
    CHECK(prepared.action->arguments == L"\"literal arguments 资料\" %UNCHANGED%");
    CHECK(prepared.action->working_directory == expected_directory);
    CHECK(prepared.action->show == launcher::LaunchShow::minimized);
    CHECK_FALSE(prepared.action->shell_namespace);
    CHECK_FALSE(fs::exists(nonexistent));
}

TEST_CASE("Exact catalog preparation uses the configured directory without further substitution", "[launch][exact-target][percent]") {
    const Fixture fixture;
    const std::wstring outer = L"LAUNCHER_EXACT_OUTER_" + std::to_wstring(GetCurrentProcessId());
    const std::wstring inner = L"LAUNCHER_EXACT_INNER_" + std::to_wstring(GetCurrentProcessId());
    const auto expected_directory = fixture.path(L"Working %" + inner + L"%");
    const TemporaryEnvironment inner_value(inner, L"Must not expand twice");
    const TemporaryEnvironment outer_value(outer, expected_directory.wstring());
    const auto configuration = launcher::parse_configuration("[[tabs]]\nname='Test'\nitems=[{target='Missing.exe',working_directory='%" + utf8(outer) + "%'}]\n");
    auto action = *configuration.tabs[0].items[0].launch;
    action.target = utf8(fixture.path(L"Missing.exe").wstring());
    CHECK(action.working_directory == utf8(expected_directory.wstring()));
    const auto prepared = launcher::prepare_exact_target_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    REQUIRE(prepared.action);
    CHECK(prepared.action->working_directory == expected_directory.wstring());
}

TEST_CASE("Exact catalog preparation preserves extended and unavailable UNC paths lexically", "[launch][exact-target]") {
    const Fixture fixture;
    for (const auto target : {L"\\\\?\\C:\\Launcher exact fixture\\tool.exe",
            L"\\\\launcher-exact-must-not-be-contacted.invalid\\share\\tool.exe"}) {
        launcher::LaunchAction action;
        action.target = utf8(target);
        const auto prepared = launcher::prepare_exact_target_action(action, fixture.directory());
        REQUIRE(prepared.result.success);
        REQUIRE(prepared.action);
        CHECK(prepared.action->target == target);
    }
}

TEST_CASE("Exact catalog preparation rejects unresolved and malformed actions", "[launch][exact-target]") {
    const Fixture fixture;
    launcher::LaunchAction action;
    action.target = utf8(fixture.path(L"Missing.exe").wstring());
    auto directory = fixture.directory();
    SECTION("A version handler cannot reenter discovery") { action.handler = "adobeCC"; }
    SECTION("A wildcard is not a concrete file") { action.target = "C:\\Program *\\tool.exe"; }
    SECTION("A protocol is not a filesystem item") { action.target = "https://example.invalid/tool.exe"; }
    SECTION("A Shell namespace is not a filesystem item") { action.target = "shell:AppsFolder"; }
    SECTION("A relative target cannot trigger lookup") { action.target = "tool.exe"; }
    SECTION("A drive-relative target is ambiguous") { action.target = "C:tool.exe"; }
    SECTION("A target with NUL must not be truncated") { action.target.push_back('\0'); }
    SECTION("A target with invalid UTF-8 is invalid") { action.target = "\xc3\x28"; }
    SECTION("Arguments with invalid UTF-8 are invalid") { action.arguments = "\xc3\x28"; }
    SECTION("A working directory with NUL must not be truncated") { action.working_directory = std::string("work\0bad", 8); }
    SECTION("A show mode must be supported") { action.show = static_cast<launcher::LaunchShow>(99); }
    SECTION("Configuration directories must be absolute") { directory = L"relative configuration"; }
    const auto prepared = launcher::prepare_exact_target_action(action, directory);
    CHECK_FALSE(prepared.result.success);
    CHECK(prepared.result.error_code != 0);
    CHECK_FALSE(prepared.result.message.empty());
    CHECK_FALSE(prepared.action);
}

TEST_CASE("Properties validation rejects non-concrete targets without opening a program", "[launch][properties]") {
    for (const auto target : {L"", L"relative.exe", L"C:\\Program *\\tool.exe", L"https://example.invalid/tool.exe", L"shell:AppsFolder"}) {
        launcher::NativeLaunchAction action;
        action.target = target;
        const auto result = launcher::show_target_properties(action);
        CHECK_FALSE(result.success);
        CHECK(result.error_code == ERROR_INVALID_PARAMETER);
    }
    launcher::NativeLaunchAction action;
    action.target = L"C:\\No real property UI.exe";
    SECTION("An embedded NUL is rejected") { action.target.push_back(L'\0'); }
    SECTION("A namespace-marked action is not an exact filesystem item") { action.shell_namespace = true; }
    const auto result = launcher::show_target_properties(action);
    CHECK_FALSE(result.success);
    CHECK(result.error_code == ERROR_INVALID_PARAMETER);
}

TEST_CASE("Display labels use the pinned App Paths executable after registration changes", "[launch][native][display-label]") {
    const Fixture fixture;
    const auto probe = fixture.copy_probe(false);
    const std::wstring name = L"LauncherLabelProbe-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L".exe";
    const AppPathRegistration registration(name);
    registration.target(probe.wstring(), REG_SZ);
    launcher::LaunchAction action;
    action.target = utf8(name);
    const auto prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    REQUIRE(prepared.action);
    REQUIRE(prepared.action->target == probe.wstring());
    registration.target(fixture.path(L"Missing registration.exe").wstring(), REG_SZ);
    CHECK(launcher::launch_display_label(*prepared.action) == L"Launcher");
    CHECK(prepared.action->target == probe.wstring());
}

TEST_CASE("Display labels fall back to Unicode filenames when executable metadata is unavailable", "[launch][native][display-label]") {
    const Fixture fixture;
    const auto resource_free = fixture.path(L"Resource-free 资料.EXE");
    REQUIRE(fs::copy_file(test_executable_directory() / L"LauncherRestartProbe.exe", resource_free));
    const auto malformed = fixture.path(L"Malformed 资料.exe");
    {
        std::ofstream file(malformed, std::ios::binary);
        REQUIRE(file.good());
        file << "Not an executable resource.";
    }
    const auto document = fixture.path(L"参考 document.txt");
    {
        std::ofstream file(document, std::ios::binary);
        REQUIRE(file.good());
        file << "A document label uses its filename.";
    }
    for (const auto& path : {resource_free, malformed, document, fixture.path(L"Missing 资料.exe")}) {
        launcher::LaunchAction action;
        action.target = utf8(path.wstring());
        const auto prepared = launcher::prepare_launch_action(action, fixture.directory());
        REQUIRE(prepared.result.success);
        REQUIRE(prepared.action);
        CHECK(launcher::launch_display_label(*prepared.action) == path.filename().wstring());
    }
    launcher::NativeLaunchAction unresolved;
    unresolved.target = L"LauncherLaunchGuiProbe.exe";
    unresolved.working_directory = test_executable_directory().wstring();
    CHECK(launcher::launch_display_label(unresolved) == L"LauncherLaunchGuiProbe.exe");
}

TEST_CASE("Display labels retain protocol and Shell namespace targets literally", "[launch][native][display-label]") {
    const Fixture fixture;
    for (const std::string target : {"https://example.invalid/test.exe", "mailto:test@example.invalid",
            "shell:AppsFolder", "::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"}) {
        launcher::LaunchAction action;
        action.target = target;
        const auto prepared = launcher::prepare_launch_action(action, fixture.directory());
        REQUIRE(prepared.result.success);
        REQUIRE(prepared.action);
        CHECK(utf8(launcher::launch_display_label(*prepared.action)) == target);
    }
}

TEST_CASE("Display label resource text and translation budgets retain a usable filename fallback", "[launch][native][display-label][display-label-budget]") {
    const Fixture fixture;
    const auto probe = fixture.copy_probe(false);
    const launcher::TargetScanLimits limits;
    std::wstring description = L"Owned bounded description";
    unsigned translation_count = 1;
    bool limited = false;
    SECTION("An ordinary owned resource keeps its description") {}
    SECTION("A description exactly at the text bound remains complete") { description.assign(limits.max_metadata_units, L'D'); }
    SECTION("An oversized description is rejected before copying") {
        description.assign(limits.max_metadata_units + 1, L'D');
        limited = true;
    }
    SECTION("An excessive translation table cannot multiply optional label work") {
        translation_count = static_cast<unsigned>(limits.max_translations + 1);
        limited = true;
    }
    auto bytes = label_resource(description, translation_count);
    install_label_resource(probe, bytes);
    DWORD ignored = 0;
    const auto resource_bytes = GetFileVersionInfoSizeW(probe.c_str(), &ignored);
    REQUIRE(resource_bytes != 0);
    launcher::NativeLaunchAction action;
    action.target = probe.wstring();
    action.arguments = L"These fixtures must never execute.";
    action.working_directory = fixture.directory().wstring();
    action.show = launcher::LaunchShow::minimized;
    const auto before = action;
    CHECK(launcher::launch_display_label(action) == (limited ? probe.filename().wstring() : description));
    CHECK(action.target == before.target);
    CHECK(action.arguments == before.arguments);
    CHECK(action.working_directory == before.working_directory);
    CHECK(action.show == before.show);
}

TEST_CASE("Launching a GUI executable delivers literal arguments and the configured directory", "[launch][native]") {
    const Fixture fixture;
    const auto probe = fixture.copy_probe(false);
    const auto receipt = fixture.path(L"GUI receipt 资料.bin");
    launcher::LaunchAction action;
    action.target = utf8(fs::relative(probe, fixture.directory()).wstring());
    action.working_directory = "Working folder 资料";
    action.arguments = quoted(receipt) + " \"two words\" \"资料 & reference\" \"\" %literal%";
    const auto result = launcher::launch_action(action, fixture.directory());
    INFO(utf8(result.message));
    REQUIRE(result.success);
    const auto report = read_receipt(receipt);
    CHECK(report.directory == fixture.path(L"Working folder 资料"));
    REQUIRE(report.arguments.size() == 6);
    CHECK(report.arguments[2] == L"two words");
    CHECK(report.arguments[3] == L"资料 & reference");
    CHECK(report.arguments[4].empty());
    CHECK(report.arguments[5] == L"%literal%");
    CHECK_FALSE(report.console_attached);
}

TEST_CASE("Shift target configuration launches its own executable with shared options", "[launch][native][shift-target]") {
    const Fixture fixture;
    const auto probe = fixture.copy_probe(false);
    const auto receipt = fixture.path(L"Alternate receipt 资料.bin");
    const auto configuration = launcher::parse_configuration("[[tabs]]\nname='Alternate'\nitems=[{"
        "target='Missing normal.exe', shift_target='" + utf8(fs::relative(probe, fixture.directory()).wstring()) +
        "', working_directory='Working folder 资料', arguments='" + quoted(receipt) + " \"alternate 资料\"'}]\n");
    const auto& item = configuration.tabs[0].items[0];
    REQUIRE(item.shift_launch);
    const auto prepared = launcher::prepare_launch_action(*item.shift_launch, fixture.directory());
    REQUIRE(prepared.result.success);
    REQUIRE(prepared.action);
    CHECK(prepared.action->target == probe.wstring());
    const auto result = launcher::launch_prepared_action(*prepared.action, 0);
    INFO(utf8(result.message));
    REQUIRE(result.success);
    const auto report = read_receipt(receipt);
    CHECK(report.directory == fixture.path(L"Working folder 资料"));
    REQUIRE(report.arguments.size() == 3);
    CHECK(report.arguments[2] == L"alternate 资料");
}

TEST_CASE("Explicit Shell verbs preserve launch data and reject invalid text", "[launch][native]") {
    const Fixture fixture;
    const auto probe = fixture.copy_probe(false);
    const auto receipt = fixture.path(L"Explicit verb receipt 资料.bin");
    launcher::LaunchAction action;
    action.target = utf8(fs::relative(probe, fixture.directory()).wstring());
    action.working_directory = "Working folder 资料";
    action.arguments = quoted(receipt) + " \"two words\" \"资料 & reference\"";
    const auto prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    REQUIRE(prepared.action);
    CHECK(launcher::can_run_as_administrator(*prepared.action));

    const auto result = launcher::launch_action(action, fixture.directory(), 0, "open");
    INFO(utf8(result.message));
    REQUIRE(result.success);
    const auto report = read_receipt(receipt);
    CHECK(report.directory == fixture.path(L"Working folder 资料"));
    REQUIRE(report.arguments.size() == 4);
    CHECK(report.arguments[2] == L"two words");
    CHECK(report.arguments[3] == L"资料 & reference");
    CHECK_FALSE(report.console_attached);

    const auto invalid_receipt = fixture.path(L"Invalid verb must not launch.bin");
    action.arguments = quoted(invalid_receipt);
    const auto invalid = launcher::launch_action(action, fixture.directory(), 0,
        std::string_view("open\0runas", 10));
    CHECK_FALSE(invalid.success);
    CHECK(invalid.error_code == ERROR_INVALID_PARAMETER);
    CHECK_FALSE(fs::exists(invalid_receipt));
    CHECK(launcher::launch_action(action, fixture.directory(), 0,
        std::string_view("\xff", 1)).error_code == ERROR_NO_UNICODE_TRANSLATION);

    action.target = "https://example.invalid/test";
    const auto protocol = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(protocol.result.success);
    REQUIRE(protocol.action);
    CHECK_FALSE(launcher::can_run_as_administrator(*protocol.action));
    CHECK(launcher::reveal_launch_target(*protocol.action).error_code == ERROR_NOT_SUPPORTED);
    action.target = utf8(fixture.path(L"Missing probe.exe").wstring());
    const auto missing = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(missing.result.success);
    REQUIRE(missing.action);
    CHECK_FALSE(launcher::can_run_as_administrator(*missing.action));
    CHECK_FALSE(launcher::reveal_launch_target(*missing.action).success);
}

TEST_CASE("Console launches create a console and apply explicit show modes", "[launch][native]") {
    const Fixture fixture;
    const auto probe = fixture.copy_probe(true);
    for (const auto show : {launcher::LaunchShow::normal, launcher::LaunchShow::hidden,
            launcher::LaunchShow::minimized}) {
        const auto receipt = fixture.path(L"Console receipt " +
            std::to_wstring(static_cast<int>(show)) + L".bin");
        launcher::LaunchAction action;
        action.target = utf8(probe.wstring());
        action.arguments = quoted(receipt);
        action.show = show;
        const auto result = launcher::launch_action(action, fixture.directory());
        INFO(utf8(result.message));
        REQUIRE(result.success);
        const auto report = read_receipt(receipt);
        CHECK(report.console_attached);
        CHECK(report.directory == probe.parent_path());
        CHECK((report.startup_flags & STARTF_USESHOWWINDOW) != 0);
        CHECK(report.show == static_cast<DWORD>(show == launcher::LaunchShow::normal ? SW_SHOWNORMAL :
            show == launcher::LaunchShow::hidden ? SW_HIDE : SW_SHOWMINIMIZED));
        // Windows Terminal can expose a nonvisible proxy HWND even for a visible
        // console. Console attachment and the requested startup mode are stable.
        if (show == launcher::LaunchShow::hidden) {
            CHECK_FALSE(report.console_visible);
        }
    }
}

TEST_CASE("Launching a shortcut preserves its stored arguments and Start in", "[launch][native]") {
    const Fixture fixture;
    const auto probe = fixture.copy_probe(false);
    const auto receipt = fixture.path(L"Shortcut receipt 资料.bin");
    const auto link = fixture.path(L"Probe shortcut 资料.lnk");
    const auto working = fixture.path(L"Working folder 资料");
    const std::wstring arguments = L"\"" + receipt.wstring() + L"\" \"stored link argument\"";
    shortcut(link, probe, working, arguments);
    launcher::LaunchAction action;
    action.target = utf8(link.filename().wstring());
    const auto result = launcher::launch_action(action, fixture.directory());
    INFO(utf8(result.message));
    REQUIRE(result.success);
    const auto report = read_receipt(receipt);
    CHECK(report.directory == working);
    REQUIRE(report.arguments.size() == 3);
    CHECK(report.arguments[2] == L"stored link argument");
}

TEST_CASE("Shell launch failures return a code and message without invoking another handler", "[launch][native]") {
    const Fixture fixture;
    launcher::LaunchAction action;
    action.target = utf8(fixture.path(L"Missing own probe.exe").wstring());
    auto result = launcher::launch_action(action, fixture.directory());
    CHECK_FALSE(result.success);
    CHECK(result.error_code != 0);
    CHECK_FALSE(result.message.empty());
    action.handler = "future-special-handler";
    result = launcher::launch_action(action, fixture.directory());
    CHECK_FALSE(result.success);
    CHECK(result.error_code == ERROR_NOT_SUPPORTED);
}

TEST_CASE("Bare executable icons use the configured working directory before App Paths", "[launch][native]") {
    const Fixture fixture;
    const auto registered_probe = fixture.copy_probe(false);
    const std::wstring name = L"LauncherOwnProbe-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L".exe";
    const AppPathRegistration registration(name);
    registration.target(L"\"" + registered_probe.wstring() + L"\"", REG_SZ);
    const auto local_probe = fixture.path(name);
    REQUIRE(fs::copy_file(registered_probe, local_probe));
    launcher::LaunchAction action;
    action.target = utf8(name);
    const auto prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    CHECK(prepared.action->target == local_probe.wstring());
    const auto receipt = fixture.path(L"Bare executable receipt.bin");
    action.arguments = quoted(receipt);
    REQUIRE(launcher::launch_action(action, fixture.directory()).success);
    const auto report = read_receipt(receipt);
    CHECK(report.directory == fixture.directory());
    CHECK(fs::path(report.arguments.front()) == local_probe);
}

TEST_CASE("EXE helper targets launch beside the executable from a separate configuration", "[launch][native][exe]") {
    const Fixture fixture;
    const auto probe = fixture.copy_probe(false);
    const auto config = fixture.path(L"Launcher.toml");
    {
        std::ofstream file(config, std::ios::binary);
        file << "[[tabs]]\nname='General'\nitems=[{target='$EXE\\Gui probe.exe'}]\n";
        REQUIRE(file.good());
    }
    const auto loaded = launcher::load_configuration_files(config, {}, probe.parent_path());
    auto action = *loaded.configuration.tabs.front().items.front().launch;
    const auto prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    REQUIRE(prepared.action->target == probe.wstring());
    const auto receipt = fixture.path(L"EXE helper receipt.bin");
    action.arguments = quoted(receipt);
    REQUIRE(launcher::launch_action(action, fixture.directory()).success);
    const auto report = read_receipt(receipt);
    CHECK(fs::path(report.arguments.front()) == probe);
    CHECK(report.executable == probe);
    CHECK(report.directory == probe.parent_path());
}

TEST_CASE("App Paths executables are pinned for launch and metadata", "[launch][native][pinned]") {
    const Fixture fixture;
    const auto probe = fixture.copy_probe(false);
    const std::wstring name = L"LauncherRegisteredProbe-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L".exe";
    const AppPathRegistration registration(name);
    registration.target(L"\"" + probe.wstring() + L"\"", REG_SZ);
    const auto controlled_path = fixture.path(L"Working folder 资料").wstring();
    registration.path_environment(controlled_path);
    launcher::LaunchAction action;
    action.target = utf8(name);
    const auto prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    CHECK(prepared.action->target == probe.wstring());
    const auto receipt = fixture.path(L"Registered executable receipt.bin");
    action.arguments = quoted(receipt);
    REQUIRE(launcher::launch_action(action, fixture.directory()).success);
    const auto report = read_receipt(receipt);
    CHECK(fs::path(report.arguments.front()) == probe);
    const auto baseline_receipt = fixture.path(L"Windows App Paths baseline receipt.bin");
    launch_windows_baseline(probe.wstring(), fixture.directory(), baseline_receipt);
    const auto baseline = read_receipt(baseline_receipt);
    CHECK(fs::path(baseline.arguments.front()) == probe);
    CHECK(report.arguments.size() == baseline.arguments.size());
    CHECK(report.directory == baseline.directory);
    // Compare the environment with Windows dispatch of the same explicit file.
    // Cast the comparisons to bool so failures never print the user's PATH.
    CHECK(static_cast<bool>(report.path_environment == baseline.path_environment));
    const auto bare_receipt = fixture.path(L"Bare registration environment receipt.bin");
    launch_windows_baseline(name, fixture.directory(), bare_receipt);
    const auto bare = read_receipt(bare_receipt);
    std::cout << "Optional App Paths PATH directory: bare=" << path_has_directory(bare.path_environment, controlled_path)
        << ", explicit=" << path_has_directory(report.path_environment, controlled_path) << '\n';
    const std::wstring variable = L"LAUNCHER_REGISTERED_PROBE_" + std::to_wstring(GetCurrentProcessId());
    const TemporaryEnvironment environment(variable, probe.wstring());
    registration.target(L"\"%" + variable + L"%\"", REG_EXPAND_SZ);
    const auto expanded = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(expanded.result.success);
    CHECK(expanded.action->target == probe.wstring());
    registration.target(fixture.path(L"Missing probe.exe").wstring(), REG_SZ);
    const auto missing = launcher::prepare_launch_action(action, fixture.directory());
    CHECK_FALSE(missing.result.success);
    CHECK(missing.result.error_code == ERROR_FILE_NOT_FOUND);
    CHECK_FALSE(missing.action);
}

TEST_CASE("Prepared executable dispatch cannot be redirected by later App Paths or PATH changes", "[launch][native][pinned]") {
    const Fixture fixture;
    const auto original = fixture.copy_probe(false);
    const std::wstring name = L"LauncherPinnedProbe-" + std::to_wstring(GetCurrentProcessId()) + L".exe";
    const AppPathRegistration registration(name);
    registration.target(original.wstring(), REG_SZ);
    const auto receipt = fixture.path(L"Pinned executable receipt.bin");
    launcher::LaunchAction action;
    action.target = utf8(name);
    action.arguments = quoted(receipt) + " \"same arguments\"";
    action.working_directory = "Working folder 资料";
    action.show = launcher::LaunchShow::hidden;
    const auto prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    REQUIRE(prepared.action);
    CHECK(prepared.action->target == original.wstring());
    CHECK(launcher::can_run_as_administrator(*prepared.action));

    const auto replacement = fixture.path(L"Different registered executable.exe");
    REQUIRE(fs::copy_file(original, replacement));
    registration.target(replacement.wstring(), REG_SZ);
    const auto changed_registration = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(changed_registration.action);
    CHECK(changed_registration.action->target == replacement.wstring());
    const auto path_directory = fixture.path(L"New PATH folder");
    REQUIRE(fs::create_directory(path_directory));
    const auto path_replacement = path_directory / name;
    REQUIRE(fs::copy_file(original, path_replacement));
    const TemporaryEnvironment path(L"PATH", path_directory.wstring());
    const auto changed_path = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(changed_path.action);
    CHECK(changed_path.action->target == path_replacement.wstring());
    const auto result = launcher::launch_prepared_action(*prepared.action, 0, "open");
    INFO(utf8(result.message));
    REQUIRE(result.success);
    const auto report = read_receipt(receipt);
    CHECK(report.executable == original);
    CHECK(report.directory == fixture.path(L"Working folder 资料"));
    REQUIRE(report.arguments.size() == 3);
    CHECK(report.arguments[2] == L"same arguments");
    CHECK(prepared.action->target == original.wstring());

    REQUIRE(fs::remove(original));
    auto missing = *prepared.action;
    const auto missing_receipt = fixture.path(L"Must not fall back to replacement.bin");
    missing.arguments = L"\"" + missing_receipt.wstring() + L"\"";
    const auto failed = launcher::launch_prepared_action(missing);
    CHECK_FALSE(failed.success);
    CHECK_FALSE(fs::exists(missing_receipt));
    launcher::NativeLaunchAction unresolved;
    unresolved.target = name;
    CHECK(launcher::launch_prepared_action(unresolved).error_code == ERROR_BAD_PATHNAME);
    auto invalid = *prepared.action;
    invalid.target = std::wstring(L"abc\0def", 7);
    CHECK(launcher::launch_prepared_action(invalid).error_code == ERROR_INVALID_PARAMETER);
}

TEST_CASE("Wildcard launch targets verify matching components before following their parent", "[launch][native][target-versions]") {
    const Fixture fixture;
    const auto target = fixture.path(L"Version parent.pdf");
    {
        std::ofstream file(target, std::ios::binary);
        file << "A document target.";
        REQUIRE(file.good());
    }
    launcher::LaunchAction action;
    action.target = "Missing */../Version parent.pdf";
    const auto pattern = launcher::version_target_pattern(action, fixture.directory());
    REQUIRE(pattern);
    CHECK(pattern->wstring().find(L"Missing *") != std::wstring::npos);
    const auto missing = launcher::prepare_launch_action(action, fixture.directory());
    CHECK_FALSE(missing.result.success);
    CHECK(missing.result.error_code == ERROR_FILE_NOT_FOUND);
    REQUIRE(fs::create_directory(fixture.path(L"Installed 5.2")));
    action.target = "Installed */../Version parent.pdf";
    const auto prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    REQUIRE(prepared.action);
    CHECK(prepared.action->target == target.wstring());
}

TEST_CASE("Bare executable icons search quoted PATH entries before App Paths", "[launch][native]") {
    const Fixture fixture;
    const auto registered_probe = fixture.copy_probe(false);
    const std::wstring name = L"LauncherPathProbe-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L".exe";
    const AppPathRegistration registration(name);
    registration.target(registered_probe.wstring(), REG_SZ);
    const auto path_probe = fixture.path(L"Working folder 资料") / name;
    REQUIRE(fs::copy_file(registered_probe, path_probe));
    const TemporaryEnvironment environment(L"PATH", L"\"" + path_probe.parent_path().wstring() + L"\"");
    launcher::LaunchAction action;
    action.target = utf8(name);
    const auto prepared = launcher::prepare_launch_action(action, fixture.directory());
    REQUIRE(prepared.result.success);
    CHECK(prepared.action->target == path_probe.wstring());
    const auto receipt = fixture.path(L"PATH executable receipt.bin");
    action.arguments = quoted(receipt);
    REQUIRE(launcher::launch_action(action, fixture.directory()).success);
    const auto report = read_receipt(receipt);
    CHECK(fs::path(report.arguments.front()) == path_probe);
}
