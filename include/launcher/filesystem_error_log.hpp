#pragma once

#include "launcher/file_lookup.hpp"
#include "launcher/target_version_scan.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>

namespace launcher {

// Shared journal/notification classification; no error text lookup or I/O.
bool is_external_filesystem_error(const TargetScanDiagnostic& diagnostic) noexcept;

// Share one instance among the scan workers. Construction is lexical only;
// appending opens/closes the journal on that worker and never throws. The file
// contains independent UTF-8 JSON records, not Launcher activity chronology.
class FilesystemErrorLog final {
public:
    explicit FilesystemErrorLog(std::filesystem::path absolute_path, std::uint64_t maximum_bytes = 256 * 1024, FileLookup files = FileLookup({}, {}));
    FilesystemErrorLog(const FilesystemErrorLog&) = delete;
    FilesystemErrorLog& operator=(const FilesystemErrorLog&) = delete;

    // Records native/generic probe, enumeration and metadata errors. Internal
    // normalization/guardrail/custom-category diagnostics stay in the app log.
    // True includes an empty/ineligible batch. False never changes scan results;
    // the next call retries, including after a transient access/sharing failure.
    [[nodiscard]] bool append(std::span<const TargetScanDiagnostic> diagnostics) noexcept;
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    FileLookup files_;
    std::filesystem::path path_;
    std::uint64_t maximum_bytes_;
    std::mutex mutex_;
};

} // namespace launcher
