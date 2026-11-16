#pragma once

#include "launcher/target_versions.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace launcher {

// Owned by one UI thread. Workers only publish private results; snapshot reads
// and poll adoption never need a lock or wait. Destroying the cache does not
// wait for discovery and cannot leave a worker referring to its former owner.
class TargetVersionCache final {
public:
    using Catalog = std::vector<TargetVersion>;
    using Scanner = std::function<Catalog()>;

    struct RefreshResult {
        bool changed = false;
        std::uint64_t elapsed_ms = 0;
        std::string error;
    };

    TargetVersionCache() = default;
    ~TargetVersionCache() = default;
    TargetVersionCache(const TargetVersionCache&) = delete;
    TargetVersionCache& operator=(const TargetVersionCache&) = delete;
    TargetVersionCache(TargetVersionCache&&) = delete;
    TargetVersionCache& operator=(TargetVersionCache&&) = delete;

    // Duplicate requests coalesce until poll consumes the current refresh.
    // Thread creation failures can throw.
    bool refresh(Scanner scan);
    std::optional<RefreshResult> poll();

    const Catalog& snapshot() const noexcept { return catalog_; }
    bool initialized() const noexcept { return initialized_; }
    bool refreshing() const noexcept { return pending_ != nullptr; }

private:
    struct PendingRefresh;
    Catalog catalog_;
    std::shared_ptr<PendingRefresh> pending_;
    bool initialized_ = false;
};

} // namespace launcher
