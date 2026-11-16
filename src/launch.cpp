#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/launch.hpp"
#include "launcher/adobe_cc.hpp"
#include "launcher/target_versions.hpp"
#include "launcher/target_version_scan.hpp"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace launcher {
namespace {

namespace fs = std::filesystem;

class LaunchError final : public std::runtime_error {
public:
    LaunchError(DWORD code, const char* message) : std::runtime_error(message), code_(code) {}
    DWORD code() const noexcept { return code_; }
private:
    DWORD code_;
};

std::wstring utf16(std::string_view source) {
    if (source.find('\0') != std::string_view::npos || source.size() > INT_MAX) {
        throw LaunchError(ERROR_INVALID_PARAMETER, "Launch text contains NUL or is too long.");
    }
    if (source.empty()) {
        return {};
    }
    const int length = static_cast<int>(source.size());
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        source.data(), length, nullptr, 0);
    if (count == 0) {
        throw LaunchError(ERROR_NO_UNICODE_TRANSLATION, "Launch text is not valid UTF-8.");
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            source.data(), length, result.data(), count) == 0) {
        throw LaunchError(GetLastError(), "Cannot convert launch text to Unicode.");
    }
    return result;
}

std::wstring expand_registry_environment(const std::wstring& value) {
    const DWORD count = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (count == 0) {
        throw LaunchError(GetLastError(), "Cannot expand registry environment variables.");
    }
    std::wstring result(count, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(value.c_str(), result.data(), count);
    if (written == 0 || written > count) {
        throw LaunchError(written == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER,
            "Cannot expand registry environment variables.");
    }
    result.resize(written - 1U);
    return result;
}

bool starts_with_insensitive(std::wstring_view value, std::wstring_view prefix) noexcept {
    return value.size() >= prefix.size() && CompareStringOrdinal(value.data(),
        static_cast<int>(prefix.size()), prefix.data(), static_cast<int>(prefix.size()),
        TRUE) == CSTR_EQUAL;
}

bool equal_insensitive(std::wstring_view value, std::wstring_view other) noexcept {
    return value.size() == other.size() && starts_with_insensitive(value, other);
}

bool letter(wchar_t character) noexcept {
    return (character >= L'a' && character <= L'z') ||
        (character >= L'A' && character <= L'Z');
}

bool uri(std::wstring_view value) noexcept {
    const auto colon = value.find(L':');
    // A single-letter prefix is a Windows drive, including drive-relative paths.
    if (colon == std::wstring_view::npos || colon <= 1 || !letter(value.front())) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.begin() + colon, [](wchar_t character) {
        return letter(character) || (character >= L'0' && character <= L'9') ||
            character == L'+' || character == L'-' || character == L'.';
    });
}

bool explicit_path(std::wstring_view target) {
    const fs::path path(target);
    if (path.has_parent_path() || target.starts_with(L'.')) {
        return true;
    }
    // Bare executable names use App Paths/PATH lookup. Other simple
    // filenames (links and documents in particular) belong beside the config.
    const auto extension = path.extension().wstring();
    return !extension.empty() && !equal_insensitive(extension, L".exe") &&
        !equal_insensitive(extension, L".com");
}

fs::path absolute_path(const fs::path& source, const fs::path& base, bool normalize = true) {
    if (source.has_root_name() && !source.has_root_directory()) {
        throw LaunchError(ERROR_BAD_PATHNAME,
            "Drive-relative launch paths are ambiguous; use an absolute path.");
    }
    const auto path = source.is_absolute() ? source : base / source;
    return normalize ? path.lexically_normal() : path;
}

fs::path configuration_base(const fs::path& configuration_directory) {
    if (configuration_directory.native().find(L'\0') != std::wstring::npos) {
        throw LaunchError(ERROR_INVALID_PARAMETER, "The config directory contains NUL.");
    }
    return fs::absolute(configuration_directory).lexically_normal();
}

