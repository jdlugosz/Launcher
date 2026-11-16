#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/target_version_scan.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <limits>
#include <optional>
#include <set>
#include <span>
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

struct PathParts {
    std::filesystem::path root;
    std::vector<std::filesystem::path> components;
};

PathParts split_path(const std::filesystem::path& path) {
    PathParts result{path.root_path(), {}};
    for (const auto& part : path.relative_path()) { result.components.push_back(part); }
    auto root = result.root.native();
    std::size_t consumed = 0;
    // The standard library decomposes \\?\C:\ into root \\?\ plus
    // component C:. Consume the actual filesystem root before path appends,
    // which would otherwise replace the extended prefix with drive-relative C:.
    if (root == L"\\\\?\\" && !result.components.empty()) {
        if (lower(result.components[0].native()) == L"unc" && result.components.size() >= 3) {
            root += result.components[0].native() + L"\\" + result.components[1].native() + L"\\" +
                result.components[2].native() + L"\\";
            consumed = 3;
        } else if (const auto& drive = result.components[0].native(); drive.size() == 2 && drive[1] == L':') {
            root += drive + L"\\";
            consumed = 1;
            if (result.components.size() > 1 && result.components[1] == L"\\") { ++consumed; }
        }
    } else if (root.starts_with(L"\\\\") && !result.components.empty()) {
        // A UNC share is part of the filesystem root, although filesystem::path
        // treats it as the first relative component.
        root += result.components[0].native() + L"\\";
        consumed = 1;
    }
    result.root = std::move(root);
    result.components.erase(result.components.begin(), result.components.begin() + consumed);
    return result;
}

std::filesystem::path normalized_pattern(std::filesystem::path path, std::error_code* failure = nullptr) {
    if (path.empty()) { return {}; }
    std::error_code error;
    if (!path.is_absolute()) {
        auto base = std::filesystem::current_path(error);
        if (error) { if (failure) { *failure = error; } return {}; }
        if (path.has_root_name() && path.root_name() != base.root_name()) {
            // Resolve only a drive-relative root (C:), preserving every pattern
            // component instead of letting GetFullPathName collapse wildcard/..
            // before the wildcard directory has been checked.
            base = std::filesystem::absolute(path.root_name(), error);
            if (error) { if (failure) { *failure = error; } return {}; }
            path = base / path.relative_path();
        } else {
            path = base / path;
        }
    }
    path.make_preferred();
    auto parsed = split_path(path);
    // Preserve wildcard/.. pairs: the wildcard component must actually exist
    // before following its parent. Ordinary literal pairs can collapse safely.
    std::vector<std::filesystem::path> parts;
    for (const auto& part : parsed.components) {
        if (part.empty() || part == L".") { continue; }
        if (part == L".." && !parts.empty() && parts.back() != L".." &&
                !has_target_wildcards(parts.back().native())) {
            parts.pop_back();
        } else if (part != L".." || !parts.empty()) {
            parts.push_back(part);
        }
    }
    auto result = parsed.root;
    for (const auto& part : parts) { result /= part; }
    return result.make_preferred();
}

std::wstring pattern_key(const std::filesystem::path& pattern) {
    return lower(normalized_pattern(pattern).native());
}

std::wstring path_key(const std::filesystem::path& path) {
    auto normalized = path.lexically_normal();
    normalized.make_preferred();
    return lower(normalized.native());
}

std::wstring version_text(const std::array<std::uint16_t, 4>& version) {
    return std::format(L"{}.{}.{}.{}", version[0], version[1], version[2], version[3]);
}

bool digit(wchar_t value) { return value >= L'0' && value <= L'9'; }

// Digit strings avoid an arbitrary component-size or integer-overflow limit.
std::vector<std::wstring> numeric_parts(std::wstring_view text) {
    std::vector<std::wstring> parts;
    for (std::size_t index = 0; index < text.size();) {
        if (!digit(text[index])) { ++index; continue; }
        const auto first = index;
        while (index < text.size() && digit(text[index])) { ++index; }
        auto value = text.substr(first, index - first);
        while (value.size() > 1 && value.front() == L'0') { value.remove_prefix(1); }
        parts.emplace_back(value);
    }
    while (!parts.empty() && parts.back() == L"0") { parts.pop_back(); }
    return parts;
}

