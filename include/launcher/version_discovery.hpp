#pragma once

#include "launcher/target_version_scan.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace launcher {

enum class VersionSearchKind { target_pattern, adobe_catalog };

// The binding/provider adapter supplies an owned, canonical identity: expanded
// absolute pattern (one location) for a generic search, or additional roots and
// their product-family hint for Adobe. Case/path normalization, Adobe root ordering and any default-root
// representation belong to that adapter, preserving wildcard/.. rules.
// Launch arguments, selectors, item identity and activation are not key fields.
struct VersionSearchKey {
    VersionSearchKind kind = VersionSearchKind::target_pattern;
    std::vector<std::wstring> locations;
    std::uint32_t semantic_revision = 1;
    std::wstring product{}; // Adobe family hint or executable pattern; empty for the default shared catalog.

    auto operator<=>(const VersionSearchKey&) const = default;
};

// Externally owned execution boundary. start either accepts the work exactly
// once and returns, or throws before accepting it. Execution may be inline.
// Accepted work owns its captures; it must never refer to a destroyed executor.
// The executor must outlive the service. An asynchronous adapter owns and reaps
// its threads. Destroying the service does not destroy this executor, wait for
// work, or establish safe process exit.
class VersionDiscoveryExecutor {
public:
    using Work = std::function<void()>;
    virtual ~VersionDiscoveryExecutor() = default;
    virtual void start(Work work) = 0;
};

enum class VersionRequestReason { activation, attention };
enum class VersionRequestResult { ignored, already_checked, joined, queued, started, start_failed };
enum class VersionDiscoveryActivity { idle, queued, running };
enum class VersionDiscoveryFailure { none, start, scanner };

struct VersionDiscoveryAttempt {
    // Zero is reserved for the preallocated allocation-failure fallback.
    std::uint64_t id = 0;
    TargetVersionScanResult observation{TargetScanStatus::failed};
    VersionDiscoveryFailure failure = VersionDiscoveryFailure::scanner;
    std::string error = "Version discovery failed.";
};

struct VersionDiscoveryState {
    VersionDiscoveryActivity activity = VersionDiscoveryActivity::idle;
    // null means never successfully checked; a present empty catalog is known
    // empty. Handles remain valid across later collections and key removal.
    std::shared_ptr<const std::vector<TargetVersion>> snapshot;
    std::shared_ptr<const VersionDiscoveryAttempt> last_attempt;
    std::uint64_t catalog_revision = 0;
    // Queuing alone does not consume an attempt; starting/joining work does.
    bool checked_this_activation = false;
};

struct VersionDiscoveryLimits {
    // Activation admission requires total active < active - attention_reserve;
    // attention can use the entire active limit. Invalid limits are rejected.
    std::size_t active = 16;
    std::size_t attention_reserve = 4;
};

// All methods run on one owner/UI thread; only private job publication crosses
// threads. The core is independent of windows, timers and notification transport;
// the application supplies its executor and explicitly collects published work.
class VersionDiscoveryService final {
public:
    using Activation = std::uint64_t;
    // Each job copies this callable and its key. Captures must own everything
    // the scan uses, and separate jobs may call their copies concurrently.
    using Scanner = std::function<TargetVersionScanResult(const VersionSearchKey&)>;

    VersionDiscoveryService(Scanner scanner, VersionDiscoveryExecutor& executor, VersionDiscoveryLimits limits = {});
    ~VersionDiscoveryService();
    VersionDiscoveryService(const VersionDiscoveryService&) = delete;
    VersionDiscoveryService& operator=(const VersionDiscoveryService&) = delete;
    VersionDiscoveryService(VersionDiscoveryService&&) = delete;
    VersionDiscoveryService& operator=(VersionDiscoveryService&&) = delete;

    // Configuration retention is independent of the currently visible keys.
    // Removed running keys remain registered until collected (tombstones).
    void set_searches(std::span<const VersionSearchKey> keys);
    // Update shown bindings after reload, without a new activation or scan.
    // Both visibility APIs reject unconfigured keys before changing state.
    void set_visible_searches(std::span<const VersionSearchKey> visible_keys);
    // Begin a new shown-tab visit and request its distinct keys. Hidden keys
    // never start here. New visibility takes effect before admitting more work.
    // After capturing terminal user intents, pass false to join any publication
    // racing that capture instead of replacing its job before intent handling.
    Activation begin_activation(std::span<const VersionSearchKey> visible_keys, bool collect_published = true);
    void deactivate();
    // Stale tokens and keys outside the current shown tab are ignored.
    VersionRequestResult ensure_checked(const VersionSearchKey& key, Activation activation, VersionRequestReason reason);
    // Retain one explicit user intent against currently queued/running work.
    // Pinned queues use attention capacity and survive tab hiding/deactivation;
    // retaining an idle/terminal search never starts another attempt. Each true
    // return requires release_pending, including after completion. Removal from
    // configured searches cancels all pins belonging to that binding.
    bool retain_pending(const VersionSearchKey& key);
    void release_pending(const VersionSearchKey& key);
    // Drop a never-started reservation, allowing a later request in this same
    // visit to queue it again. Running or pinned work cannot be canceled here.
    bool discard_queued(const VersionSearchKey& key);
    // Explicit drain, called by a UI notification adapter. Returns the number
    // of attempts collected. Before a visibility transition, pass false to
    // capture terminal user intents without admitting the tab's obsolete queue.
    // No waiting, timer or periodic readiness polling is performed here.
    std::size_t collect_ready(bool admit_queued = true);
    std::optional<VersionDiscoveryState> state(const VersionSearchKey& key) const;
    Activation activation() const noexcept;
    std::size_t active_count() const noexcept;
    std::size_t queued_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace launcher
