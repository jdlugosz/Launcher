#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

struct TargetVersion {
    std::wstring name;
    std::wstring product_name;
    std::wstring product_version;
    std::wstring file_version;
    std::array<std::uint16_t, 4> version{};
    std::array<std::uint16_t, 4> file_version_numbers{};
    std::filesystem::path executable;
    bool beta = false;
    std::wstring target_pattern;
    std::wstring path_label;
    bool has_version_resource = false;

    bool operator==(const TargetVersion&) const = default;
};

// Only * and ? are wildcards. The ? in a Windows extended-path prefix is literal.
bool has_target_wildcards(std::wstring_view target);

// Reads optional Windows version metadata without requiring ProductName. Files
// without a valid fixed version resource return nullopt; their paths still work
// as generic versioned targets.
std::optional<TargetVersion> read_target_version(const std::filesystem::path& path);

// Expands each path component with FindFirstFileW's native wildcard semantics.
// Traversal is bounded by the supplied components, including across deliberate
// directory links; only existing final regular files become versions. Product
// metadata is optional. Matching path components provide readable fallback labels.
// An incomplete/failed check throws filesystem_error so existing background
// caches retain their previous complete catalog. See target_version_scan.hpp
// for structured observations and injectable filesystem/metadata adapters.
std::vector<TargetVersion> scan_target_versions(std::filesystem::path expanded_pattern);

// Groups by an absolute, normalized, case-insensitive pattern. Each result is
// newest first with duplicate executable paths collapsed case-insensitively.
std::vector<TargetVersion> target_versions_for_pattern(std::span<const TargetVersion> versions, const std::filesystem::path& pattern);

// Metadata-bearing generic targets sort first; numeric product/file versions
// then take precedence over path labels. Without metadata, numeric runs in
// matching components sort naturally (5.10 > 5.9).
// Sorting also removes duplicate paths; call this separately for each pattern.
void sort_target_versions(std::vector<TargetVersion>& versions);

// Same ordering without allocating a duplicate-path set. The caller guarantees
// distinct executable identities, as the bounded discovery scanners do.
void sort_distinct_target_versions(std::vector<TargetVersion>& versions);

} // namespace launcher