std::wstring launch_target(const LaunchAction& action) {
    auto target = utf16(action.target);
    if (target.empty()) {
        throw LaunchError(ERROR_INVALID_PARAMETER, "The launch target is empty.");
    }
    return target;
}

bool shell_namespace(std::wstring_view target) noexcept {
    return starts_with_insensitive(target, L"shell:") || target.starts_with(L"::");
}

std::optional<fs::path> target_pattern(std::wstring_view target, const fs::path& base) {
    if (shell_namespace(target) || uri(target) || !has_target_wildcards(target)) {
        return std::nullopt;
    }
    // A bare wildcard filename belongs beside the config just like a relative
    // path; executable PATH/App Paths lookup applies only to literal names.
    // Keep wildcard/.. pairs so discovery verifies the wildcard component
    // exists before following its parent; lexical normalization would erase it.
    return absolute_path(fs::path(target), base, false);
}

std::wstring unquote(std::wstring value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(L" \t\r\n");
    value = value.substr(first, last - first + 1);
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

bool existing_file(const fs::path& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring environment_value(const wchar_t* name) {
    const DWORD count = GetEnvironmentVariableW(name, nullptr, 0);
    if (count == 0) {
        return {};
    }
    std::wstring value(count, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), count);
    if (written == 0 || written >= count) {
        return {};
    }
    value.resize(written);
    return value;
}

std::wstring system_directory(bool windows) {
    std::wstring value(32768, L'\0');
    const UINT written = windows ? GetWindowsDirectoryW(value.data(),
        static_cast<UINT>(value.size())) : GetSystemDirectoryW(value.data(),
        static_cast<UINT>(value.size()));
    if (written == 0 || written >= value.size()) {
        return {};
    }
    value.resize(written);
    return value;
}

std::wstring registered_application(HKEY root, const std::wstring& executable) {
    const std::wstring key = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + executable;
    constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
    DWORD bytes = 0;
    DWORD type = 0;
    if (RegGetValueW(root, key.c_str(), nullptr, flags, &type, nullptr, &bytes) != ERROR_SUCCESS ||
            bytes < sizeof(wchar_t) || bytes > 65536 || bytes % sizeof(wchar_t) != 0) {
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key.c_str(), nullptr, flags, &type, value.data(), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    if (value.find(L'\0') != std::wstring::npos) {
        return {};
    }
    if (type == REG_EXPAND_SZ) {
        value = expand_registry_environment(value);
    }
    const fs::path path(unquote(std::move(value)));
    // App Paths' default value is a full filename, not a command line.
    return path.is_absolute() && existing_file(path) ? path.lexically_normal().wstring() : std::wstring{};
}

LaunchResult failure(DWORD code, std::wstring prefix) {
    if (code == ERROR_SUCCESS) {
        code = ERROR_GEN_FAILURE;
    }
    wchar_t* buffer = nullptr;
    const DWORD count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
        0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    const auto release = [](wchar_t* value) noexcept { LocalFree(value); };
    const std::unique_ptr<wchar_t, decltype(release)> owned_buffer(buffer, release);
    if (count != 0 && buffer != nullptr) {
        std::wstring description(buffer, count);
        while (!description.empty() && (description.back() == L'\r' ||
                description.back() == L'\n' || description.back() == L' ')) {
            description.pop_back();
        }
        prefix += L" " + description;
    }
    return {false, code, std::move(prefix)};
}

int show_command(LaunchShow show) {
    switch (show) {
    case LaunchShow::normal: return SW_SHOWNORMAL;
    case LaunchShow::hidden: return SW_HIDE;
    case LaunchShow::minimized: return SW_SHOWMINIMIZED;
    }
    throw LaunchError(ERROR_INVALID_PARAMETER, "Invalid launch show mode.");
}

class ComScope final {
public:
    ComScope() noexcept : result_(CoInitializeEx(nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ComScope() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }
    HRESULT result() const noexcept { return result_; }
private:
    HRESULT result_;
};

std::wstring lookup_executable(const NativeLaunchAction& action);

} // namespace

std::optional<fs::path> version_target_pattern(const LaunchAction& action, const fs::path& configuration_directory) {
    try {
        if (utf16(action.handler) != L"shell") { return std::nullopt; }
        return target_pattern(launch_target(action), configuration_base(configuration_directory));
    } catch (const std::exception&) {
        // Discovery is optional; normal preparation reports malformed actions.
        return std::nullopt;
    }
}

PreparedLaunch prepare_launch_action(const LaunchAction& action,
    const fs::path& configuration_directory) {
    try {
        const auto handler = utf16(action.handler);
        std::optional<AdobeCCInstallation> adobe;
        if (handler == L"adobeCC") {
            adobe = adobe_cc_latest(scan_adobe_cc_for_action(action, configuration_directory), utf16(action.target), action.include_beta);
            if (!adobe) {
                return {std::nullopt, failure(ERROR_FILE_NOT_FOUND,
                    L"No installed Adobe CC application matches \"" + utf16(action.target) + L"\".")};
            }
        } else if (handler != L"shell") {
            return {std::nullopt, failure(ERROR_NOT_SUPPORTED,
                L"Unsupported launch handler: " + handler + L".")};
        }
        NativeLaunchAction resolved;
        resolved.target = adobe ? adobe->executable.wstring() : launch_target(action);
        resolved.arguments = utf16(action.arguments);
        resolved.show = action.show;
        static_cast<void>(show_command(resolved.show));
        const fs::path base = configuration_base(configuration_directory);
        resolved.shell_namespace = shell_namespace(resolved.target);
        if (!adobe) {
            if (const auto pattern = target_pattern(resolved.target, base)) {
                const auto versions = scan_target_versions(*pattern);
                if (versions.empty()) {
                    return {std::nullopt, failure(ERROR_FILE_NOT_FOUND,
                        L"No files match the version target: " + pattern->wstring() + L".")};
                }
                resolved.target = versions.front().executable.wstring();
            }
        }
        const bool filesystem_target = !resolved.shell_namespace && !uri(resolved.target) &&
            explicit_path(resolved.target);
        if (filesystem_target) {
            resolved.target = absolute_path(fs::path(resolved.target), base).wstring();
        }
        if (!action.working_directory.empty()) {
            resolved.working_directory = absolute_path(fs::path(utf16(action.working_directory)), base).wstring();
        } else if (filesystem_target) {
            const fs::path target(resolved.target);
            const auto extension = target.extension().wstring();
            if (!equal_insensitive(extension, L".lnk")) {
                resolved.working_directory = target.parent_path().wstring();
            }
        } else {
            resolved.working_directory = base.wstring();
        }
        if (auto found = lookup_executable(resolved); !found.empty()) {
            // Pin every filesystem match, including App Paths. Dispatch must
            // use the same file shown by icons, labels and context actions.
            resolved.target = std::move(found);
        } else if (!resolved.shell_namespace && !uri(resolved.target) && !explicit_path(resolved.target)) {
            return {std::nullopt, failure(ERROR_FILE_NOT_FOUND,
                L"Cannot resolve the executable: " + resolved.target + L".")};
        }
        return {std::move(resolved), {true, 0, {}}};
    } catch (const LaunchError& error) {
        return {std::nullopt, failure(error.code(), utf16(error.what()))};
    } catch (const fs::filesystem_error& error) {
        return {std::nullopt, failure(static_cast<DWORD>(error.code().value()),
            L"Cannot resolve the launch path.")};
    }
}

PreparedLaunch prepare_exact_target_action(const LaunchAction& action, const fs::path& configuration_directory) {
    try {
        if (utf16(action.handler) != L"shell") {
            return {std::nullopt, failure(ERROR_NOT_SUPPORTED, L"An exact catalog target requires the Shell handler.")};
        }
        NativeLaunchAction resolved;
        resolved.target = utf16(action.target);
        resolved.arguments = utf16(action.arguments);
        resolved.show = action.show;
        static_cast<void>(show_command(resolved.show));
        if (resolved.target.empty() || shell_namespace(resolved.target) || uri(resolved.target) || has_target_wildcards(resolved.target)) {
            return {std::nullopt, failure(ERROR_INVALID_PARAMETER, L"An exact catalog target must name one concrete filesystem item.")};
        }
        const fs::path target(resolved.target);
        if (!target.is_absolute()) {
            return {std::nullopt, failure(ERROR_BAD_PATHNAME, L"An exact catalog target must be an absolute path.")};
        }
        if (!configuration_directory.is_absolute() || configuration_directory.native().find(L'\0') != std::wstring::npos) {
            return {std::nullopt, failure(ERROR_BAD_PATHNAME, L"Exact target preparation requires an absolute configuration directory.")};
        }
        if (!action.working_directory.empty()) {
            resolved.working_directory = absolute_path(fs::path(utf16(action.working_directory)), configuration_directory.lexically_normal()).wstring();
        } else if (!equal_insensitive(target.extension().wstring(), L".lnk")) {
            resolved.working_directory = target.parent_path().wstring();
        }
        return {std::move(resolved), {true, 0, {}}};
    } catch (const LaunchError& error) {
        return {std::nullopt, failure(error.code(), utf16(error.what()))};
    } catch (const fs::filesystem_error& error) {
        return {std::nullopt, failure(static_cast<DWORD>(error.code().value()), L"Cannot prepare the exact catalog path.")};
    }
}

namespace {

std::wstring lookup_executable(const NativeLaunchAction& action) {
    try {
        if (action.shell_namespace || uri(action.target) || explicit_path(action.target) ||
                action.target.empty()) {
            return {};
        }
        const fs::path base(action.working_directory);
        if (!base.is_absolute()) {
            return {};
        }
        std::vector<std::wstring> names{action.target};
        if (fs::path(action.target).extension().empty()) {
            names.push_back(action.target + L".exe");
        }
        const auto in_directory = [&](const fs::path& directory) -> std::wstring {
            for (const auto& name : names) {
                const fs::path file = absolute_path(directory / name, base);
                if (existing_file(file)) {
                    return file.wstring();
                }
            }
            return {};
        };
        // Use the documented locations with the configured working directory
        // as our deterministic first location. No process-wide cwd changes.
        if (auto found = in_directory(base); !found.empty()) {
            return found;
        }
        for (const bool windows : {true, false}) {
            const auto directory = system_directory(windows);
            if (!directory.empty()) {
                if (auto found = in_directory(fs::path(directory)); !found.empty()) {
                    return found;
                }
            }
        }
        const auto path = environment_value(L"PATH");
        bool quoted = false;
        std::size_t begin = 0;
        for (std::size_t index = 0; index <= path.size(); ++index) {
            if (index < path.size() && path[index] == L'"') {
                quoted = !quoted;
            }
            if (index == path.size() || (path[index] == L';' && !quoted)) {
                const auto directory = unquote(path.substr(begin, index - begin));
                if (auto found = in_directory(directory.empty() ? base : fs::path(directory));
                        !found.empty()) {
                    return found;
                }
                begin = index + 1;
            }
        }
        for (const HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
            for (const auto& name : names) {
                if (auto found = registered_application(root, name); !found.empty()) {
                    return found;
                }
            }
        }
    } catch (const std::exception&) {
        // An unavailable registry value or transient environment/path error
        // should not prevent the caller from drawing its generic fallback icon.
    }
    return {};
}

bool version_range(const std::vector<std::byte>& bytes, const void* value, std::size_t size) noexcept {
    if (value == nullptr) { return false; }
    const auto begin = reinterpret_cast<std::uintptr_t>(bytes.data());
    const auto address = reinterpret_cast<std::uintptr_t>(value);
    return address >= begin && address - begin <= bytes.size() &&
        size <= bytes.size() - (address - begin);
}

std::wstring executable_description(const fs::path& path) {
    // Optional presentation must not bypass discovery's resource/text bounds.
    // A rejected description simply leaves the concrete filename as the label.
    const TargetScanLimits limits;
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0 || size > limits.max_resource_bytes) { return {}; }
    std::vector<std::byte> bytes(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, bytes.data())) { return {}; }
    struct Translation { WORD language; WORD codepage; };
    const std::byte* translations = nullptr;
    std::size_t translation_count = 0;
    void* value = nullptr;
    UINT length = 0;
    if (VerQueryValueW(bytes.data(), L"\\VarFileInfo\\Translation", &value, &length) &&
            length % sizeof(Translation) == 0 && version_range(bytes, value, length)) {
        translations = static_cast<const std::byte*>(value);
        translation_count = length / sizeof(Translation);
    }
    if (translation_count > limits.max_translations) { return {}; }
    // A few resources retain a conventional string table without listing it.
    for (std::size_t index = 0; index < translation_count + 2; ++index) {
        Translation translation{0x0409, index == translation_count ? WORD{0x04b0} : WORD{0x04e4}};
        if (index < translation_count) { std::memcpy(&translation, translations + index * sizeof(Translation), sizeof(translation)); }
        const auto query = std::format(L"\\StringFileInfo\\{:04x}{:04x}\\FileDescription",
            translation.language, translation.codepage);
        value = nullptr;
        length = 0;
        if (!VerQueryValueW(bytes.data(), query.c_str(), &value, &length) || length == 0 ||
                length > bytes.size() / sizeof(wchar_t) ||
                !version_range(bytes, value, static_cast<std::size_t>(length) * sizeof(wchar_t))) {
            continue;
        }
        std::size_t units = 0;
        for (; units < length; ++units) {
            wchar_t unit = 0;
            std::memcpy(&unit, static_cast<const std::byte*>(value) + units * sizeof(wchar_t), sizeof(wchar_t));
            if (unit == L'\0') { break; }
            if (units == limits.max_metadata_units) { return {}; }
        }
        std::wstring description(units, L'\0');
        std::memcpy(description.data(), value, units * sizeof(wchar_t));
        const auto first = description.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos) { continue; }
        const auto last = description.find_last_not_of(L" \t\r\n");
        return description.substr(first, last - first + 1);
    }
    return {};
}

} // namespace

