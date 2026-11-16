#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/target_version_scan.hpp"
#include "launcher/file_lookup.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace launcher {
namespace {

std::error_code native_error(DWORD error) {
    return {static_cast<int>(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error), std::system_category()};
}

class FindHandle final {
public:
    explicit FindHandle(HANDLE value) : value_(value) {}
    ~FindHandle() { reset(); }
    FindHandle(FindHandle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    FindHandle(const FindHandle&) = delete;
    FindHandle& operator=(const FindHandle&) = delete;
    HANDLE get() const { return value_; }
    void reset() {
        if (value_ != INVALID_HANDLE_VALUE) { FindClose(std::exchange(value_, INVALID_HANDLE_VALUE)); }
    }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

class FileHandle final {
public:
    explicit FileHandle(HANDLE value) : value_(value) {}
    ~FileHandle() { if (value_ != INVALID_HANDLE_VALUE) { CloseHandle(value_); } }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    HANDLE get() const { return value_; }

private:
    HANDLE value_;
};

std::expected<HANDLE, std::error_code> open_native_exact(const std::filesystem::path& path, DWORD access, DWORD flags) {
    return FileLookup({}, {}).open_exact(path, [=](const auto& exact) -> std::expected<HANDLE, std::error_code> {
        const auto handle = CreateFileW(exact.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, flags, nullptr);
        if (handle == INVALID_HANDLE_VALUE) { return std::unexpected(native_error(GetLastError())); }
        return handle;
    });
}

TargetFileKind entry_kind(DWORD attributes) {
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) { return TargetFileKind::directory; }
    if (attributes & FILE_ATTRIBUTE_DEVICE) { return TargetFileKind::other; }
    return TargetFileKind::regular_file;
}

class NativeDirectoryCursor final : public TargetDirectoryCursor {
public:
    NativeDirectoryCursor(FindHandle handle, WIN32_FIND_DATAW first) : handle_(std::move(handle)), data_(first) {}

    std::expected<std::optional<TargetDirectoryEntry>, std::error_code> next() override {
        if (ended_) { return std::nullopt; }
        if (first_) {
            first_ = false;
        } else if (!FindNextFileW(handle_.get(), &data_)) {
            const DWORD error = GetLastError();
            ended_ = true;
            handle_.reset();
            if (error == ERROR_NO_MORE_FILES) { return std::nullopt; }
            return std::unexpected(native_error(error));
        }
        return std::optional<TargetDirectoryEntry>{{data_.cFileName, entry_kind(data_.dwFileAttributes),
            (data_.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0}};
    }

private:
    FindHandle handle_;
    WIN32_FIND_DATAW data_{};
    bool first_ = true;
    bool ended_ = false;
};

bool unc_share_root(const std::filesystem::path& path) {
    auto name = path.native();
    std::ranges::replace(name, L'/', L'\\');
    while (name.size() > 2 && name.back() == L'\\') { name.pop_back(); }
    std::wstring_view remainder(name);
    if (name.size() >= 8 && CompareStringOrdinal(name.data(), 8, L"\\\\?\\UNC\\", 8, TRUE) == CSTR_EQUAL) {
        remainder.remove_prefix(8);
    } else if (name.starts_with(L"\\\\") && !name.starts_with(L"\\\\?\\") && !name.starts_with(L"\\\\.\\")) {
        remainder.remove_prefix(2);
    } else {
        return false;
    }
    const auto separator = remainder.find(L'\\');
    return separator != std::wstring_view::npos && separator != 0 && separator + 1 < remainder.size() &&
        remainder.find(L'\\', separator + 1) == std::wstring_view::npos;
}

std::expected<DWORD, std::error_code> query_attributes(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) { return attributes; }
    const DWORD error = GetLastError();
    if (!unc_share_root(path)) { return std::unexpected(native_error(error)); }

    // GetFileAttributesW cannot query a share itself. Opening just that root for
    // attribute queries avoids mistaking a valid UNC search root for no matches.
    const auto opened = open_native_exact(path, 0, FILE_FLAG_BACKUP_SEMANTICS);
    if (!opened) { return std::unexpected(opened.error()); }
    FileHandle handle(*opened);
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle.get(), &information)) { return std::unexpected(native_error(GetLastError())); }
    return information.dwFileAttributes;
}

