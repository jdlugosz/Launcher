#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/filesystem_error_notice.hpp"
#include "launcher/filesystem_error_log.hpp"

#include <algorithm>
#include <cstddef>
#include <system_error>
#include <utility>
#include <vector>

namespace launcher {

bool PerSearchNoticeState::observe(const TargetVersionScanResult& result) noexcept {
    pending_.clear();
    if (result.status == TargetScanStatus::complete) {
        shown_.clear();
        return false;
    }
    try {
        constexpr TargetScanLimits bounds{};
        std::vector<Signature> observed;
        for (const auto& diagnostic : result.diagnostics) {
            if (!is_external_filesystem_error(diagnostic)) { continue; }
            if (diagnostic.path.native().size() > bounds.max_diagnostic_path_units) { return false; }
            Signature signature{diagnostic.path.native(), diagnostic.error.category() == std::system_category(), diagnostic.error.value()};
            const auto position = std::lower_bound(observed.begin(), observed.end(), signature);
            if (position != observed.end() && *position == signature) { continue; }
            // Do not remember a partial signature set as an accepted banner.
            // Production scanners already cap the complete diagnostic vector.
            if (observed.size() == bounds.max_diagnostics) { return false; }
            observed.insert(position, std::move(signature));
        }
        if (observed.empty() || observed == shown_) { return false; }
        pending_ = std::move(observed);
        return true;
    } catch (...) { return false; }
}

void PerSearchNoticeState::mark_shown() noexcept {
    if (pending_.empty()) { return; }
    shown_.swap(pending_);
    pending_.clear();
}

} // namespace launcher