std::wstring launch_display_label(const NativeLaunchAction& action) {
    if (action.shell_namespace || uri(action.target)) { return action.target; }
    const auto& value = action.target;
    std::wstring fallback = value;
    try {
        if (value.find(L'\0') != std::wstring::npos) { return {}; }
        const fs::path path(value);
        if (const auto filename = path.filename().wstring(); !filename.empty()) { fallback = filename; }
        // Never let the version API perform its own bare-name search: only the
        // prepared absolute target can identify the executable we will open.
        if (path.is_absolute() && equal_insensitive(path.extension().wstring(), L".exe")) {
            if (auto description = executable_description(path); !description.empty()) { return description; }
        }
    } catch (const std::exception&) {
        // A label is optional presentation; unavailable metadata cannot veto a launch.
    }
    return fallback;
}

bool can_run_as_administrator(const NativeLaunchAction& action) noexcept {
    try {
        if (action.shell_namespace || action.target.empty() || uri(action.target)) {
            return false;
        }
        const auto& value = action.target;
        if (value.find(L'\0') != std::wstring::npos) { return false; }
        const fs::path target(value);
        if (!target.is_absolute() || !existing_file(target)) { return false; }
        const auto extension = target.extension().wstring();
        return equal_insensitive(extension, L".exe") || equal_insensitive(extension, L".com") ||
            equal_insensitive(extension, L".bat") || equal_insensitive(extension, L".cmd") ||
            equal_insensitive(extension, L".lnk");
    } catch (const std::exception&) {
        return false;
    }
}

