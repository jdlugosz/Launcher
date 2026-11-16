#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/adobe_cc.hpp"
#include "launcher/config.hpp"
#include "launcher/version_discovery.hpp"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace launcher {
namespace {

std::wstring lower(std::wstring_view text) {
    std::wstring result(text);
    if (!result.empty()) { CharLowerBuffW(result.data(), static_cast<DWORD>(result.size())); }
    return result;
}

std::wstring trim(std::wstring_view text) {
    const auto first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring_view::npos) { return {}; }
    const auto last = text.find_last_not_of(L" \t\r\n");
    return std::wstring(text.substr(first, last - first + 1));
}

bool is_beta(std::wstring_view text) {
    return lower(text).find(L"(beta)") != std::wstring::npos;
}

bool executable_target(std::wstring_view text) {
    return lower(text).ends_with(L".exe");
}

// Case-insensitive filename glob with * and ?, using no path separators.
bool executable_matches(std::wstring_view pattern, std::wstring_view filename) {
    const auto p = lower(pattern);
    const auto f = lower(filename);
    std::size_t index = 0, pos = 0, star = std::wstring::npos, retry = 0;
    while (pos < f.size()) {
        if (index < p.size() && (p[index] == L'?' || p[index] == f[pos])) { ++index; ++pos; }
        else if (index < p.size() && p[index] == L'*') { star = index++; retry = pos; }
        else if (star != std::wstring::npos) { index = star + 1; pos = ++retry; }
        else { return false; }
    }
    while (index < p.size() && p[index] == L'*') { ++index; }
    return index == p.size();
}

std::wstring product_key(std::wstring_view product) {
    auto text = lower(canonical_adobe_cc_name(product));
    if (text.starts_with(L"adobe ")) { text.erase(0, 6); }
    return text;
}

std::wstring executable_family_key(std::wstring_view product) {
    auto key = product_key(product);
    std::erase_if(key, [](wchar_t unit) { return unit == L' ' || unit == L'-' || unit == L'_'; });
    return key;
}

bool main_executable(const std::filesystem::path& path) {
    constexpr std::wstring_view names[] = {
        L"afterfx.exe", L"afterfx (beta).exe", L"adobe audition.exe",
        L"adobe audition (beta).exe", L"adobe bridge.exe", L"illustrator.exe",
        L"indesign.exe", L"lightroom.exe", L"adobe media encoder.exe",
        L"adobe media encoder (beta).exe", L"photoshop.exe",
        L"adobe premiere pro.exe", L"adobe premiere pro (beta).exe",
        L"acrobat.exe", L"creative cloud.exe"
    };
    const auto filename = lower(path.filename().wstring());
    return std::ranges::find(names, filename) != std::end(names);
}

std::error_code native_error(DWORD error) {
    return {static_cast<int>(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error), std::system_category()};
}

bool missing(const std::error_code& error) {
    if (error.category() == std::system_category()) {
        return error.value() == ERROR_FILE_NOT_FOUND || error.value() == ERROR_PATH_NOT_FOUND;
    }
    return error == std::errc::no_such_file_or_directory;
}

std::filesystem::path scan_anchor(const std::filesystem::path& root) {
    const auto& text = root.native();
    std::size_t first = std::wstring::npos;
    if (text.size() >= 8 && CompareStringOrdinal(text.data(), 8, L"\\\\?\\UNC\\", 8, TRUE) == CSTR_EQUAL) {
        first = 8;
    } else if (text.starts_with(L"\\\\") && !text.starts_with(L"\\\\?\\") && !text.starts_with(L"\\\\.\\")) {
        first = 2;
    }
    if (first != std::wstring::npos) {
        const auto server_end = text.find(L'\\', first);
        const auto share_end = server_end == std::wstring::npos ? server_end : text.find(L'\\', server_end + 1);
        return text.substr(0, share_end);
    }
    return root.root_path();
}

