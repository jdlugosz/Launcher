#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/resource_module.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace launcher {
namespace {

[[noreturn]] void resource_error(DWORD code, std::string_view action) {
    if (code == ERROR_SUCCESS) { code = ERROR_GEN_FAILURE; }
    throw std::system_error(std::error_code(static_cast<int>(code), std::system_category()), std::string(action));
}

class NativeIdentifier final {
public:
    explicit NativeIdentifier(const ResourceIdentifier& value) {
        if (const auto* id = std::get_if<std::uint16_t>(&value)) {
            id_ = *id;
            return;
        }
        const auto& text = std::get<std::string>(value);
        if (text.empty() || text.find('\0') != std::string::npos) {
            resource_error(ERROR_INVALID_NAME, "Resource names must be nonempty and contain no NUL");
        }
        if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            resource_error(ERROR_INVALID_PARAMETER, "Resource name is too long");
        }
        const auto length = static_cast<int>(text.size());
        const auto needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
        if (needed == 0) {
            const auto error = GetLastError();
            resource_error(error, "Cannot convert resource name from UTF-8");
        }
        text_.resize(static_cast<std::size_t>(needed));
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, text_.data(), needed) == 0) {
            const auto error = GetLastError();
            resource_error(error, "Cannot convert resource name from UTF-8");
        }
    }

    LPCWSTR get() const noexcept { return id_ ? MAKEINTRESOURCEW(*id_) : text_.c_str(); }

private:
    std::optional<std::uint16_t> id_;
    std::wstring text_;
};

struct LanguageCollection final {
    std::vector<WORD> languages;
    std::exception_ptr failure;
};

BOOL CALLBACK collect_language(HMODULE, LPCWSTR, LPCWSTR, WORD language, LONG_PTR parameter) noexcept {
    auto& collection = *reinterpret_cast<LanguageCollection*>(parameter);
    try {
        collection.languages.push_back(language);
        return TRUE;
    } catch (...) {
        collection.failure = std::current_exception();
        return FALSE;
    }
}

} // namespace

void ResourceModule::ModuleCloser::operator()(HMODULE module) const noexcept {
    FreeLibrary(module);
}

ResourceModule::ResourceModule(const std::filesystem::path& path)
    : module_(LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {
    if (!module_) {
        const auto error = GetLastError();
        resource_error(error, "Cannot map resource module");
    }
}

ResourceModule::ResourceModule(std::nullptr_t) noexcept : current_executable_(true) {}

void ResourceModule::require_language(HMODULE module, const ResourceIdentifier& type, const ResourceIdentifier& name, LPCWSTR native_type, LPCWSTR native_name, WORD language) {
    if (!language_cache_ || language_cache_->type != type || language_cache_->name != name) {
        LanguageCollection collection;
        const auto enumerated = EnumResourceLanguagesW(module, native_type, native_name, collect_language, reinterpret_cast<LONG_PTR>(&collection));
        const auto error = enumerated ? ERROR_SUCCESS : GetLastError();
        if (collection.failure) { std::rethrow_exception(collection.failure); }
        if (!enumerated) { resource_error(error, "Cannot enumerate resource languages"); }
        language_cache_ = LanguageCache{type, name, std::move(collection.languages)};
    }
    if (std::find(language_cache_->languages.begin(), language_cache_->languages.end(), language) == language_cache_->languages.end()) {
        resource_error(ERROR_RESOURCE_LANG_NOT_FOUND, "Requested resource language does not exist");
    }
}

std::span<const BYTE> ResourceModule::FetchBin(const ResourceIdentifier& type, const ResourceIdentifier& name, std::optional<std::uint16_t> language) {
    if (!current_executable_ && !module_) { resource_error(ERROR_INVALID_HANDLE, "Resource module has been moved from"); }
    const auto module = module_.get();
    const NativeIdentifier native_type(type);
    const NativeIdentifier native_name(name);
    if (language) { require_language(module, type, name, native_type.get(), native_name.get(), *language); }
    const auto resource = language ? FindResourceExW(module, native_type.get(), native_name.get(), *language)
        : FindResourceW(module, native_name.get(), native_type.get());
    if (!resource) {
        const auto error = GetLastError();
        resource_error(error, "Cannot find resource");
    }
    const auto size = SizeofResource(module, resource);
    if (size == 0) { resource_error(ERROR_RESOURCE_DATA_NOT_FOUND, "Resource has no data"); }
    const auto loaded = LoadResource(module, resource);
    if (!loaded) {
        const auto error = GetLastError();
        resource_error(error, "Cannot load resource");
    }
    const auto* bytes = static_cast<const BYTE*>(LockResource(loaded));
    if (!bytes) { resource_error(ERROR_RESOURCE_DATA_NOT_FOUND, "Cannot access resource data"); }
    return {bytes, static_cast<std::size_t>(size)};
}

std::string_view ResourceModule::FetchText(const ResourceIdentifier& type, const ResourceIdentifier& name, std::optional<std::uint16_t> language) {
    const auto bytes = FetchBin(type, name, language);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace launcher