LaunchResult reveal_launch_target(const NativeLaunchAction& action, std::uintptr_t owner_window) {
    try {
        if (action.target.empty() || action.target.find(L'\0') != std::wstring::npos) {
            return failure(ERROR_INVALID_PARAMETER, L"The reveal target is empty or contains NUL.");
        }
        std::wstring target = action.target;
        if (!action.shell_namespace) {
            if (uri(target)) {
                return failure(ERROR_NOT_SUPPORTED, L"A web or protocol target has no folder to reveal.");
            }
            // Preparation has pinned executable paths. Shortcuts retain their
            // own .lnk path, so reveal selects the shortcut itself.
            if (target.empty() || target.find(L'\0') != std::wstring::npos ||
                    !fs::path(target).is_absolute()) {
                return failure(ERROR_FILE_NOT_FOUND, L"No resolved filesystem target is available to reveal.");
            }
            if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
                return failure(GetLastError(), L"The target to reveal does not exist or is unavailable: " + target + L".");
            }
        }
        const ComScope com;
        if (FAILED(com.result())) {
            return failure(ERROR_NOT_SUPPORTED, L"Revealing a target requires a COM STA thread.");
        }
        struct Pidl final {
            PIDLIST_ABSOLUTE value = nullptr;
            ~Pidl() { CoTaskMemFree(value); }
        } pidl;
        SFGAOF attributes = 0;
        const HRESULT parsed = SHParseDisplayName(target.c_str(), nullptr, &pidl.value,
            SFGAO_FOLDER, &attributes);
        if (FAILED(parsed)) {
            return failure(static_cast<DWORD>(HRESULT_CODE(parsed)), L"Windows could not locate " + target + L".");
        }
        if (action.shell_namespace && (attributes & SFGAO_FOLDER) != 0) {
            // Virtual containers such as This PC do not have an ordinary
            // parent directory; open that container without launching an item.
            SHELLEXECUTEINFOW execute{};
            execute.cbSize = sizeof(execute);
            execute.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            execute.hwnd = reinterpret_cast<HWND>(owner_window);
            execute.lpVerb = L"open";
            execute.lpIDList = pidl.value;
            execute.nShow = SW_SHOWNORMAL;
            SetLastError(ERROR_SUCCESS);
            const BOOL opened = ShellExecuteExW(&execute);
            return opened ? LaunchResult{true, 0, {}} :
                failure(GetLastError(), L"Windows could not open the target's container.");
        }
        // With zero selection entries, Windows selects this fully qualified
        // item in its parent folder, including a .lnk rather than its target.
        const HRESULT opened = SHOpenFolderAndSelectItems(pidl.value, 0, nullptr, 0);
        return SUCCEEDED(opened) ? LaunchResult{true, 0, {}} :
            failure(static_cast<DWORD>(HRESULT_CODE(opened)), L"Windows could not reveal " + target + L".");
    } catch (const fs::filesystem_error& error) {
        return failure(static_cast<DWORD>(error.code().value()), L"Cannot resolve the target to reveal.");
    }
}