TargetVersionScanResult normalization_failure(const std::filesystem::path& path, std::error_code error, const TargetScanLimits& limits) {
    TargetVersionScanResult result;
    result.status = TargetScanStatus::failed;
    if (limits.max_diagnostics) {
        std::filesystem::path diagnostic_path(path.native().substr(0, limits.max_diagnostic_path_units));
        const auto bytes = diagnostic_path.native().capacity() * sizeof(wchar_t);
        if (bytes <= limits.max_storage_bytes && sizeof(TargetScanDiagnostic) <= limits.max_storage_bytes - bytes) {
            result.diagnostics.push_back({TargetScanOperation::normalize, std::move(diagnostic_path), error});
            return result;
        }
        result.exhausted_limit = TargetScanLimit::storage_bytes;
    }
    ++result.omitted_diagnostics;
    return result;
}

class AdobeScan final {
public:
    AdobeScan(std::filesystem::path root, TargetVersionSource& source, const TargetScanLimits& limits, std::wstring_view executable_pattern = {})
        : root_(std::move(root)), anchor_(scan_anchor(root_)), source_(source), limits_(limits), executable_pattern_(executable_pattern) {}

    TargetVersionScanResult run(std::span<const AdobeCCRegistration> registrations = {}) {
        if (!check_path(root_) || !storage((root_.native().capacity() + anchor_.native().capacity()) * sizeof(wchar_t), root_)) {
            return take_result();
        }
        std::size_t components = 0;
        for (const auto& ignored : root_) {
            (void)ignored;
            if (components == limits_.max_components) { limit(TargetScanLimit::components, root_); return take_result(); }
            ++components;
        }
        if (directory(root_, true)) {
            // Adobe main programs occupy fixed entry layouts. Never recursively
            // inspect asset/plugin trees or follow links into another tree.
            scan_directory(root_, true);
            scan_suffixes(root_);
        }
        if (registrations.size() > limits_.max_targets) { limit(TargetScanLimit::targets, root_); return take_result(); }
        for (const auto& registration : registrations) {
            if (exhausted_) { break; }
            if (registration.product.size() > limits_.max_metadata_units) { limit(TargetScanLimit::metadata_units, root_); break; }
            auto path = registration.location.lexically_normal();
            if (!path.is_absolute() || has_target_wildcards(path.native()) || path.native().find(L'\0') != std::wstring::npos) {
                error(TargetScanOperation::normalize, path, std::make_error_code(std::errc::invalid_argument));
                continue;
            }
            anchor_ = scan_anchor(path);
            std::size_t registered_components = 0;
            for (const auto& component : path) {
                (void)component;
                if (++registered_components > limits_.max_components) { limit(TargetScanLimit::components, path); break; }
            }
            if (exhausted_) { break; }
            registered_product_ = registration.product;
            if (lower(path.extension().native()) == L".exe") { path = path.parent_path(); }
            if (directory(path)) {
                scan_directory(path, true, registration.recursive, registered_components);
                if (!registration.recursive) { scan_suffixes(path); }
            }
        }
        return take_result();
    }

private:
    TargetVersionScanResult take_result() {
        sort_distinct_target_versions(result_.versions);
        return std::move(result_);
    }

    void error(TargetScanOperation operation, const std::filesystem::path& path, std::error_code code, bool failed = false) {
        const auto named_limit = target_scan_limit(code);
        if (named_limit != TargetScanLimit::none) {
            exhausted_ = true;
            result_.status = TargetScanStatus::incomplete;
            if (result_.exhausted_limit == TargetScanLimit::none) { result_.exhausted_limit = named_limit; }
        } else if (result_.status != TargetScanStatus::failed) {
            result_.status = failed ? TargetScanStatus::failed : TargetScanStatus::incomplete;
        }
        if (result_.diagnostics.size() < limits_.max_diagnostics) {
            std::filesystem::path diagnostic_path(path.native().substr(0, limits_.max_diagnostic_path_units));
            const auto capacity = result_.diagnostics.capacity();
            const auto next = result_.diagnostics.size() < capacity ? capacity :
                capacity + (std::min)((std::max)(capacity / 2, std::size_t{1}), limits_.max_diagnostics - capacity);
            const auto available = limits_.max_storage_bytes - (std::min)(storage_bytes_, limits_.max_storage_bytes);
            const auto path_bytes = diagnostic_path.native().capacity() * sizeof(wchar_t);
            if (path_bytes <= available && next - capacity <= (available - path_bytes) / sizeof(TargetScanDiagnostic)) {
                result_.diagnostics.reserve(next);
                const auto actual = result_.diagnostics.capacity();
                if (actual - capacity <= (available - path_bytes) / sizeof(TargetScanDiagnostic)) {
                    storage_bytes_ += path_bytes + (actual - capacity) * sizeof(TargetScanDiagnostic);
                    result_.diagnostics.push_back({named_limit == TargetScanLimit::none ? operation : TargetScanOperation::budget,
                        std::move(diagnostic_path), code ? code : native_error(ERROR_GEN_FAILURE), named_limit});
                    return;
                }
            }
            if (result_.exhausted_limit == TargetScanLimit::none) { result_.exhausted_limit = TargetScanLimit::storage_bytes; }
            exhausted_ = true;
        }
        if (result_.omitted_diagnostics != (std::numeric_limits<std::size_t>::max)()) {
            ++result_.omitted_diagnostics;
        }
    }

