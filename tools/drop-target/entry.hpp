#pragma once

#include <windows.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace launcher::drop {

inline DWORD drop_effect(DWORD offered, bool accepted) noexcept {
    if (!accepted) { return DROPEFFECT_NONE; }
    if (offered & DROPEFFECT_COPY) { return DROPEFFECT_COPY; }
    if (offered & DROPEFFECT_LINK) { return DROPEFFECT_LINK; }
    return DROPEFFECT_NONE;
}

struct TaskMemoryDeleter {
    void operator()(wchar_t* value) const noexcept { CoTaskMemFree(value); }
};
using TaskString = std::unique_ptr<wchar_t, TaskMemoryDeleter>;

inline std::wstring shell_target(IShellItem* item) {
    PWSTR name = nullptr;
    const HRESULT path_result = item->GetDisplayName(SIGDN_FILESYSPATH, &name);
    TaskString path(name);
    if (SUCCEEDED(path_result) && path && *path) { return path.get(); }

    name = nullptr;
    const HRESULT result = item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &name);
    TaskString parsing_name(name);
    if (FAILED(result) || !parsing_name || !*parsing_name) {
        throw std::runtime_error("This item has no reusable Windows Shell name.");
    }
    std::wstring target(parsing_name.get());
    if (!target.starts_with(L"::") && !target.starts_with(L"shell:") && target.find(L':') == target.npos) {
        throw std::runtime_error("This item's Shell name cannot be used as a Launcher target.");
    }
    // A display caption is insufficient: require a name Windows can parse back.
    Microsoft::WRL::ComPtr<IShellItem> recovered;
    if (FAILED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&recovered)))) {
        throw std::runtime_error("Windows cannot reopen this item by its Shell name.");
    }
    int order = 0;
    if (FAILED(item->Compare(recovered.Get(), SICHINT_CANONICAL, &order)) || order != 0) {
        throw std::runtime_error("This item's Shell name does not identify the same item.");
    }
    return target;
}

inline std::wstring format_entry(std::wstring_view target) {
    if (target.empty() || target.find(L'\0') != target.npos) {
        throw std::runtime_error("The target is empty or contains a NUL character.");
    }
    bool basic = false;
    for (const wchar_t ch : target) {
        if (ch == L'\'' || ch < 0x20 || ch == 0x7f) { basic = true; }
    }
    std::wstring result = L"    { target = ";
    result += basic ? L'"' : L'\'';
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (const wchar_t ch : target) {
        if (ch == L'$') { result += L"$$"; }
        else if (ch == L'%') { result += L"$/"; }
        else if (basic && (ch == L'\\' || ch == L'"')) { result += L'\\'; result += ch; }
        else if (basic && (ch < 0x20 || ch == 0x7f)) {
            result += L"\\u00";
            result += hex[(ch >> 4) & 15];
            result += hex[ch & 15];
        } else { result += ch; }
    }
    result += basic ? L'"' : L'\'';
    result += L" },\r\n";
    return result;
}

} // namespace launcher::drop
