#pragma once

#include "launcher/target_version_scan.hpp"

#include <compare>
#include <string>
#include <vector>

namespace launcher {

// UI-owned suppression state for one configured search. No filesystem or
// notification APIs are called here; the caller owns the per-search mapping.
class PerSearchNoticeState final {
public:
    // Stage a changed set of external filesystem errors. Reordering and duplicate
    // observations do not change the set. A complete scan clears suppression;
    // an incomplete/failed internal-only result does not imply recovery.
    // False also covers an allocation failure or an out-of-contract observation
    // beyond the scanner's default 64 diagnostics / 32767 path-unit bounds.
    [[nodiscard]] bool observe(const TargetVersionScanResult& result) noexcept;

    // Call only after the staged notification was delivered. A failed native
    // delivery remains retryable on the next observe(). Another observe()
    // invalidates the previous staged candidate, even when it returns false.
    void mark_shown() noexcept;

private:
    struct Signature {
        std::wstring path;
        bool native_domain;
        int code;
        auto operator<=>(const Signature&) const = default;
    };
    std::vector<Signature> shown_;
    std::vector<Signature> pending_;
};

} // namespace launcher