    bool limit(TargetScanLimit value, const std::filesystem::path& path) {
        if (!exhausted_) { error(TargetScanOperation::budget, path, make_target_scan_limit_error(value)); }
        exhausted_ = true;
        return false;
    }

    bool storage(std::size_t bytes, const std::filesystem::path& path) {
        if (bytes > limits_.max_storage_bytes - (std::min)(storage_bytes_, limits_.max_storage_bytes)) {
            return limit(TargetScanLimit::storage_bytes, path);
        }
        storage_bytes_ += bytes;
        return true;
    }

    bool check_path(const std::filesystem::path& path) {
        return path.native().size() <= limits_.max_path_units || limit(TargetScanLimit::path_units, path);
    }

    bool operation(const std::filesystem::path& path) {
        if (exhausted_) { return false; }
        if (operations_ == limits_.max_operations) { return limit(TargetScanLimit::operations, path); }
        ++operations_;
        return true;
    }

    template<class T>
    bool reserve_one(std::vector<T>& values, const std::filesystem::path& path) {
        if (values.size() < values.capacity()) { return true; }
        const auto capacity = values.capacity();
        const auto growth = (std::min)((std::max)(capacity / 2, std::size_t{1}),
            (std::numeric_limits<std::size_t>::max)() / sizeof(T) - capacity);
        if (!growth) { return limit(TargetScanLimit::storage_bytes, path); }
        if (!storage(growth * sizeof(T), path)) { return false; }
        values.reserve(capacity + growth);
        return storage((values.capacity() - capacity - growth) * sizeof(T), path);
    }

    bool remember_executable(const std::filesystem::path& path) {
        auto key = lower(path.lexically_normal().native());
        const auto found = std::ranges::lower_bound(executables_, key);
        if (found != executables_.end() && *found == key) { return false; }
        const auto position = static_cast<std::size_t>(found - executables_.begin());
        if (!reserve_one(executables_, path) || !storage(key.capacity() * sizeof(wchar_t), path)) { return false; }
        executables_.insert(executables_.begin() + position, std::move(key));
        return true;
    }

    // A missing descendant is absence only while a parent still answers. Never
    // climb beyond a drive/share anchor and turn an unavailable mount into empty.
    bool expected_absence(const std::filesystem::path& path, std::error_code code, TargetScanOperation action, bool failed) {
        if (!missing(code)) { error(action, path, code, failed); return false; }
        auto ancestor = path.parent_path();
        while (!ancestor.empty() && ancestor.native().size() >= anchor_.native().size()) {
            if (!operation(ancestor)) { return false; }
            const auto probe = source_.probe(ancestor);
            if (probe) {
                if (*probe == TargetFileKind::directory) { return true; }
                error(TargetScanOperation::probe, ancestor, native_error(ERROR_DIRECTORY), failed);
                return false;
            }
            if (!missing(probe.error()) || ancestor == anchor_) {
                error(TargetScanOperation::probe, ancestor, probe.error(), failed);
                return false;
            }
            const auto parent = ancestor.parent_path();
            if (parent == ancestor) { break; }
            ancestor = parent;
        }
        error(action, path, code, failed);
        return false;
    }

    bool directory(const std::filesystem::path& path, bool root = false) {
        if (!check_path(path) || !operation(path)) { return false; }
        const auto probe = source_.probe_no_follow(path);
        if (!probe) { expected_absence(path, probe.error(), TargetScanOperation::probe, root); return false; }
        return *probe == TargetFileKind::directory;
    }

