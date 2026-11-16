#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#ifndef __INTELLISENSE__
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/file_lookup.hpp"
#include "launcher/color_resources.hpp"
#include "launcher/config_error.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <format>
#include <ranges>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef __INTELLISENSE__
#include <jmd/files.hpp>
#else
import jmd.files;
#endif

namespace launcher {
namespace {
namespace fs = std::filesystem;

std::string path_text(const fs::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

[[noreturn]] void file_error(std::string_view action, const fs::path& path, DWORD code) {
    const std::error_code error(static_cast<int>(code), std::system_category());
    throw ConfigError(std::format("{} '{}': {} ({})", action, path_text(path), error.message(), code));
}

fs::path full_path(const fs::path& path) {
    if (path.empty()) { return {}; }
    std::error_code error;
    const auto full = fs::absolute(path, error);
    if (error) { throw ConfigError(std::format("Cannot resolve '{}': {}", path_text(path), error.message())); }
    return full;
}

using jmd::same_directory;

fs::path known_folder(REFKNOWNFOLDERID id) try {
    return jmd::known_folder(id, KF_FLAG_DONT_VERIFY);
} catch (const std::system_error& error) {
    throw ConfigError("Cannot locate a Windows application folder: " + error.code().message());
}

fs::path module_filename() try {
    return jmd::module_filename();
} catch (const std::system_error& error) {
    file_error("Cannot locate executable", {}, static_cast<DWORD>(error.code().value()));
}

InstallationScope scope_for_directory(const fs::path& directory) {
    static const auto local = known_folder(FOLDERID_LocalAppData) / L"Programs" / L"Długosz" / L"Launcher";
    static const auto machine = known_folder(FOLDERID_ProgramFiles) / L"Długosz" / L"Launcher";
    if (same_directory(directory, machine)) { return InstallationScope::all_users; }
    if (same_directory(directory, local)) { return InstallationScope::current_user; }
    return InstallationScope::portable;
}

class FileHandle final {
public:
    explicit FileHandle(const fs::path& path) : handle_(CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        if (handle_ == INVALID_HANDLE_VALUE) { file_error("Cannot open file", path, GetLastError()); }
    }
    ~FileHandle() { CloseHandle(handle_); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    HANDLE get() const { return handle_; }
private:
    HANDLE handle_;
};

bool readable_file(const fs::path& path) try {
    return jmd::readable_file(path);
} catch (const jmd::FileProbeError& error) {
    const auto& failed = error.path1();
    const auto code = static_cast<DWORD>(error.code().value());
    switch (error.failure()) {
    case jmd::FileProbeFailure::inspect_file: file_error("Cannot inspect file", failed, code);
    case jmd::FileProbeFailure::open_file: file_error("Cannot open file", failed, code);
    case jmd::FileProbeFailure::inspect_parent: file_error("Cannot inspect directory", failed, code);
    case jmd::FileProbeFailure::open_parent: file_error("Cannot access directory", failed, code);
    case jmd::FileProbeFailure::file_is_directory:
        throw ConfigError("Expected a regular file at '" + path_text(failed) + "', but found a directory");
    case jmd::FileProbeFailure::parent_is_not_directory:
        throw ConfigError("Expected a directory at '" + path_text(failed) + "' while looking for '" + path_text(path) + "'");
    }
    throw;
}

void validate_name(const fs::path& name) {
    if (name.empty()) { throw ConfigError("File name must not be empty"); }
    if (!name.is_absolute() && (name.has_root_name() || name.has_root_directory())) {
        throw ConfigError("File path must be fully absolute or relative: '" + path_text(name) + "'");
    }
}
} // namespace

FileLookup::FileLookup(fs::path executable, fs::path user_directory, fs::path configuration_directory, bool explicitly_named)
    : executable_(full_path(executable)), user_directory_(full_path(user_directory)), explicitly_named_(explicitly_named) {
    if (!executable_.empty()) { installation_scope_ = scope_for_directory(executable_.parent_path()); }
    if (explicitly_named_ && !configuration_directory.empty()) { directories_.push_back(full_path(configuration_directory)); }
    if (portable()) { directories_.push_back(executable_.parent_path()); }
    if (!user_directory_.empty()) { directories_.push_back(user_directory_); }

    std::vector<fs::path> unique;
    for (const auto& directory : directories_ | std::views::reverse) {
        if (std::ranges::none_of(unique, [&](const auto& existing) { return same_directory(directory, existing); })) {
            unique.push_back(directory);
        }
    }
    std::ranges::reverse(unique);
    directories_ = std::move(unique);
}

std::optional<fs::path> FileLookup::find(const fs::path& name, FileFallback fallback) const {
    validate_name(name);
    if (name.is_absolute() || fallback == FileFallback::exact_file) {
        const auto candidate = full_path(name);
        return readable_file(candidate) ? std::optional(candidate) : std::nullopt;
    }
    for (const auto& directory : directories_) {
        const auto candidate = directory / name;
        if (readable_file(candidate)) { return candidate; }
    }
    return std::nullopt;
}

fs::path FileLookup::require(const fs::path& name, FileFallback fallback) const {
    if (const auto path = find(name, fallback)) { return *path; }
    throw ConfigError(missing_message(name, fallback));
}

std::string FileLookup::missing_message(const fs::path& name, FileFallback fallback) const {
    std::string message = "Cannot find file '" + path_text(fallback == FileFallback::exact_file ? exact_path(name) : name) + "'";
    if (!name.is_absolute() && fallback != FileFallback::exact_file) {
        message += "; searched:";
        for (const auto& directory : directories_) { message += "\n  " + path_text(directory / name); }
    }
    return message + ": " + std::system_category().message(ERROR_FILE_NOT_FOUND) + " (2)";
}

FileText FileLookup::read_text(const fs::path& name, FileFallback fallback) const {
    if (auto source = read_optional_text(name, fallback)) { return std::move(*source); }
    throw ConfigError(missing_message(name, fallback));
}

std::optional<FileText> FileLookup::read_optional_text(const fs::path& name, FileFallback fallback) const {
    if (const auto path = find(name, fallback)) {
        try { return FileText{read_file(*path), path_text(*path), *path}; }
        catch (ConfigError& error) {
            error.source_file = *path;
            throw;
        }
    }
    if (fallback == FileFallback::embedded_resource && !name.is_absolute()) {
        const auto filename = path_text(name);
        const auto source = path_text(module_filename()) + "!resource:" + filename;
        try { return FileText{std::string(color_resource_text(filename)), source, {}}; }
        catch (const std::exception& error) { throw ConfigError(source + ": " + error.what()); }
    }
    return std::nullopt;
}

fs::path FileLookup::exact_path(const fs::path& name) const {
    validate_name(name);
    return full_path(name);
}

fs::path FileLookup::runtime_path(const fs::path& configuration) const {
    auto result = configuration;
    result.replace_extension(L".runtime.toml");
    return result;
}

std::ofstream FileLookup::open_output(const fs::path& name, bool append) const {
    const auto path = exact_path(name);
    std::ofstream output(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!output) {
        const std::error_code error(errno ? errno : EIO, std::generic_category());
        throw ConfigError("Cannot open output file '" + path_text(path) + "': " + error.message() + " (" + std::to_string(error.value()) + ")");
    }
    return output;
}

void FileLookup::write_text(const fs::path& name, std::string_view text) const {
    const auto path = exact_path(name);
    auto output = open_output(path);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    if (!output) { throw ConfigError("Cannot finish writing file '" + path_text(path) + "': " + std::generic_category().message(errno ? errno : EIO)); }
}

std::string FileLookup::read_file(const fs::path& filename, std::size_t maximum_bytes) {
    const auto path = full_path(filename);
    const FileHandle file(path);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) { file_error("Cannot inspect file size", path, GetLastError()); }
    std::string text;
    if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > std::min(maximum_bytes, text.max_size())) {
        throw ConfigError("File is too large to read: '" + path_text(path) + "'");
    }
    text.reserve(static_cast<std::size_t>(size.QuadPart));
    std::array<char, 64 * 1024> buffer;
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            file_error("Cannot read file", path, GetLastError());
        }
        if (!count) { return text; }
        if (count > maximum_bytes - text.size()) { throw ConfigError("File grew beyond the read limit: '" + path_text(path) + "'"); }
        text.append(buffer.data(), count);
    }
}

} // namespace launcher