LaunchResult show_target_properties(const NativeLaunchAction& action, std::uintptr_t owner_window) {
    try {
        if (action.target.empty() || action.target.find(L'\0') != std::wstring::npos || action.shell_namespace ||
                shell_namespace(action.target) || uri(action.target) || has_target_wildcards(action.target) ||
                !fs::path(action.target).is_absolute()) {
            return failure(ERROR_INVALID_PARAMETER, L"Properties requires one concrete absolute filesystem target.");
        }
        const ComScope com;
        if (FAILED(com.result())) { return failure(ERROR_NOT_SUPPORTED, L"Properties requires a COM STA worker."); }
        struct Pidl final {
            PIDLIST_ABSOLUTE value = nullptr;
            ~Pidl() { CoTaskMemFree(value); }
        } pidl;
        const HRESULT parsed = SHParseDisplayName(action.target.c_str(), nullptr, &pidl.value, 0, nullptr);
        if (FAILED(parsed)) {
            return failure(static_cast<DWORD>(HRESULT_CODE(parsed)), L"Windows could not locate the item whose Properties were requested.");
        }
        SHELLEXECUTEINFOW execute{};
        execute.cbSize = sizeof(execute);
        execute.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        execute.hwnd = reinterpret_cast<HWND>(owner_window);
        execute.lpVerb = L"properties";
        execute.lpIDList = pidl.value;
        execute.nShow = SW_SHOWNORMAL;
        SetLastError(ERROR_SUCCESS);
        const BOOL shown = ShellExecuteExW(&execute);
        const DWORD error = shown ? ERROR_SUCCESS : GetLastError();
        if (execute.hProcess) { CloseHandle(execute.hProcess); }
        return shown ? LaunchResult{true, 0, {}} : failure(error, L"Windows could not show Properties for the selected item.");
    } catch (const fs::filesystem_error& error) {
        return failure(static_cast<DWORD>(error.code().value()), L"Cannot resolve the item whose Properties were requested.");
    }
}