    void read_installation(const std::filesystem::path& path) {
        if (std::ranges::binary_search(executables_, lower(path.lexically_normal().native()))) { return; }
        if (!check_path(path) || !operation(path)) { return; }
        const auto probe = source_.probe_no_follow(path);
        if (!probe) { expected_absence(path, probe.error(), TargetScanOperation::probe, false); return; }
        if (*probe != TargetFileKind::regular_file || !operation(path)) { return; }
        auto remaining = limits_;
        remaining.max_storage_bytes -= (std::min)(storage_bytes_, remaining.max_storage_bytes);
        auto metadata = source_.read_metadata(path, remaining);
        if (!metadata) {
            if (missing(metadata.error()) && operation(path)) {
                const auto present = source_.probe_no_follow(path);
                if (!present) { expected_absence(path, present.error(), TargetScanOperation::probe, false); return; }
                if (*present != TargetFileKind::regular_file) { return; }
            }
            error(TargetScanOperation::metadata, path, metadata.error());
            return;
        }
        const bool explicit_match = !executable_pattern_.empty() && executable_matches(executable_pattern_, path.filename().native());
        if ((!*metadata || (*metadata)->product_name.empty()) && !explicit_match) { return; }
        auto candidate = *metadata ? std::move(**metadata) : TargetVersion{};
        for (const auto* text : {&candidate.name, &candidate.product_name, &candidate.product_version,
                &candidate.file_version, &candidate.target_pattern, &candidate.path_label}) {
            if (text->size() > limits_.max_metadata_units) { limit(TargetScanLimit::metadata_units, path); return; }
        }
        candidate.executable = path;
        candidate.name = candidate.product_name.empty() ? path.stem().wstring() : canonical_adobe_cc_name(candidate.product_name);
        if (candidate.name.empty()) { return; }
        // Registered products are open-ended. Recognize their main executable
        // by its family name, without admitting crash reporters and uninstallers
        // that share the application's ProductName resource.
        if (!explicit_match && !main_executable(path)) {
            if (registered_product_.empty() || executable_family_key(path.stem().native()) != executable_family_key(registered_product_)) { return; }
        }
        if (!remember_executable(path)) { return; }
        if (candidate.name.size() > limits_.max_metadata_units) { limit(TargetScanLimit::metadata_units, path); return; }
        candidate.beta = is_beta(candidate.product_name) || is_beta(path.native());
        candidate.target_pattern = explicit_match ? executable_pattern_ : L"";
        if (explicit_match) { candidate.path_label = path.parent_path().filename().wstring(); }
        if (result_.versions.size() == limits_.max_targets) { limit(TargetScanLimit::targets, path); return; }
        std::size_t text_bytes = candidate.executable.native().capacity() * sizeof(wchar_t);
        for (const auto* text : {&candidate.name, &candidate.product_name, &candidate.product_version,
                &candidate.file_version, &candidate.target_pattern, &candidate.path_label}) {
            const auto capacity = text->capacity();
            if (capacity > (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t) - text_bytes / sizeof(wchar_t)) {
                limit(TargetScanLimit::storage_bytes, path); return;
            }
            text_bytes += capacity * sizeof(wchar_t);
        }
        if (!storage(text_bytes, path)) { return; }
        if (!reserve_one(result_.versions, path)) { return; }
        result_.versions.push_back(std::move(candidate));
    }

