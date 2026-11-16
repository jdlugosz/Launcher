#pragma once

#include <windows.h>

#include <climits>
#include <string>
#include <string_view>
#include <system_error>

namespace launcher {

inline std::string expand_environment_text(std::string_view text) {
    if (text.empty()) { return {}; }
    if (text.size() > INT_MAX || text.find('\0') != text.npos) {
        throw std::system_error(ERROR_INVALID_PARAMETER, std::system_category(), "Environment text");
    }
    const int length = static_cast<int>(text.size());
    const int wide_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (!wide_size) { throw std::system_error(GetLastError(), std::system_category(), "Environment text UTF-8"); }
    std::wstring source(wide_size, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, source.data(), wide_size)) {
        throw std::system_error(GetLastError(), std::system_category(), "Environment text UTF-8");
    }
    const DWORD size = ExpandEnvironmentStringsW(source.c_str(), nullptr, 0);
    if (!size) { throw std::system_error(GetLastError(), std::system_category(), "ExpandEnvironmentStringsW"); }
    std::wstring wide(size, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(source.c_str(), wide.data(), size);
    if (!written || written > size) {
        throw std::system_error(written ? ERROR_INSUFFICIENT_BUFFER : GetLastError(), std::system_category(), "ExpandEnvironmentStringsW");
    }
    wide.resize(written - 1);
    const int utf8_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (!utf8_size && !wide.empty()) { throw std::system_error(GetLastError(), std::system_category(), "Expanded environment UTF-8"); }
    std::string result(utf8_size, '\0');
    if (utf8_size && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), result.data(), utf8_size, nullptr, nullptr)) {
        throw std::system_error(GetLastError(), std::system_category(), "Expanded environment UTF-8");
    }
    return result;
}

} // namespace launcher