std::vector<std::wstring> sort_version(const TargetVersion& version) {
    if (version.target_pattern.empty() || version.has_version_resource) {
        if (version.target_pattern.empty() || std::ranges::any_of(version.version, [](auto part) { return part != 0; })) {
            return numeric_parts(version_text(version.version));
        }
    }
    return numeric_parts(version.product_version.empty() ? version.path_label : version.product_version);
}

int compare_numbers(const std::vector<std::wstring>& left, const std::vector<std::wstring>& right) {
    const auto count = (std::min)(left.size(), right.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (left[index].size() != right[index].size()) { return left[index].size() < right[index].size() ? -1 : 1; }
        if (left[index] != right[index]) { return left[index] < right[index] ? -1 : 1; }
    }
    if (left.size() == right.size()) { return 0; }
    return left.size() < right.size() ? -1 : 1;
}

int natural_compare(std::wstring_view left, std::wstring_view right) {
    std::size_t left_index = 0;
    std::size_t right_index = 0;
    while (left_index < left.size() && right_index < right.size()) {
        if (digit(left[left_index]) && digit(right[right_index])) {
            const auto left_start = left_index;
            const auto right_start = right_index;
            while (left_index < left.size() && digit(left[left_index])) { ++left_index; }
            while (right_index < right.size() && digit(right[right_index])) { ++right_index; }
            const auto difference = compare_numbers(numeric_parts(left.substr(left_start, left_index - left_start)),
                numeric_parts(right.substr(right_start, right_index - right_start)));
            if (difference != 0) { return difference; }
        } else {
            if (left[left_index] != right[right_index]) { return left[left_index] < right[right_index] ? -1 : 1; }
            ++left_index;
            ++right_index;
        }
    }
    if (left_index == left.size() && right_index == right.size()) { return 0; }
    return left_index == left.size() ? -1 : 1;
}

bool newest_first(const TargetVersion& left, const TargetVersion& right) {
    const bool left_metadata = !left.target_pattern.empty() && left.has_version_resource;
    const bool right_metadata = !right.target_pattern.empty() && right.has_version_resource;
    if (left_metadata != right_metadata) { return left_metadata; }
    if (const auto difference = compare_numbers(sort_version(left), sort_version(right)); difference != 0) {
        return difference > 0;
    }
    if (left.file_version_numbers != right.file_version_numbers) {
        return left.file_version_numbers > right.file_version_numbers;
    }
    if (left.beta != right.beta) { return !left.beta; }
    const auto left_label = left.target_pattern.empty() ? std::wstring{} :
        lower(left.product_version.empty() ? left.path_label : left.product_version);
    const auto right_label = right.target_pattern.empty() ? std::wstring{} :
        lower(right.product_version.empty() ? right.path_label : right.product_version);
    if (const auto difference = natural_compare(left_label, right_label); difference != 0) { return difference > 0; }
    if (const auto difference = natural_compare(lower(left.path_label), lower(right.path_label)); difference != 0) {
        return difference > 0;
    }
    const auto left_path = path_key(left.executable);
    const auto right_path = path_key(right.executable);
    if (left_path != right_path) { return left_path < right_path; }
    return left.executable.native() < right.executable.native();
}

struct Match {
    std::filesystem::path path;
    std::wstring label;
};

std::error_code native_error(DWORD code) { return {static_cast<int>(code), std::system_category()}; }

class TargetScanLimitCategory final : public std::error_category {
public:
    const char* name() const noexcept override { return "launcher.target-scan-limit"; }
    std::string message(int value) const override {
        switch (static_cast<TargetScanLimit>(value)) {
        case TargetScanLimit::components: return "Version search component limit exceeded";
        case TargetScanLimit::operations: return "Version search provider operation limit exceeded";
        case TargetScanLimit::entries: return "Version search directory entry limit exceeded";
        case TargetScanLimit::branches: return "Version search branch limit exceeded";
        case TargetScanLimit::targets: return "Version search distinct target limit exceeded";
        case TargetScanLimit::resource_bytes: return "Version resource byte limit exceeded";
        case TargetScanLimit::storage_bytes: return "Version search accounted storage limit exceeded";
        case TargetScanLimit::metadata_units: return "Version metadata text limit exceeded";
        case TargetScanLimit::path_units: return "Version search path length limit exceeded";
        case TargetScanLimit::translations: return "Version resource translation count limit exceeded";
        default: return "Unknown version search limit";
        }
    }
};