    void scan_directory(const std::filesystem::path& directory, bool installation_children, bool tree = false, std::size_t depth = 0) {
        if (exhausted_) { return; }
        if (tree && depth > limits_.max_components) { limit(TargetScanLimit::components, directory); return; }
        bool main_found = false;
        if (tree) {
            // Check Adobe's known main-program layouts before considering a
            // general descent, so Illustrator's Required/UXP assets, for example,
            // are never visited ahead of Support Files/Contents/Windows.
            scan_suffixes(directory);
            for (const auto& parent : {directory / L"Support Files", directory / L"Support Files" / L"Contents" / L"Windows",
                    directory / L"Acrobat", directory / L"ACC"}) {
                const auto identity = lower(parent.lexically_normal().native());
                main_found = main_found || std::ranges::any_of(executables_, [&](const auto& executable) {
                    return std::filesystem::path(executable).parent_path().native() == identity;
                });
            }
            if (exhausted_) { return; }
        }
        if (branches_ == limits_.max_branches) { limit(TargetScanLimit::branches, directory); return; }
        ++branches_;
        const auto pattern = directory / L"*";
        if (!check_path(pattern) || !operation(pattern)) { return; }
        auto cursor = source_.enumerate(pattern);
        if (!cursor) { expected_absence(pattern, cursor.error(), TargetScanOperation::enumerate, false); return; }
        if (!*cursor) { error(TargetScanOperation::enumerate, pattern, native_error(ERROR_INVALID_HANDLE)); return; }
        std::vector<std::filesystem::path> descendants;
        while (!exhausted_ && operation(pattern)) {
            auto step = (*cursor)->next();
            if (!step) { error(TargetScanOperation::enumerate, pattern, step.error()); break; }
            if (!*step) { break; }
            if (entries_ == limits_.max_entries) { limit(TargetScanLimit::entries, pattern); break; }
            ++entries_;
            const auto& entry = **step;
            if (entry.name == L"." || entry.name == L".." || entry.reparse_point) { continue; }
            // Adapter names are one directory component, never a path that can
            // escape the fixed Adobe layouts in a fault-injection scenario.
            if (entry.name.empty() || entry.name.find_first_of(L"\\/\0", 0, 3) != std::wstring::npos) {
                error(TargetScanOperation::enumerate, directory, native_error(ERROR_INVALID_NAME)); continue;
            }
            if (entry.name.size() >= limits_.max_path_units - (std::min)(directory.native().size(), limits_.max_path_units)) {
                limit(TargetScanLimit::path_units, directory); break;
            }
            const auto path = directory / entry.name;
            if (!check_path(path)) { break; }
            if ((!executable_pattern_.empty() && executable_matches(executable_pattern_, path.filename().native())) || main_executable(path) || (!registered_product_.empty() && lower(path.extension().native()) == L".exe" &&
                    executable_family_key(path.stem().native()) == executable_family_key(registered_product_))) {
                read_installation(path);
                main_found = main_found || std::ranges::binary_search(executables_, lower(path.lexically_normal().native()));
            }
            if (installation_children && entry.kind == TargetFileKind::directory && this->directory(path)) {
                if (tree && !main_found) {
                    if (!reserve_one(descendants, path) || !storage(path.native().capacity() * sizeof(wchar_t), path)) { break; }
                    descendants.push_back(path);
                } else if (!tree) {
                    scan_directory(path, false);
                    scan_suffixes(path);
                }
            }
        }
        // Once a main executable is found, its asset/plugin tree is irrelevant.
        // Close the enumeration before descending to bound open search handles.
        cursor->reset();
        if (tree && !main_found) {
            for (const auto& child : descendants) { scan_directory(child, true, true, depth + 1); }
        }
    }

    void scan_suffixes(const std::filesystem::path& installation_root) {
        if (exhausted_) { return; }
        const auto support = installation_root / L"Support Files";
        if (directory(support)) {
            scan_directory(support, false);
            const auto contents = support / L"Contents";
            if (directory(contents) && directory(contents / L"Windows")) {
                scan_directory(contents / L"Windows", false);
            }
        }
        for (const auto suffix : {L"Acrobat", L"ACC"}) {
            const auto path = installation_root / suffix;
            if (directory(path)) { scan_directory(path, false); }
        }
    }

    std::filesystem::path root_;
    std::filesystem::path anchor_;
    TargetVersionSource& source_;
    const TargetScanLimits& limits_;
    TargetVersionScanResult result_;
    std::vector<std::wstring> executables_;
    std::wstring registered_product_;
    std::wstring executable_pattern_;
    std::size_t operations_ = 0;
    std::size_t entries_ = 0;
    std::size_t branches_ = 0;
    std::size_t storage_bytes_ = 0;
    bool exhausted_ = false;
};

} // namespace

