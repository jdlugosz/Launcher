#pragma once

#include "launcher/target_versions.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace launcher {

enum class TargetScanStatus { complete, incomplete, failed };
enum class TargetScanOperation { normalize, probe, enumerate, metadata, budget };
enum class TargetFileKind { directory, regular_file, other };

enum class TargetScanLimit { none, components, operations, entries, branches, targets, resource_bytes,
    storage_bytes, metadata_units, path_units, translations };

// Finite per-search guardrails, injectable so small deterministic fixtures can
// exercise exhaustion. Actual target/action paths are rejected, never truncated.
// Storage accounts container capacity and retained text, excluding allocator
// bookkeeping. Synchronous provider calls can still block within these bounds.
struct TargetScanLimits {
    std::size_t max_components = 256;
    std::size_t max_operations = 100'000;
    std::size_t max_entries = 100'000;
    std::size_t max_branches = 100'000;
    std::size_t max_targets = 4'096;
    std::size_t max_resource_bytes = 16 * 1024 * 1024;
    std::size_t max_storage_bytes = 64 * 1024 * 1024;
    std::size_t max_metadata_units = 4'096;
    std::size_t max_translations = 128;
    std::size_t max_diagnostics = 64;
    // Defaults match the Windows path limit below so filesystem diagnostics
    // retain the full path supplied to native I/O. Smaller test caps may sample.
    std::size_t max_diagnostic_path_units = 32'767;
    std::size_t max_path_units = 32'767;
};

std::error_code make_target_scan_limit_error(TargetScanLimit limit);
TargetScanLimit target_scan_limit(const std::error_code& error) noexcept;

struct TargetScanDiagnostic {
    TargetScanOperation operation;
    std::filesystem::path path;
    std::error_code error;
    TargetScanLimit limit = TargetScanLimit::none;
};

struct TargetVersionScanResult {
    TargetScanStatus status = TargetScanStatus::complete;
    // Non-complete observations are diagnostic evidence, never an authoritative
    // catalog. The compatibility API rejects them instead of replacing a cache.
    std::vector<TargetVersion> versions;
    std::vector<TargetScanDiagnostic> diagnostics;
    std::size_t omitted_diagnostics = 0;
    // Preserved even when no diagnostic record fits the configured cap.
    TargetScanLimit exhausted_limit = TargetScanLimit::none;
};

struct TargetDirectoryEntry {
    std::wstring name;
    // Enumeration attributes are advisory. Final files are probed again so
    // vanished files and links to non-files do not become launch targets.
    TargetFileKind kind = TargetFileKind::other;
    bool reparse_point = false;
};

class TargetDirectoryCursor {
public:
    virtual ~TargetDirectoryCursor() = default;
    // nullopt is a normal end; unexpected is a failed/partial enumeration.
    virtual std::expected<std::optional<TargetDirectoryEntry>, std::error_code> next() = 0;
};

// Synchronous, worker-owned adapter. Implementations return errors rather than
// silently treating them as absence. Win32 errors use std::system_category();
// finite guardrails use make_target_scan_limit_error for named exhaustion.
// Exceptions (including allocation failures) propagate to the worker boundary.
class TargetVersionSource {
public:
    virtual ~TargetVersionSource() = default;
    virtual std::expected<TargetFileKind, std::error_code> probe(const std::filesystem::path& path) = 0;
    // Adobe's legacy catalog deliberately skips links; generic configured paths
    // follow them. Linkless injected sources need no extra implementation.
    virtual std::expected<TargetFileKind, std::error_code> probe_no_follow(const std::filesystem::path& path) { return probe(path); }
    virtual std::expected<std::unique_ptr<TargetDirectoryCursor>, std::error_code> enumerate(const std::filesystem::path& pattern) = 0;
    // Value = present metadata, nullopt = absent/invalid resource, unexpected =
    // unreadable file/resource. Missing optional metadata is a valid fallback.
    virtual std::expected<std::optional<TargetVersion>, std::error_code> read_metadata(const std::filesystem::path& path) = 0;
    virtual std::expected<std::optional<TargetVersion>, std::error_code> read_metadata(const std::filesystem::path& path,
            const TargetScanLimits&) { return read_metadata(path); }
};

std::unique_ptr<TargetVersionSource> make_native_target_version_source(const TargetScanLimits& limits = {});

// Shared lexical normalization for discovery and search identity. This preserves
// wildcard/.. pairs and does not probe the filesystem or resolve directory links.
std::filesystem::path normalize_target_version_pattern(std::filesystem::path expanded_pattern);

TargetVersionScanResult scan_target_versions_detailed(std::filesystem::path expanded_pattern, TargetVersionSource& source);
TargetVersionScanResult scan_target_versions_detailed(std::filesystem::path expanded_pattern, TargetVersionSource& source,
    const TargetScanLimits& limits);
TargetVersionScanResult scan_target_versions_detailed(std::filesystem::path expanded_pattern);
TargetVersionScanResult scan_target_versions_detailed(std::filesystem::path expanded_pattern, const TargetScanLimits& limits);

// Same complete-only compatibility policy as the ordinary one-argument API,
// with an injected source for deterministic tests and future worker adapters.
// Throws filesystem_error for an incomplete/failed observation.
std::vector<TargetVersion> scan_target_versions(std::filesystem::path expanded_pattern, TargetVersionSource& source);

} // namespace launcher