const TargetScanLimitCategory limit_category;

std::size_t add_size(std::size_t left, std::size_t right) {
    return right > (std::numeric_limits<std::size_t>::max)() - left ? (std::numeric_limits<std::size_t>::max)() : left + right;
}

std::size_t text_bytes(const std::wstring& text) {
    return text.capacity() > (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t) ?
        (std::numeric_limits<std::size_t>::max)() : text.capacity() * sizeof(wchar_t);
}

std::size_t version_text_bytes(const TargetVersion& version) {
    auto bytes = add_size(text_bytes(version.name), text_bytes(version.product_name));
    bytes = add_size(bytes, text_bytes(version.product_version));
    bytes = add_size(bytes, text_bytes(version.file_version));
    bytes = add_size(bytes, text_bytes(version.executable.native()));
    bytes = add_size(bytes, text_bytes(version.target_pattern));
    return add_size(bytes, text_bytes(version.path_label));
}

class ScanBudget final {
public:
    ScanBudget(TargetVersionScanResult& result, const TargetScanLimits& limits) : result_(result), limits_(limits) {}

    void record(TargetScanOperation operation, const std::filesystem::path& path, std::error_code error) {
        result_.status = TargetScanStatus::incomplete;
        const auto limit = target_scan_limit(error);
        if (limit != TargetScanLimit::none) {
            operation = TargetScanOperation::budget;
            exhausted_ = true;
            if (result_.exhausted_limit == TargetScanLimit::none) { result_.exhausted_limit = limit; }
        }
        // Diagnostic samples are deliberately bounded; actual action identities
        // elsewhere are never shortened. Reserve/account only accepted records.
        auto sample = std::filesystem::path(path.native().substr(0, limits_.max_diagnostic_path_units));
        const auto previous_capacity = result_.diagnostics.capacity();
        const auto requested_capacity = (std::max)(previous_capacity, result_.diagnostics.size() + 1);
        const auto bytes = add_size((requested_capacity - previous_capacity) * sizeof(TargetScanDiagnostic), text_bytes(sample.native()));
        if (result_.diagnostics.size() < limits_.max_diagnostics && bytes <= remaining()) {
            if (result_.diagnostics.size() == result_.diagnostics.capacity()) {
                // Exact incremental reserve keeps diagnostic capacity in its
                // own cap even when callers inject very small limits.
                result_.diagnostics.reserve(result_.diagnostics.size() + 1);
            }
            const auto actual_bytes = add_size((result_.diagnostics.capacity() - previous_capacity) * sizeof(TargetScanDiagnostic), text_bytes(sample.native()));
            if (actual_bytes > remaining()) {
                exhausted_ = true;
                if (result_.exhausted_limit == TargetScanLimit::none) { result_.exhausted_limit = TargetScanLimit::storage_bytes; }
                ++result_.omitted_diagnostics;
                return;
            }
            result_.diagnostics.push_back({operation, std::move(sample), error ? error : native_error(ERROR_GEN_FAILURE), limit});
            used_ += actual_bytes;
        } else {
            ++result_.omitted_diagnostics;
        }
    }

    bool fail(TargetScanLimit limit, const std::filesystem::path& path) {
        exhausted_ = true;
        if (result_.exhausted_limit == TargetScanLimit::none) { result_.exhausted_limit = limit; }
        record(TargetScanOperation::budget, path, make_target_scan_limit_error(limit));
        return false;
    }

    bool operation(const std::filesystem::path& path) { return take(operations_, limits_.max_operations, TargetScanLimit::operations, path); }
    bool entry(const std::filesystem::path& path) { return take(entries_, limits_.max_entries, TargetScanLimit::entries, path); }
    bool branch(const std::filesystem::path& path) { return take(branches_, limits_.max_branches, TargetScanLimit::branches, path); }
    bool path(const std::filesystem::path& path) {
        return path.native().size() <= limits_.max_path_units || fail(TargetScanLimit::path_units, path);
    }
    bool storage(std::size_t bytes, const std::filesystem::path& path) {
        if (bytes > remaining()) { return fail(TargetScanLimit::storage_bytes, path); }
        used_ += bytes;
        return true;
    }
    void release(std::size_t bytes) { used_ -= bytes; }
    std::size_t remaining() const { return limits_.max_storage_bytes - used_; }
    bool exhausted() const { return exhausted_; }

