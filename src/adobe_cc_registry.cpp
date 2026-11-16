#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/adobe_cc.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

namespace launcher {
namespace {

struct CloseKey { void operator()(HKEY key) const noexcept { RegCloseKey(key); } };
using RegistryKey = std::unique_ptr<std::remove_pointer_t<HKEY>, CloseKey>;

void registry_error(LSTATUS status, std::wstring_view path) {
    if (status != ERROR_SUCCESS) {
        throw std::filesystem::filesystem_error("Cannot read Adobe installation registration",
            std::filesystem::path(path), {static_cast<int>(status), std::system_category()});
    }
}

RegistryKey open_key(std::wstring_view path, REGSAM view) {
    HKEY key = nullptr;
    const auto status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, std::wstring(path).c_str(), 0, KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | view, &key);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) { return {}; }
    registry_error(status, path);
    return RegistryKey(key);
}

std::wstring text_value(HKEY key, const wchar_t* name, const TargetScanLimits& limits) {
    DWORD bytes = 0;
    constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
    auto status = RegGetValueW(key, nullptr, name, flags, nullptr, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_UNSUPPORTED_TYPE) { return {}; }
    registry_error(status, {});
    if (bytes > (limits.max_path_units + 1) * sizeof(wchar_t)) {
        throw std::system_error(make_target_scan_limit_error(TargetScanLimit::path_units));
    }
    std::wstring text(bytes / sizeof(wchar_t), L'\0');
    status = RegGetValueW(key, nullptr, name, flags, nullptr, text.data(), &bytes);
    registry_error(status, {});
    while (!text.empty() && text.back() == L'\0') { text.pop_back(); }
    if (text.find(L'\0') != std::wstring::npos) { return {}; }
    return text;
}

std::wstring folded(std::wstring text) {
    if (!text.empty()) { CharLowerBuffW(text.data(), static_cast<DWORD>(text.size())); }
    return text;
}

class RegistryLocations final {
public:
    explicit RegistryLocations(const TargetScanLimits& limits) : limits_(limits) {}

    void collect(REGSAM view) {
        constexpr auto adobe = L"SOFTWARE\\Adobe";
        walk(adobe, {}, 0, view);
        constexpr auto uninstall = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
        children(uninstall, view, [&](const auto& path, const auto&) {
            const auto key = open_key(path, view);
            if (!key) { return; }
            const auto name = text_value(key.get(), L"DisplayName", limits_);
            const auto publisher = text_value(key.get(), L"Publisher", limits_);
            if (folded(name).starts_with(L"adobe ") || folded(publisher).starts_with(L"adobe")) {
                add(name, text_value(key.get(), L"InstallLocation", limits_));
            }
        });
    }

    std::vector<AdobeCCRegistration> take() { return std::move(locations_); }

private:
    void add(const std::wstring& product, std::wstring value) {
        if (product.empty() || value.empty()) { return; }
        if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') { value = value.substr(1, value.size() - 2); }
        std::filesystem::path path(value);
        if (!path.is_absolute() || has_target_wildcards(value)) { return; }
        path = path.lexically_normal();
        while (path.has_relative_path() && path.filename().empty()) { path = path.parent_path(); }
        if (folded(path.extension().native()) == L".exe") { path = path.parent_path(); }
        // Product-specific records often identify only the currently registered
        // version. Scan its installation container to discover sibling versions.
        auto installation = path;
        if (folded(installation.filename().native()) == L"windows" && folded(installation.parent_path().filename().native()) == L"contents") {
            installation = installation.parent_path().parent_path();
        }
        if (const auto leaf = folded(installation.filename().native()); leaf == L"support files" || leaf == L"acc") {
            installation = installation.parent_path();
        }
        if (folded(canonical_adobe_cc_name(installation.filename().native())) == folded(canonical_adobe_cc_name(product))) {
            path = installation.parent_path();
        }
        const auto identity = folded(path.native());
        const auto name = canonical_adobe_cc_name(product);
        if (name.size() > limits_.max_metadata_units) { throw std::system_error(make_target_scan_limit_error(TargetScanLimit::metadata_units)); }
        if (std::ranges::any_of(locations_, [&](const auto& entry) {
                return folded(entry.location.native()) == identity && entry.product == name;
            })) { return; }
        const auto bytes = (path.native().size() + name.size()) * sizeof(wchar_t) + sizeof(AdobeCCRegistration);
        if (locations_.size() >= limits_.max_targets || bytes > limits_.max_storage_bytes - storage_) {
            throw std::system_error(make_target_scan_limit_error(TargetScanLimit::storage_bytes));
        }
        storage_ += bytes;
        locations_.push_back({name, std::move(path)});
    }

    template<class Function>
    void children(const std::wstring& path, REGSAM view, Function visit) {
        const auto key = open_key(path, view);
        if (!key) { return; }
        for (DWORD index = 0;; ++index) {
            std::array<wchar_t, 256> name{};
            DWORD length = static_cast<DWORD>(name.size());
            const auto status = RegEnumKeyExW(key.get(), index, name.data(), &length, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS) { break; }
            registry_error(status, path);
            if (++keys_ > limits_.max_entries) { throw std::system_error(make_target_scan_limit_error(TargetScanLimit::entries)); }
            const auto child = path + L"\\" + std::wstring(name.data(), length);
            if (child.size() > limits_.max_path_units) { throw std::system_error(make_target_scan_limit_error(TargetScanLimit::path_units)); }
            visit(child, std::wstring(name.data(), length));
        }
    }

    void walk(const std::wstring& path, const std::wstring& product, std::size_t depth, REGSAM view) {
        if (depth >= limits_.max_components) { throw std::system_error(make_target_scan_limit_error(TargetScanLimit::components)); }
        const auto key = open_key(path, view);
        if (!key) { return; }
        for (const auto name : {L"ApplicationPath", L"InstallPath", L"InstallDir", L"InstallLocation", L"InstallDirectory", L"Path"}) {
            add(product, text_value(key.get(), name, limits_));
        }
        const auto leaf = folded(path.substr(path.find_last_of(L'\\') + 1));
        if (leaf == L"applicationpath" || leaf == L"installpath" || leaf == L"installdir" || leaf == L"installlocation" || leaf == L"installdirectory") {
            add(product, text_value(key.get(), nullptr, limits_));
        }
        children(path, view, [&](const auto& child, const auto& name) { walk(child, product.empty() ? name : product, depth + 1, view); });
    }

    const TargetScanLimits& limits_;
    std::vector<AdobeCCRegistration> locations_;
    std::size_t keys_ = 0;
    std::size_t storage_ = 0;
};

} // namespace

std::vector<AdobeCCRegistration> adobe_cc_registrations(const TargetScanLimits& limits) {
    RegistryLocations registry(limits);
    registry.collect(KEY_WOW64_64KEY);
    registry.collect(KEY_WOW64_32KEY);
    return registry.take();
}

} // namespace launcher