std::wstring canonical_adobe_cc_name(std::wstring_view product) {
    auto text = trim(product);
    auto folded = lower(text);
    if (const auto beta = folded.find(L"(beta)"); beta != std::wstring::npos) {
        text.erase(beta, 6);
        text = trim(text);
    }
    const auto last_space = text.find_last_of(L' ');
    const auto year = last_space == std::wstring::npos ? std::wstring_view(text) :
        std::wstring_view(text).substr(last_space + 1);
    if (year.size() == 4 && std::ranges::all_of(year, [](wchar_t digit) {
            return digit >= L'0' && digit <= L'9';
        }) && year[0] >= L'1' && year[0] <= L'2') {
        text = last_space == std::wstring::npos ? L"" : trim(std::wstring_view(text).substr(0, last_space));
    }
    folded = lower(text);
    if (folded.starts_with(L"adobe ")) { folded.erase(0, 6); }
    constexpr std::pair<std::wstring_view, std::wstring_view> products[] = {
        {L"after effects", L"Adobe After Effects"}, {L"audition", L"Adobe Audition"},
        {L"bridge", L"Adobe Bridge"}, {L"illustrator", L"Adobe Illustrator"},
        {L"indesign", L"Adobe InDesign"}, {L"lightroom classic", L"Adobe Lightroom Classic"},
        {L"lightroom", L"Adobe Lightroom"}, {L"media encoder", L"Adobe Media Encoder"},
        {L"photoshop", L"Adobe Photoshop"}, {L"premiere", L"Adobe Premiere"},
        {L"premiere pro", L"Adobe Premiere"}, {L"acrobat", L"Adobe Acrobat"},
        {L"creative cloud", L"Creative Cloud Desktop"},
        {L"creative cloud desktop", L"Creative Cloud Desktop"}
    };
    for (const auto& [key, name] : products) {
        if (folded == key) { return std::wstring(name); }
    }
    return text;
}

TargetVersionScanResult scan_adobe_cc_detailed(std::filesystem::path root, TargetVersionSource& source,
        const TargetScanLimits& limits) {
    return scan_adobe_cc_detailed(std::move(root), source, limits, {});
}

TargetVersionScanResult scan_adobe_cc_detailed(std::filesystem::path root, TargetVersionSource& source,
        const TargetScanLimits& limits, std::span<const AdobeCCRegistration> registrations) {
    return scan_adobe_cc_detailed(std::move(root), source, limits, registrations, {});
}

TargetVersionScanResult scan_adobe_cc_detailed(std::filesystem::path root, TargetVersionSource& source,
        const TargetScanLimits& limits, std::span<const AdobeCCRegistration> registrations, std::wstring_view executable_pattern) {
    if (!executable_pattern.empty() && (executable_pattern.size() > limits.max_metadata_units ||
            executable_pattern.find_first_of(L"\\/:") != std::wstring_view::npos || !executable_target(executable_pattern))) {
        return normalization_failure({}, std::make_error_code(std::errc::invalid_argument), limits);
    }
    if (root.empty()) {
        PWSTR folder = nullptr;
        const HRESULT status = SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &folder);
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned_folder(folder, CoTaskMemFree);
        if (FAILED(status) || !folder) {
            return normalization_failure({}, {FAILED(status) ? static_cast<int>(status) : ERROR_PATH_NOT_FOUND,
                std::system_category()}, limits);
        }
        root = std::filesystem::path(folder) / L"Adobe";
    }
    std::error_code error;
    if (root.native().find(L'\0') != std::wstring::npos || has_target_wildcards(root.native())) {
        error = std::make_error_code(std::errc::invalid_argument);
    } else {
        root = std::filesystem::absolute(root, error).lexically_normal().make_preferred();
    }
    if (error) {
        return normalization_failure(root, error, limits);
    }
    return AdobeScan(std::move(root), source, limits, executable_pattern).run(registrations);
}

TargetVersionScanResult scan_adobe_cc_detailed(std::filesystem::path root, TargetVersionSource& source) {
    return scan_adobe_cc_detailed(std::move(root), source, {});
}

TargetVersionScanResult scan_adobe_cc_detailed(std::filesystem::path root, const TargetScanLimits& limits) {
    if (root.empty()) { return scan_adobe_cc_catalog({VersionSearchKind::adobe_catalog, {}, 1}, limits); }
    auto source = make_native_target_version_source(limits);
    return scan_adobe_cc_detailed(std::move(root), *source, limits);
}