    template<class T>
    bool reserve_one(std::vector<T>& values, std::size_t& accounted, const std::filesystem::path& path) {
        if (values.size() != values.capacity()) { return true; }
        const auto capacity = values.capacity();
        const auto desired = capacity == 0 ? 1 : add_size(capacity, capacity);
        if (desired > (std::numeric_limits<std::size_t>::max)() / sizeof(T)) { return fail(TargetScanLimit::storage_bytes, path); }
        const auto bytes = (desired - capacity) * sizeof(T);
        if (!storage(bytes, path)) { return false; }
        values.reserve(desired);
        accounted = add_size(accounted, bytes);
        // reserve is allowed to provide more capacity than requested.
        const auto extra = (values.capacity() - desired) * sizeof(T);
        if (!storage(extra, path)) { return false; }
        accounted = add_size(accounted, extra);
        return true;
    }

private:
    bool take(std::size_t& value, std::size_t maximum, TargetScanLimit limit, const std::filesystem::path& path) {
        if (exhausted_) { return false; }
        if (value == maximum) { return fail(limit, path); }
        ++value;
        return true;
    }

    TargetVersionScanResult& result_;
    const TargetScanLimits& limits_;
    std::size_t used_ = 0;
    std::size_t operations_ = 0;
    std::size_t entries_ = 0;
    std::size_t branches_ = 0;
    bool exhausted_ = false;
};

bool missing(const std::error_code& error) {
    // MSVC also maps BAD_NETPATH/INVALID_NAME to generic ENOENT. Preserve the
    // native distinction before considering portable adapter errors.
    if (error.category() == std::system_category()) {
        return error.value() == ERROR_FILE_NOT_FOUND || error.value() == ERROR_PATH_NOT_FOUND;
    }
    return error.category() == std::generic_category() && error.value() == static_cast<int>(std::errc::no_such_file_or_directory);
}

// A missing descendant is ordinary absence only if a directory above it still
// answers. Never walk beyond the scan's drive/share anchor to turn provider
// disappearance into an authoritative empty catalog.
bool expected_absence(TargetVersionSource& source, const std::filesystem::path& path, const std::filesystem::path& root,
        std::error_code error, TargetScanOperation operation, ScanBudget& budget) {
    if (!missing(error)) { budget.record(operation, path, error); return false; }
    auto ancestor = path.parent_path();
    while (!ancestor.empty()) {
        if (!budget.operation(ancestor)) { return false; }
        const auto probe = source.probe(ancestor);
        if (probe) {
            if (*probe == TargetFileKind::directory) { return true; }
            budget.record(TargetScanOperation::probe, ancestor, native_error(ERROR_DIRECTORY));
            return false;
        }
        if (!missing(probe.error()) || ancestor.native().size() <= root.native().size()) {
            budget.record(TargetScanOperation::probe, ancestor, probe.error());
            return false;
        }
        const auto parent = ancestor.parent_path();
        if (parent == ancestor) { break; }
        ancestor = parent;
    }
    budget.record(operation, path, error);
    return false;
}

const char* operation_failure(TargetScanOperation operation) {
    switch (operation) {
    case TargetScanOperation::normalize: return "Cannot resolve the version search pattern";
    case TargetScanOperation::probe: return "Cannot inspect a version search path";
    case TargetScanOperation::enumerate: return "Cannot finish a version directory listing";
    case TargetScanOperation::metadata: return "Cannot read version metadata";
    case TargetScanOperation::budget: return "Version search limit exceeded";
    }
    return "Cannot complete version discovery";
}

} // namespace

std::error_code make_target_scan_limit_error(TargetScanLimit limit) { return {static_cast<int>(limit), limit_category}; }

TargetScanLimit target_scan_limit(const std::error_code& error) noexcept {
    return error.category() == limit_category ? static_cast<TargetScanLimit>(error.value()) : TargetScanLimit::none;
}

bool has_target_wildcards(std::wstring_view target) {
    if (target.size() >= 4 && (target[0] == L'\\' || target[0] == L'/') &&
            (target[1] == L'\\' || target[1] == L'/') && target[2] == L'?' &&
            (target[3] == L'\\' || target[3] == L'/')) {
        target.remove_prefix(4);
    }
    return target.find_first_of(L"*?") != std::wstring_view::npos;
}