std::array<std::uint16_t, 4> numeric_version(DWORD high, DWORD low) {
    return {HIWORD(high), LOWORD(high), HIWORD(low), LOWORD(low)};
}

std::wstring version_text(const std::array<std::uint16_t, 4>& version) {
    return std::format(L"{}.{}.{}.{}", version[0], version[1], version[2], version[3]);
}

std::wstring trim(std::wstring_view text) {
    const auto first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring_view::npos) { return {}; }
    const auto last = text.find_last_not_of(L" \t\r\n");
    return std::wstring(text.substr(first, last - first + 1));
}

bool contains_bytes(const std::vector<std::byte>& bytes, const void* value, std::size_t length) {
    if (!value) { return false; }
    // VerQueryValueW promises a pointer into this buffer. Check its numeric
    // extent before copying; do not subtract potentially unrelated pointers.
    const auto first = reinterpret_cast<std::uintptr_t>(bytes.data());
    const auto address = reinterpret_cast<std::uintptr_t>(value);
    if (address < first || address - first > bytes.size()) { return false; }
    return length <= bytes.size() - static_cast<std::size_t>(address - first);
}

std::expected<std::wstring, std::error_code> query_text(const std::vector<std::byte>& bytes, WORD language, WORD codepage,
        std::wstring_view field, std::size_t maximum) {
    const auto query = std::format(L"\\StringFileInfo\\{:04x}{:04x}\\{}", language, codepage, field);
    void* value = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(bytes.data(), query.c_str(), &value, &length) || !value || length == 0) { return std::wstring{}; }
    // String lengths are UTF-16 characters; fixed and translation lengths are
    // bytes. Copying also avoids assuming alignment of malformed resource data.
    if (length > bytes.size() / sizeof(wchar_t) || !contains_bytes(bytes, value, length * sizeof(wchar_t))) { return std::wstring{}; }
    // Check the real field, rather than its declared padded extent, before
    // allocating. memcpy avoids assuming alignment in malformed resources.
    std::size_t units = 0;
    for (; units < length; ++units) {
        wchar_t unit = 0;
        std::memcpy(&unit, static_cast<const std::byte*>(value) + units * sizeof(wchar_t), sizeof(wchar_t));
        if (unit == L'\0') { break; }
        if (units == maximum) { return std::unexpected(make_target_scan_limit_error(TargetScanLimit::metadata_units)); }
    }
    std::wstring text(units, L'\0');
    std::memcpy(text.data(), value, units * sizeof(wchar_t));
    return trim(text);
}

bool absent_or_invalid_resource(DWORD error) {
    switch (error) {
    case ERROR_RESOURCE_DATA_NOT_FOUND:
    case ERROR_RESOURCE_TYPE_NOT_FOUND:
    case ERROR_RESOURCE_NAME_NOT_FOUND:
    case ERROR_RESOURCE_LANG_NOT_FOUND:
    case ERROR_BAD_FORMAT:
    case ERROR_BAD_EXE_FORMAT:
    case ERROR_INVALID_EXE_SIGNATURE:
    case ERROR_EXE_MARKED_INVALID:
    case ERROR_INVALID_DATA:
        return true;
    default:
        return false;
    }
}

std::expected<std::optional<TargetVersion>, std::error_code> metadata_failure(DWORD error) {
    if (absent_or_invalid_resource(error)) { return std::nullopt; }
    return std::unexpected(native_error(error));
}

