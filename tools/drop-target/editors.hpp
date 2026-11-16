#pragma once

#include <windows.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace launcher::drop {

struct Editor {
    std::wstring name;
    std::wstring executable;
};

inline std::wstring editor_path(std::wstring path) {
    // App Paths values are filenames, not command lines. Some installers quote them.
    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') { path = path.substr(1, path.size() - 2); }
    if (path.empty() || path.find_first_of(L"\"\r\n") != path.npos || path.find(L'\0') != path.npos) { return {}; }
    const bool absolute = (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) || path.starts_with(L"\\\\");
    if (!absolute || path.size() < 4 || _wcsicmp(path.c_str() + path.size() - 4, L".exe") != 0) { return {}; }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY) ? path : std::wstring{};
}

inline std::wstring registered_editor(const wchar_t* executable, HKEY root, REGSAM view) {
    const std::wstring key = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + std::wstring(executable);
    const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | view;
    DWORD bytes = 0;
    if (RegGetValueW(root, key.c_str(), nullptr, flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes > 65536) { return {}; }
    std::wstring path(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(root, key.c_str(), nullptr, flags, nullptr, path.data(), &bytes) != ERROR_SUCCESS) { return {}; }
    path.resize(path.find(L'\0'));
    return editor_path(std::move(path));
}

inline std::wstring environment_value(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) { return {}; }
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    if (!written || written >= size) { return {}; }
    value.resize(written);
    return value;
}

inline std::wstring editor_on_path(const wchar_t* executable, const std::wstring& path) {
    if (path.empty()) { return {}; }
    std::wstring found(32768, L'\0');
    const DWORD size = SearchPathW(path.c_str(), executable, nullptr, static_cast<DWORD>(found.size()), found.data(), nullptr);
    if (!size || size >= found.size()) { return {}; }
    found.resize(size);
    return editor_path(std::move(found));
}

inline std::wstring find_editor(const wchar_t* executable) {
    for (const HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (const REGSAM view : {RRF_SUBKEY_WOW6464KEY, RRF_SUBKEY_WOW6432KEY}) {
            if (auto path = registered_editor(executable, root, view); !path.empty()) { return path; }
        }
    }
    if (auto path = editor_on_path(executable, environment_value(L"PATH")); !path.empty()) { return path; }
    // ZIP/portable editions need not register anything; cover common locations too.
    struct Location { const wchar_t* executable; const wchar_t* folder; };
    constexpr Location locations[]{
        {L"notepad++.exe", L"Notepad++"}, {L"Code.exe", L"Microsoft VS Code"},
        {L"sublime_text.exe", L"Sublime Text"}, {L"sublime_text.exe", L"Sublime Text 3"},
        {L"Notepad3.exe", L"Notepad3"}, {L"Notepad4.exe", L"Notepad4"}
    };
    const auto local = environment_value(L"LOCALAPPDATA");
    const auto user_programs = local.empty() ? std::wstring{} : local + L"\\Programs";
    for (const auto& location : locations) {
        if (_wcsicmp(executable, location.executable) != 0) { continue; }
        for (const auto& root : {environment_value(L"ProgramFiles"), environment_value(L"ProgramFiles(x86)"), user_programs}) {
            if (root.empty()) { continue; }
            if (auto path = editor_path(root + L"\\" + location.folder + L"\\" + executable); !path.empty()) { return path; }
        }
    }
    return {};
}

template<class Lookup>
std::vector<Editor> discover_editors(Lookup lookup) {
    struct Candidate { const wchar_t* name; const wchar_t* executable; };
    constexpr Candidate candidates[]{
        {L"Notepad++", L"notepad++.exe"},
        {L"Visual Studio Code", L"Code.exe"},
        {L"Sublime Text", L"sublime_text.exe"},
        {L"Notepad3", L"Notepad3.exe"},
        {L"Notepad4", L"Notepad4.exe"}
    };
    std::vector<Editor> editors;
    for (const auto& candidate : candidates) {
        if (auto path = lookup(candidate.executable); !path.empty()) { editors.push_back({candidate.name, std::move(path)}); }
    }
    return editors;
}

} // namespace launcher::drop