std::filesystem::path normalize_target_version_pattern(std::filesystem::path expanded_pattern) {
    if (expanded_pattern.empty() || expanded_pattern.native().find(L'\0') != std::wstring::npos) {
        throw std::filesystem::filesystem_error("Invalid version target pattern", expanded_pattern, std::make_error_code(std::errc::invalid_argument));
    }
    std::error_code error;
    auto result = normalized_pattern(expanded_pattern, &error);
    if (error) { throw std::filesystem::filesystem_error("Cannot normalize version target pattern", expanded_pattern, error); }
    return result;
}

TargetVersionScanResult scan_target_versions_detailed(std::filesystem::path expanded_pattern, TargetVersionSource& source) {
    return scan_target_versions_detailed(std::move(expanded_pattern), source, TargetScanLimits{});
}

TargetVersionScanResult scan_target_versions_detailed(std::filesystem::path expanded_pattern, TargetVersionSource& source,
        const TargetScanLimits& limits) {
    TargetVersionScanResult result;
    ScanBudget budget(result, limits);
    std::error_code normalization_error;
    if (!budget.path(expanded_pattern)) { return result; }
    if (expanded_pattern.native().find(L'\0') != std::wstring::npos) {
        budget.record(TargetScanOperation::normalize, expanded_pattern, native_error(ERROR_INVALID_NAME));
        if (!budget.exhausted()) { result.status = TargetScanStatus::failed; }
        return result;
    }
    const auto original = expanded_pattern;
    expanded_pattern = normalized_pattern(std::move(expanded_pattern), &normalization_error);
    if (expanded_pattern.empty()) {
        budget.record(TargetScanOperation::normalize, original,
            normalization_error ? normalization_error : native_error(ERROR_INVALID_NAME));
        if (!budget.exhausted()) { result.status = TargetScanStatus::failed; }
        return result;
    }
    if (!budget.path(expanded_pattern)) { return result; }
    const auto parsed = split_path(expanded_pattern);
    const auto& components = parsed.components;
    if (components.size() > limits.max_components) {
        budget.fail(TargetScanLimit::components, expanded_pattern);
        return result;
    }
    auto identity_bytes = add_size(text_bytes(original.native()), text_bytes(expanded_pattern.native()));
    identity_bytes = add_size(identity_bytes, text_bytes(parsed.root.native()));
    identity_bytes = add_size(identity_bytes, components.capacity() * sizeof(std::filesystem::path));
    for (const auto& component : components) { identity_bytes = add_size(identity_bytes, text_bytes(component.native())); }
    if (!budget.storage(identity_bytes, expanded_pattern) || !budget.operation(parsed.root)) { return result; }
    const auto anchor = source.probe(parsed.root);
    if (!anchor || *anchor != TargetFileKind::directory) {
        budget.record(TargetScanOperation::probe, parsed.root,
            anchor ? native_error(ERROR_DIRECTORY) : anchor.error());
        if (!budget.exhausted()) { result.status = TargetScanStatus::failed; }
        return result;
    }
    if (components.empty()) { return result; }
    std::vector<Match> matches;
    std::size_t match_bytes = 0;
    if (!budget.reserve_one(matches, match_bytes, parsed.root)) { return result; }
    auto root_match = Match{parsed.root, {}};
    auto root_bytes = add_size(text_bytes(root_match.path.native()), text_bytes(root_match.label));
    if (!budget.storage(root_bytes, parsed.root)) { return result; }
    match_bytes = add_size(match_bytes, root_bytes);
    matches.push_back(std::move(root_match));
    std::vector<std::wstring> targets;
    std::size_t target_bytes = 0;
    for (std::size_t index = 0; index < components.size() && !matches.empty(); ++index) {
        const auto& component = components[index];
        const bool wildcard = has_target_wildcards(component.native());
        const bool final = index + 1 == components.size();
        std::vector<Match> next;
        std::size_t next_bytes = 0;
        const auto append_match = [&](const Match& parent, const std::filesystem::path& path,
                std::wstring_view name, TargetFileKind kind) {
            if (kind != (final ? TargetFileKind::regular_file : TargetFileKind::directory)) { return true; }
            if (!budget.path(path) || !budget.branch(path)) { return false; }
            if (final) {
                auto key = path_key(path);
                const auto found = std::ranges::lower_bound(targets, key);
                if (found == targets.end() || *found != key) {
                    const auto position = static_cast<std::size_t>(found - targets.begin());
                    if (targets.size() == limits.max_targets) { return budget.fail(TargetScanLimit::targets, path); }
                    if (!budget.reserve_one(targets, target_bytes, path)) { return false; }
                    const auto bytes = text_bytes(key);
                    if (!budget.storage(bytes, path)) { return false; }
                    target_bytes = add_size(target_bytes, bytes);
                    targets.insert(targets.begin() + position, std::move(key));
                }
            }
            auto label = parent.label;
            if (wildcard) {
                if (!label.empty()) { label += L" / "; }
                label += name;
            }
            auto match = Match{path, std::move(label)};
            const auto bytes = add_size(text_bytes(match.path.native()), text_bytes(match.label));
            if (!budget.reserve_one(next, next_bytes, path) || !budget.storage(bytes, path)) { return false; }
            next_bytes = add_size(next_bytes, bytes);
            next.push_back(std::move(match));
            return true;
        };
        for (const auto& parent : matches) {
            const auto search = parent.path / component;
            if (!budget.path(search) || !budget.operation(search)) { return result; }
            if (!wildcard) {
                const auto probe = source.probe(search);
                if (probe) {
                    if (!append_match(parent, search, component.native(), *probe)) { return result; }
                } else {
                    expected_absence(source, search, parsed.root, probe.error(), TargetScanOperation::probe, budget);
                    if (budget.exhausted()) { return result; }
                }
                continue;
            }
            auto cursor = source.enumerate(search);
            if (!cursor) {
                expected_absence(source, search, parsed.root, cursor.error(), TargetScanOperation::enumerate, budget);
                if (budget.exhausted()) { return result; }
                continue;
            }
            if (!*cursor) {
                budget.record(TargetScanOperation::enumerate, search, native_error(ERROR_INVALID_HANDLE));
                continue;
            }
            while (true) {
                if (!budget.operation(search)) { return result; }
                auto entry = (*cursor)->next();
                if (!entry) {
                    budget.record(TargetScanOperation::enumerate, search, entry.error());
                    if (budget.exhausted()) { return result; }
                    break;
                }
                if (!*entry) { break; }
                if (!budget.entry(search)) { return result; }
                const auto& found = **entry;
                if (found.name == L"." || found.name == L"..") { continue; }
                if (found.name.empty() || found.name.find_first_of(L"/\\") != std::wstring::npos ||
                        found.name.find(L'\0') != std::wstring::npos) {
                    budget.record(TargetScanOperation::enumerate, search, native_error(ERROR_INVALID_DATA));
                    continue;
                }
                if (found.name.size() > limits.max_path_units) { budget.fail(TargetScanLimit::path_units, search); return result; }
                const auto path = parent.path / found.name;
                if (!budget.path(path)) { return result; }
                auto kind = found.kind;
                if (final && kind != TargetFileKind::directory) {
                    if (!budget.operation(path)) { return result; }
                    const auto probe = source.probe(path);
                    if (!probe) {
                        expected_absence(source, path, parsed.root, probe.error(), TargetScanOperation::probe, budget);
                        if (budget.exhausted()) { return result; }
                        continue;
                    }
                    kind = *probe;
                }
                if (!append_match(parent, path, found.name, kind)) { return result; }
            }
        }
        budget.release(match_bytes);
        matches = std::move(next);
        match_bytes = next_bytes;
    }
    const auto& key = expanded_pattern.native();
    std::size_t version_bytes = 0;
    for (auto& match : matches) {
        if (!budget.operation(match.path)) { break; }
        auto metadata_limits = limits;
        metadata_limits.max_storage_bytes = budget.remaining();
        auto metadata = source.read_metadata(match.path, metadata_limits);
        if (!metadata) {
            if (target_scan_limit(metadata.error()) != TargetScanLimit::none) {
                budget.record(TargetScanOperation::budget, match.path, metadata.error());
                break;
            }
            // A disappearing file is an omitted branch, not an absent resource.
            // Other metadata I/O failures keep the whole observation untrusted.
            if (missing(metadata.error())) {
                if (!budget.operation(match.path)) { break; }
                const auto probe = source.probe(match.path);
                if (!probe) {
                    expected_absence(source, match.path, parsed.root, probe.error(), TargetScanOperation::probe, budget);
                    if (budget.exhausted()) { break; }
                    continue;
                }
                if (*probe != TargetFileKind::regular_file) { continue; }
            }
            budget.record(TargetScanOperation::metadata, match.path, metadata.error());
        }
        auto version = metadata ? std::move(*metadata).value_or(TargetVersion{}) : TargetVersion{};
        if (version.name.size() > limits.max_metadata_units || version.product_name.size() > limits.max_metadata_units ||
                version.product_version.size() > limits.max_metadata_units || version.file_version.size() > limits.max_metadata_units) {
            budget.fail(TargetScanLimit::metadata_units, match.path);
            break;
        }
        version.executable = match.path.lexically_normal();
        version.target_pattern = key;
        version.path_label = match.label;
        if (version.product_name.empty()) { version.product_name = match.path.stem().native(); }
        version.name = version.product_name;
        if (!version.has_version_resource) {
            version.product_version = version.path_label.empty() ? match.path.filename().native() : version.path_label;
        }
        if (!budget.reserve_one(result.versions, version_bytes, match.path) ||
                !budget.storage(version_text_bytes(version), match.path)) { break; }
        version_bytes = add_size(version_bytes, version_text_bytes(version));
        result.versions.push_back(std::move(version));
    }
    // Reuse the bounded identity table during final deduplication. The public
    // convenience sorter builds its own set, which would duplicate all retained
    // path strings outside this job's storage account.
    std::vector<Match>().swap(matches);
    budget.release(match_bytes);
    if (!budget.storage(targets.size(), expanded_pattern)) { return result; }
    std::vector<unsigned char> seen(targets.size(), 0);
    std::ranges::sort(result.versions, newest_first);
    std::erase_if(result.versions, [&](const auto& version) {
        const auto key = path_key(version.executable);
        const auto found = std::ranges::lower_bound(targets, key);
        const auto index = static_cast<std::size_t>(found - targets.begin());
        return std::exchange(seen[index], static_cast<unsigned char>(1)) != 0;
    });
    return result;
}