std::expected<std::optional<TargetVersion>, std::error_code> read_native_metadata(const std::filesystem::path& path,
        const TargetScanLimits& limits) {
    // The version APIs can report ERROR_RESOURCE_DATA_NOT_FOUND even when an
    // exclusive sharing lock prevented reading an existing version resource.
    // Retain a readable handle so that access failures remain distinguishable
    // from optional metadata absence; allow installers their normal sharing.
    const auto opened = open_native_exact(path, GENERIC_READ, FILE_ATTRIBUTE_NORMAL);
    if (!opened) { return std::unexpected(opened.error()); }
    FileHandle readable(*opened);
    DWORD ignored = 0;
    SetLastError(ERROR_SUCCESS);
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) { return metadata_failure(GetLastError()); }
    if (size > limits.max_resource_bytes) { return std::unexpected(make_target_scan_limit_error(TargetScanLimit::resource_bytes)); }
    // Bound the resource buffer together with the metadata it can produce.
    // Four retained strings, a query/trim temporary and an executable path are
    // conservatively reserved before reading. Allocator bookkeeping is extra.
    const auto unit_bytes = sizeof(wchar_t);
    const auto maximum = (std::numeric_limits<std::size_t>::max)();
    const auto field_units = (std::min)(limits.max_metadata_units, static_cast<std::size_t>(size) / unit_bytes);
    if (field_units > maximum / (6 * unit_bytes) || path.native().capacity() > maximum / unit_bytes) {
        return std::unexpected(make_target_scan_limit_error(TargetScanLimit::storage_bytes));
    }
    const auto field_bytes = field_units * 6 * unit_bytes;
    const auto path_bytes = path.native().capacity() * unit_bytes;
    if (size > limits.max_storage_bytes || field_bytes > limits.max_storage_bytes - size ||
            path_bytes > limits.max_storage_bytes - size - field_bytes ||
            sizeof(TargetVersion) > limits.max_storage_bytes - size - field_bytes - path_bytes) {
        return std::unexpected(make_target_scan_limit_error(TargetScanLimit::storage_bytes));
    }
    std::vector<std::byte> bytes(size);
    SetLastError(ERROR_SUCCESS);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, bytes.data())) { return metadata_failure(GetLastError()); }
    void* value = nullptr;
    UINT length = 0;
    // VerQueryValueW does not document GetLastError; a missing/invalid root is
    // optional metadata absence, rather than an unrelated stale thread error.
    if (!VerQueryValueW(bytes.data(), L"\\", &value, &length) || length < sizeof(VS_FIXEDFILEINFO) ||
            !contains_bytes(bytes, value, length)) {
        return std::nullopt;
    }
    VS_FIXEDFILEINFO fixed{};
    std::memcpy(&fixed, value, sizeof(fixed));
    if (fixed.dwSignature != 0xfeef04bd) { return std::nullopt; }
    TargetVersion result;
    result.version = numeric_version(fixed.dwProductVersionMS, fixed.dwProductVersionLS);
    result.file_version_numbers = numeric_version(fixed.dwFileVersionMS, fixed.dwFileVersionLS);
    result.executable = path.lexically_normal();
    result.has_version_resource = true;
    struct Translation { WORD language; WORD codepage; };
    const std::byte* translations = nullptr;
    std::size_t translation_count = 0;
    if (VerQueryValueW(bytes.data(), L"\\VarFileInfo\\Translation", &value, &length) &&
            length % sizeof(Translation) == 0 && contains_bytes(bytes, value, length)) {
        translations = static_cast<const std::byte*>(value);
        translation_count = length / sizeof(Translation);
    }
    if (translation_count > limits.max_translations) {
        return std::unexpected(make_target_scan_limit_error(TargetScanLimit::translations));
    }
    // Read translation records directly from the bounded resource buffer;
    // malformed resources cannot force another unbounded translation vector.
    for (std::size_t index = 0; index < translation_count + 2; ++index) {
        Translation translation{0x0409, index == translation_count ? WORD{0x04b0} : WORD{0x04e4}};
        if (index < translation_count) { std::memcpy(&translation, translations + index * sizeof(Translation), sizeof(translation)); }
        const auto read_field = [&](std::wstring& destination, std::wstring_view field) -> std::error_code {
            if (!destination.empty()) { return {}; }
            auto text = query_text(bytes, translation.language, translation.codepage, field, limits.max_metadata_units);
            if (!text) { return text.error(); }
            destination = std::move(*text);
            return {};
        };
        if (const auto error = read_field(result.product_name, L"ProductName")) { return std::unexpected(error); }
        if (const auto error = read_field(result.product_version, L"ProductVersion")) { return std::unexpected(error); }
        if (const auto error = read_field(result.file_version, L"FileVersion")) { return std::unexpected(error); }
        if (!result.product_name.empty() && !result.product_version.empty() && !result.file_version.empty()) { break; }
    }
    if (result.product_version.empty()) { result.product_version = version_text(result.version); }
    if (result.file_version.empty()) { result.file_version = version_text(result.file_version_numbers); }
    if (result.product_name.size() > limits.max_metadata_units || result.product_version.size() > limits.max_metadata_units ||
            result.file_version.size() > limits.max_metadata_units) {
        return std::unexpected(make_target_scan_limit_error(TargetScanLimit::metadata_units));
    }
    result.name = result.product_name;
    return std::optional<TargetVersion>{std::move(result)};
}