VersionSearchKey adobe_cc_search_key(const LaunchAction& action, const std::filesystem::path& configuration_directory) {
    VersionSearchKey key{VersionSearchKind::adobe_catalog, {}, 1};
    for (auto path : action.root_paths) {
        if (path.empty() || path.native().find(L'\0') != std::wstring::npos || has_target_wildcards(path.native()) ||
                (!path.is_absolute() && (path.has_root_name() || path.has_root_directory()))) {
            throw std::invalid_argument("Adobe root_path must be a directory without wildcards.");
        }
        if (!path.is_absolute()) { path = std::filesystem::absolute(configuration_directory) / path; }
        path = path.lexically_normal().make_preferred();
        while (path.has_relative_path() && path.filename().empty()) { path = path.parent_path(); }
        key.locations.push_back(lower(path.native()));
    }
    std::ranges::sort(key.locations);
    const auto repeated = std::ranges::unique(key.locations);
    key.locations.erase(repeated.begin(), repeated.end());
    if (!key.locations.empty() || action.target.size() >= 4) {
        const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, action.target.data(), static_cast<int>(action.target.size()), nullptr, 0);
        if (!count) { throw std::invalid_argument("Invalid Adobe product name."); }
        std::wstring product(static_cast<std::size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, action.target.data(), static_cast<int>(action.target.size()), product.data(), count);
        if (executable_target(product)) {
            if (product.find_first_of(L"\\/:") != std::wstring::npos) { throw std::invalid_argument("Adobe executable targets must be filenames, not paths."); }
            key.product = lower(product);
        } else if (!key.locations.empty()) { key.product = lower(canonical_adobe_cc_name(product)); }
    }
    return key;
}

TargetVersionScanResult scan_adobe_cc_catalog(const VersionSearchKey& key, const TargetScanLimits& limits) {
    std::vector<AdobeCCRegistration> roots;
    std::optional<std::error_code> registry_error;
    try { roots = adobe_cc_registrations(limits); }
    catch (const std::system_error& error) { registry_error = error.code(); }
    for (const auto& location : key.locations) { roots.push_back({key.product, location, true}); }
    auto source = make_native_target_version_source(limits);
    auto result = scan_adobe_cc_detailed({}, *source, limits, roots, executable_target(key.product) ? key.product : L"");
    // Registration is an optional source of hints. Its failure must not prevent
    // discovery through an explicit root_path or the default filesystem root.
    if (registry_error) {
        if (result.diagnostics.size() < limits.max_diagnostics) {
            result.diagnostics.push_back({TargetScanOperation::enumerate, L"HKLM\\SOFTWARE\\Adobe (including uninstall registrations)", *registry_error});
        } else { ++result.omitted_diagnostics; }
    }
    return result;
}

TargetVersionScanResult scan_adobe_cc_detailed(std::filesystem::path root) {
    return scan_adobe_cc_detailed(std::move(root), TargetScanLimits{});
}

namespace {
std::vector<AdobeCCInstallation> complete_catalog(TargetVersionScanResult result) {
    if (result.status != TargetScanStatus::complete) {
        if (result.diagnostics.empty()) {
            const auto error = result.exhausted_limit == TargetScanLimit::none ? native_error(ERROR_GEN_FAILURE) :
                make_target_scan_limit_error(result.exhausted_limit);
            throw std::filesystem::filesystem_error("Cannot finish Adobe version discovery", error);
        }
        const auto& diagnostic = result.diagnostics.front();
        throw std::filesystem::filesystem_error("Cannot finish Adobe version discovery", diagnostic.path, diagnostic.error);
    }
    return std::move(result.versions);
}
} // namespace

std::vector<AdobeCCInstallation> scan_adobe_cc(std::filesystem::path root) {
    return complete_catalog(scan_adobe_cc_detailed(std::move(root)));
}

std::vector<AdobeCCInstallation> scan_adobe_cc_for_action(const LaunchAction& action, const std::filesystem::path& configuration_directory) {
    return complete_catalog(scan_adobe_cc_catalog(adobe_cc_search_key(action, configuration_directory)));
}

bool adobe_cc_is_beta_product(std::wstring_view product) { return is_beta(product); }
bool adobe_cc_is_executable_target(std::wstring_view target) { return executable_target(target); }

