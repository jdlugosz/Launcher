#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "launcher/resource_identifier.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace launcher {

class ResourceModule final {
public:
    // Native adapter for an already selected file: maps resources as data
    // without loading executable code or searching application directories.
    explicit ResourceModule(const std::filesystem::path& path);
    // A null module handle borrows the current executable; it is never freed.
    explicit ResourceModule(std::nullptr_t) noexcept;
    ResourceModule(const ResourceModule&) = delete;
    ResourceModule& operator=(const ResourceModule&) = delete;
    ResourceModule(ResourceModule&&) noexcept = default;
    ResourceModule& operator=(ResourceModule&&) noexcept = default;

    // Views borrow mapped storage. Keep this module alive while using a view;
    // views of the current executable remain valid for the process lifetime.
    // Omitted language uses Windows selection. Explicit 0 requires a neutral entry.
    std::span<const BYTE> FetchBin(const ResourceIdentifier& type, const ResourceIdentifier& name, std::optional<std::uint16_t> language = std::nullopt);
    std::string_view FetchText(const ResourceIdentifier& type, const ResourceIdentifier& name, std::optional<std::uint16_t> language = std::nullopt);

private:
    struct ModuleCloser final {
        void operator()(HMODULE module) const noexcept;
    };
    using Module = std::unique_ptr<std::remove_pointer_t<HMODULE>, ModuleCloser>;
    struct LanguageCache final {
        ResourceIdentifier type;
        ResourceIdentifier name;
        std::vector<WORD> languages;
    };

    void require_language(HMODULE module, const ResourceIdentifier& type, const ResourceIdentifier& name, LPCWSTR native_type, LPCWSTR native_name, WORD language);

    Module module_;
    bool current_executable_ = false;
    std::optional<LanguageCache> language_cache_;
};

} // namespace launcher