LaunchResult launch_prepared_action(const NativeLaunchAction& resolved,
    std::uintptr_t owner_window, std::string_view verb) {
    std::wstring native_verb;
    try {
        native_verb = utf16(verb);
        for (const auto* text : {&resolved.target, &resolved.arguments, &resolved.working_directory}) {
            if (text->find(L'\0') != std::wstring::npos) {
                throw LaunchError(ERROR_INVALID_PARAMETER, "Prepared launch text contains NUL.");
            }
        }
        if (resolved.target.empty()) { throw LaunchError(ERROR_INVALID_PARAMETER, "The launch target is empty."); }
        if (!resolved.shell_namespace && !uri(resolved.target) && !fs::path(resolved.target).is_absolute()) {
            throw LaunchError(ERROR_BAD_PATHNAME, "The prepared launch target must be an absolute path.");
        }
        static_cast<void>(show_command(resolved.show));
    } catch (const LaunchError& error) {
        return failure(error.code(), utf16(error.what()));
    }
    const ComScope com;
    if (FAILED(com.result())) {
        return failure(ERROR_NOT_SUPPORTED, L"Shell launching requires a COM STA thread.");
    }
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    if (resolved.shell_namespace) {
        execute.fMask |= SEE_MASK_INVOKEIDLIST;
    }
    execute.hwnd = reinterpret_cast<HWND>(owner_window);
    execute.lpVerb = native_verb.empty() ? L"open" : native_verb.c_str();
    execute.lpFile = resolved.target.c_str();
    execute.lpParameters = resolved.arguments.empty() ? nullptr : resolved.arguments.c_str();
    execute.lpDirectory = resolved.working_directory.empty() ? nullptr :
        resolved.working_directory.c_str();
    execute.nShow = show_command(resolved.show);
    SetLastError(ERROR_SUCCESS);
    const BOOL started = ShellExecuteExW(&execute);
    const DWORD error = started ? ERROR_SUCCESS : GetLastError();
    if (execute.hProcess != nullptr) {
        CloseHandle(execute.hProcess);
    }
    return started ? LaunchResult{true, 0, {}} :
        failure(error, L"Windows could not open " + resolved.target + L".");
}

LaunchResult launch_action(const LaunchAction& action,
    const fs::path& configuration_directory, std::uintptr_t owner_window, std::string_view verb) {
    const auto prepared = prepare_launch_action(action, configuration_directory);
    if (!prepared.result.success) { return prepared.result; }
    return launch_prepared_action(*prepared.action, owner_window, verb);
}

} // namespace launcher
