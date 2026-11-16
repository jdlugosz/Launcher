#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#if LAUNCHER_USE_JMD_MODULES && !defined(__INTELLISENSE__)
import jmd.log;
#else
#include <jmd/log.hpp>
#endif

#include "launcher/filesystem_error_log.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace launcher {
bool is_external_filesystem_error(const TargetScanDiagnostic& diagnostic) noexcept {
    if (!diagnostic.error || diagnostic.limit != TargetScanLimit::none ||
        (diagnostic.operation != TargetScanOperation::probe && diagnostic.operation != TargetScanOperation::enumerate &&
            diagnostic.operation != TargetScanOperation::metadata)) { return false; }
    return diagnostic.error.category() == std::system_category() || diagnostic.error.category() == std::generic_category();
}

namespace {

struct LocalMemoryDeleter {
    void operator()(wchar_t* value) const noexcept { LocalFree(value); }
};

void append_utf8(std::string& destination, std::uint32_t value) {
    if (value < 0x80) { destination += static_cast<char>(value); }
    else if (value < 0x800) {
        destination += static_cast<char>(0xc0 | (value >> 6));
        destination += static_cast<char>(0x80 | (value & 0x3f));
    } else if (value < 0x10000) {
        destination += static_cast<char>(0xe0 | (value >> 12));
        destination += static_cast<char>(0x80 | ((value >> 6) & 0x3f));
        destination += static_cast<char>(0x80 | (value & 0x3f));
    } else {
        destination += static_cast<char>(0xf0 | (value >> 18));
        destination += static_cast<char>(0x80 | ((value >> 12) & 0x3f));
        destination += static_cast<char>(0x80 | ((value >> 6) & 0x3f));
        destination += static_cast<char>(0x80 | (value & 0x3f));
    }
}

void append_escaped_unit(std::string& destination, std::uint32_t value) {
    constexpr std::string_view digits = "0123456789abcdef";
    destination += "\\u";
    for (int shift = 12; shift >= 0; shift -= 4) { destination += digits[(value >> shift) & 0xf]; }
}

std::string json_string(std::wstring_view value) {
    std::string result = "\"";
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto unit = static_cast<std::uint16_t>(value[index]);
        if (unit == L'"' || unit == L'\\') { result += '\\'; result += static_cast<char>(unit); }
        else if (unit < 0x20 || (unit >= 0x7f && unit <= 0x9f)) { append_escaped_unit(result, unit); }
        else if (unit >= 0xd800 && unit <= 0xdbff) {
            if (index + 1 < value.size()) {
                const auto low = static_cast<std::uint16_t>(value[index + 1]);
                if (low >= 0xdc00 && low <= 0xdfff) {
                    append_utf8(result, 0x10000u + ((unit - 0xd800u) << 10) + low - 0xdc00u);
                    ++index;
                    continue;
                }
            }
            // Windows paths are UTF-16 code-unit strings. Preserve an unpaired
            // surrogate in JSON rather than losing it to a replacement glyph.
            append_escaped_unit(result, unit);
        } else if (unit >= 0xdc00 && unit <= 0xdfff) { append_escaped_unit(result, unit); }
        else { append_utf8(result, unit); }
    }
    result += '"';
    return result;
}

std::wstring generic_message(const std::error_code& error) {
    const auto text = error.message();
    if (text.empty()) { return L"No generic error description is available."; }
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) { throw std::length_error("Generic error message is too long."); }
    const auto count = static_cast<int>(text.size());
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int size = MultiByteToWideChar(code_page, flags, text.data(), count, nullptr, 0);
    if (size == 0) {
        // Native CRT generic messages can use the active Windows ANSI page.
        code_page = CP_ACP;
        flags = 0;
        size = MultiByteToWideChar(code_page, flags, text.data(), count, nullptr, 0);
    }
    if (size == 0) { throw std::runtime_error("Cannot decode generic error message."); }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(code_page, flags, text.data(), count, result.data(), size) != size) {
        throw std::runtime_error("Cannot decode generic error message.");
    }
    return result;
}

std::wstring windows_message(const std::error_code& error) {
    wchar_t* allocation = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(error.value()), 0, reinterpret_cast<wchar_t*>(&allocation), 0, nullptr);
    const std::unique_ptr<wchar_t, LocalMemoryDeleter> buffer(allocation);
    if (size == 0) { return std::format(L"Windows has no message text for error {}.", static_cast<DWORD>(error.value())); }
    std::wstring message(buffer.get(), size);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) { message.pop_back(); }
    return message;
}

std::string record_for(const TargetScanDiagnostic& diagnostic) {
    SYSTEMTIME now{};
    GetSystemTime(&now);
    const auto timestamp = std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    const bool native = diagnostic.error.category() == std::system_category();
    const auto code = native ? std::to_string(static_cast<DWORD>(diagnostic.error.value())) : std::to_string(diagnostic.error.value());
    return std::format("{{\"timestamp\":\"{}\",\"path\":{},\"error_category\":\"{}\",\"error_code\":{},\"error_text\":{}}}\n",
        timestamp, json_string(diagnostic.path.native()), native ? "Win32" : "generic", code,
        json_string(native ? windows_message(diagnostic.error) : generic_message(diagnostic.error)));
}

} // namespace

FilesystemErrorLog::FilesystemErrorLog(std::filesystem::path absolute_path, std::uint64_t maximum_bytes, FileLookup files)
    : files_(std::move(files)), path_(std::move(absolute_path)), maximum_bytes_(maximum_bytes) {
    if (!path_.is_absolute() || path_.filename().empty() || path_.native().find(L'\0') != std::wstring::npos || maximum_bytes_ == 0) {
        throw std::invalid_argument("Filesystem error log requires an absolute file path and a positive size budget.");
    }
}

bool FilesystemErrorLog::append(std::span<const TargetScanDiagnostic> diagnostics) noexcept {
    try {
        // A healthy scan must not queue behind another worker's slow journal I/O.
        if (std::none_of(diagnostics.begin(), diagnostics.end(), is_external_filesystem_error)) { return true; }
        const std::lock_guard lock(mutex_);
        const auto path = files_.exact_path(path_);
        for (const auto& diagnostic : diagnostics) {
            if (is_external_filesystem_error(diagnostic)
                && !jmd::append_rotating_log(path, record_for(diagnostic), {.maximum_bytes = maximum_bytes_})) { return false; }
        }
        return true;
    } catch (...) { return false; }
}

} // namespace launcher
