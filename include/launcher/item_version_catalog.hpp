#pragma once

#include "launcher/config.hpp"
#include "launcher/target_versions.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace launcher {

enum class ItemVersionSelectionState { not_checked, available, no_eligible_version, pinned_missing };

struct ItemVersionSelection {
    // The pointed-to installation remains valid while this catalog or a copy
    // retaining its immutable version list remains alive.
    const TargetVersion* version = nullptr;
    ItemVersionSelectionState state = ItemVersionSelectionState::not_checked;
    bool automatic = true;
};

// An item's view of one adopted discovery snapshot. Generic snapshots already
// contain only their search's versions; Adobe snapshots are filtered/sorted
// once here. Copies share both immutable lists, and subsequent queries neither
// discover versions nor allocate. The original snapshot identity is retained
// independently of the filtered list for presentation-generation comparisons.
class ItemVersionCatalog final {
public:
    using Snapshot = std::shared_ptr<const std::vector<TargetVersion>>;

    ItemVersionCatalog() = default;
    ItemVersionCatalog(const LaunchAction& action, Snapshot snapshot);

    const Snapshot& snapshot() const noexcept;
    std::span<const TargetVersion> versions() const noexcept;
    std::size_t size() const noexcept;
    bool empty() const noexcept;
    bool multiple() const noexcept;
    // Automatic selection respects the item's Adobe Beta policy. Explicit
    // selections can name any edition offered in versions(), including Beta.
    const TargetVersion* latest() const noexcept;
    // Exact executable identity, compared case-insensitively without filesystem
    // access. A missing pin stays unavailable instead of selecting the latest.
    const TargetVersion* find(const std::filesystem::path& executable) const noexcept;
    ItemVersionSelection select(const std::optional<std::filesystem::path>& pinned = std::nullopt) const noexcept;

private:
    Snapshot snapshot_;
    Snapshot versions_;
    bool include_beta_ = true;
};

} // namespace launcher