TargetVersionScanResult scan_target_versions_detailed(std::filesystem::path expanded_pattern) {
    return scan_target_versions_detailed(std::move(expanded_pattern), TargetScanLimits{});
}

TargetVersionScanResult scan_target_versions_detailed(std::filesystem::path expanded_pattern, const TargetScanLimits& limits) {
    auto source = make_native_target_version_source(limits);
    return scan_target_versions_detailed(std::move(expanded_pattern), *source, limits);
}

std::vector<TargetVersion> scan_target_versions(std::filesystem::path expanded_pattern, TargetVersionSource& source) {
    auto result = scan_target_versions_detailed(expanded_pattern, source);
    if (result.status != TargetScanStatus::complete) {
        if (result.diagnostics.empty()) {
            const auto error = result.exhausted_limit == TargetScanLimit::none ?
                std::make_error_code(std::errc::io_error) : make_target_scan_limit_error(result.exhausted_limit);
            throw std::filesystem::filesystem_error("Cannot complete version discovery", expanded_pattern, error);
        }
        const auto& error = result.diagnostics.front();
        throw std::filesystem::filesystem_error(operation_failure(error.operation), error.path, error.error);
    }
    return std::move(result.versions);
}

std::vector<TargetVersion> scan_target_versions(std::filesystem::path expanded_pattern) {
    auto source = make_native_target_version_source();
    return scan_target_versions(std::move(expanded_pattern), *source);
}

std::vector<TargetVersion> target_versions_for_pattern(std::span<const TargetVersion> versions, const std::filesystem::path& pattern) {
    const auto key = pattern_key(pattern);
    std::vector<TargetVersion> result;
    if (key.empty()) { return result; }
    for (const auto& version : versions) {
        if (!version.target_pattern.empty() && pattern_key(version.target_pattern) == key) { result.push_back(version); }
    }
    sort_target_versions(result);
    return result;
}

void sort_target_versions(std::vector<TargetVersion>& versions) {
    sort_distinct_target_versions(versions);
    std::set<std::wstring> paths;
    std::erase_if(versions, [&paths](const auto& version) { return !paths.insert(path_key(version.executable)).second; });
}

void sort_distinct_target_versions(std::vector<TargetVersion>& versions) { std::ranges::sort(versions, newest_first); }

} // namespace launcher