std::vector<AdobeCCInstallation> adobe_cc_versions(
        std::span<const AdobeCCInstallation> installations, std::wstring_view product) {
    const auto key = product_key(product);
    const bool beta_only = adobe_cc_is_beta_product(product);
    std::vector<AdobeCCInstallation> result;
    if (key.empty()) { return result; }
    for (const auto& installation : installations) {
        if ((executable_target(product) ? executable_matches(product, installation.executable.filename().native()) :
                installation.target_pattern.empty() && product_key(installation.name) == key) && (!beta_only || installation.beta)) {
            result.push_back(installation);
        }
    }
    sort_target_versions(result);
    return result;
}

std::optional<AdobeCCInstallation> adobe_cc_latest(
        std::span<const AdobeCCInstallation> installations, std::wstring_view product, bool include_beta) {
    const auto versions = adobe_cc_versions(installations, product);
    const bool beta_requested = include_beta || adobe_cc_is_beta_product(product);
    const auto newest = std::ranges::find_if(versions, [beta_requested](const auto& installation) {
        return beta_requested || !installation.beta;
    });
    if (newest == versions.end()) { return std::nullopt; }
    return *newest;
}

std::string adobe_cc_tab_report(std::span<const AdobeCCInstallation> installations, std::span<const std::wstring> unmatched_targets,
        std::span<const std::wstring> configured_roots) {
    const auto quote = [](std::wstring_view wide) {
        if (wide.empty()) { return std::string("\"\""); }
        const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (!size) { throw std::runtime_error("Cannot encode an Adobe product name."); }
        std::string text(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr, nullptr);
        std::string result = "\"";
        constexpr char hex[] = "0123456789ABCDEF";
        for (const auto byte : text) {
            const auto unit = static_cast<unsigned char>(byte);
            if (byte == '\\' || byte == '"') { result += '\\'; result += byte; }
            else if (unit < 0x20 || unit == 0x7f) { result += "\\u00"; result += hex[unit >> 4]; result += hex[unit & 15]; }
            else { result += byte; }
        }
        return result + '"';
    };
    std::ostringstream output;
    output << "# Launcher: installed Adobe Creative Cloud applications.\n";
    for (const auto& target : unmatched_targets) {
        output << "# No discovered AdobeCC application matches target = " << quote(target) << ".\n";
    }
    output << "# Below is the complete discovered list, with the exact target spellings.\n"
        "# Paste this tab into your configuration, or copy its items into an existing tab.\n"
        "# Family entries choose the newest version; (Beta) entries select Beta only.\n"
        "# Add root_path = 'D:\\Adobe' to an item to search another location.\n"
        "# root_path also accepts a list, e.g. ['C:\\Creative Apps', 'D:\\Creative Apps'].\n"
        "# To select by executable instead, use target = 'Dreamweaver.exe' or 'Dreamweaver*.exe'.\n\n"
        "[[tabs]]\nname = \"Adobe Creative Cloud\"\nshort_name = \"Adobe CC\"\nitems = [\n";
    std::map<std::wstring, std::wstring> products;
    for (const auto& installation : installations) {
        if (installation.name.empty() || !installation.target_pattern.empty()) { continue; }
        auto name = canonical_adobe_cc_name(installation.name);
        products.emplace(lower(name), name);
        if (installation.beta) { products.emplace(lower(name) + L" (beta)", name + L" (Beta)"); }
    }
    for (const auto& [key, name] : products) {
        (void)key;
        output << "    { kind = \"adobeCC\", target = " << quote(name);
        std::vector<std::wstring> needed;
        for (const auto& root : configured_roots) {
            auto prefix = lower(std::filesystem::path(root).lexically_normal().native());
            if (!prefix.ends_with(L'\\')) { prefix += L'\\'; }
            if (std::ranges::any_of(installations, [&](const auto& installed) {
                    return product_key(installed.name) == product_key(name) && lower(installed.executable.native()).starts_with(prefix);
                })) { needed.push_back(root); }
        }
        if (!needed.empty()) {
            output << ", root_path = [";
            for (std::size_t index = 0; index < needed.size(); ++index) { output << (index ? ", " : "") << quote(needed[index]); }
            output << ']';
        }
        output << " },\n";
    }
    output << "]\n";
    if (products.empty()) { output << "\n# No installed Adobe applications were discovered on this machine.\n"; }
    return output.str();
}

} // namespace launcher