class NativeTargetVersionSource final : public TargetVersionSource {
public:
    explicit NativeTargetVersionSource(TargetScanLimits limits) : limits_(limits) {}

    std::expected<TargetFileKind, std::error_code> probe_no_follow(const std::filesystem::path& path) override {
        const auto attributes = query_attributes(path);
        if (!attributes) { return std::unexpected(attributes.error()); }
        return (*attributes & FILE_ATTRIBUTE_REPARSE_POINT) ? TargetFileKind::other : entry_kind(*attributes);
    }

    std::expected<TargetFileKind, std::error_code> probe(const std::filesystem::path& path) override {
        const auto attributes = query_attributes(path);
        if (!attributes) { return std::unexpected(attributes.error()); }
        if ((*attributes & FILE_ATTRIBUTE_DIRECTORY) && !(*attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            return TargetFileKind::directory;
        }
        // Follow the target with a query-only handle. MSVC filesystem::status
        // can clear network errors into file_type::not_found, losing exactly
        // the provider failures that structured observations must preserve.
        const auto opened = open_native_exact(path, 0, FILE_FLAG_BACKUP_SEMANTICS);
        if (!opened) { return std::unexpected(opened.error()); }
        FileHandle handle(*opened);
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(handle.get(), &information)) { return std::unexpected(native_error(GetLastError())); }
        if (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { return TargetFileKind::directory; }
        SetLastError(ERROR_SUCCESS);
        const DWORD type = GetFileType(handle.get());
        const DWORD error = GetLastError();
        if (type == FILE_TYPE_UNKNOWN && error != ERROR_SUCCESS) { return std::unexpected(native_error(error)); }
        return type == FILE_TYPE_DISK ? TargetFileKind::regular_file : TargetFileKind::other;
    }

    std::expected<std::unique_ptr<TargetDirectoryCursor>, std::error_code> enumerate(const std::filesystem::path& pattern) override {
        WIN32_FIND_DATAW data{};
        FindHandle handle(FindFirstFileW(pattern.c_str(), &data));
        if (handle.get() == INVALID_HANDLE_VALUE) { return std::unexpected(native_error(GetLastError())); }
        return std::make_unique<NativeDirectoryCursor>(std::move(handle), data);
    }

    std::expected<std::optional<TargetVersion>, std::error_code> read_metadata(const std::filesystem::path& path) override {
        return read_native_metadata(path, limits_);
    }

    std::expected<std::optional<TargetVersion>, std::error_code> read_metadata(const std::filesystem::path& path,
            const TargetScanLimits& limits) override {
        auto effective = limits;
        effective.max_resource_bytes = (std::min)(effective.max_resource_bytes, limits_.max_resource_bytes);
        effective.max_storage_bytes = (std::min)(effective.max_storage_bytes, limits_.max_storage_bytes);
        effective.max_metadata_units = (std::min)(effective.max_metadata_units, limits_.max_metadata_units);
        effective.max_translations = (std::min)(effective.max_translations, limits_.max_translations);
        return read_native_metadata(path, effective);
    }

private:
    TargetScanLimits limits_;
};

} // namespace

std::unique_ptr<TargetVersionSource> make_native_target_version_source(const TargetScanLimits& limits) {
    return std::make_unique<NativeTargetVersionSource>(limits);
}

std::optional<TargetVersion> read_target_version(const std::filesystem::path& path) {
    auto result = read_native_metadata(path, TargetScanLimits{});
    return result ? std::move(*result) : std::nullopt;
}

} // namespace launcher
