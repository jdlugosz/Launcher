#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/version_discovery.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

namespace launcher {

struct VersionDiscoveryService::Impl final {
    struct Job final {
        std::atomic<bool> ready = false;
        std::shared_ptr<VersionDiscoveryAttempt> attempt;
    };

    struct Record final {
        bool configured = false;
        bool visible = false;
        bool queued = false;
        VersionRequestReason priority = VersionRequestReason::activation;
        std::uint64_t queue_order = 0;
        Activation queued_activation = 0;
        Activation attempted_activation = 0;
        std::size_t pending_refs = 0;
        std::shared_ptr<Job> job;
        std::shared_ptr<const std::vector<TargetVersion>> snapshot;
        std::shared_ptr<const VersionDiscoveryAttempt> last_attempt;
        std::uint64_t catalog_revision = 0;
    };

    Scanner scanner;
    VersionDiscoveryExecutor& executor;
    VersionDiscoveryLimits limits;
    std::map<VersionSearchKey, Record> records;
    Activation current_activation = 0;
    Activation next_activation = 0;
    std::uint64_t next_attempt = 0;
    std::uint64_t next_queue_order = 0;
    std::size_t active = 0;
    std::shared_ptr<const VersionDiscoveryAttempt> allocation_failure;

    Impl(Scanner scan, VersionDiscoveryExecutor& run, VersionDiscoveryLimits capacity)
        : scanner(std::move(scan)), executor(run), limits(capacity) {
        if (!scanner) { throw std::invalid_argument("Version discovery requires a scanner."); }
        if (limits.active == 0 || limits.attention_reserve > limits.active) {
            throw std::invalid_argument("Invalid version discovery admission limits.");
        }
        auto fallback = std::make_shared<VersionDiscoveryAttempt>();
        fallback->failure = VersionDiscoveryFailure::start;
        fallback->error = "Unable to allocate version discovery work.";
        allocation_failure = std::move(fallback);
    }

    std::set<VersionSearchKey> visible_set(std::span<const VersionSearchKey> keys) const {
        std::set<VersionSearchKey> visible(keys.begin(), keys.end());
        for (const auto& key : visible) {
            const auto found = records.find(key);
            if (found == records.end() || !found->second.configured) {
                throw std::invalid_argument("Visible version search is not configured.");
            }
        }
        return visible;
    }

    void update_visibility(const std::set<VersionSearchKey>& visible) noexcept {
        for (auto& [key, record] : records) {
            record.visible = record.configured && visible.contains(key);
            if (!record.visible && record.pending_refs == 0) { record.queued = false; }
        }
    }

    void erase_unreferenced() noexcept {
        std::erase_if(records, [](const auto& entry) {
            return !entry.second.configured && !entry.second.job;
        });
    }

    void queue(Record& record, VersionRequestReason reason) noexcept {
        if (!record.queued) {
            record.queued = true;
            record.queue_order = ++next_queue_order;
            record.queued_activation = current_activation;
            record.priority = reason;
        } else if (reason == VersionRequestReason::attention) {
            record.priority = reason;
        }
    }

    static void describe_failure(VersionDiscoveryAttempt& attempt, const std::exception& error) noexcept {
        try { attempt.error = error.what(); } catch (...) {}
    }

    void start(const VersionSearchKey& key, Record& record) noexcept {
        // Reserving capacity and consuming the attempt precede executor entry:
        // an executor is allowed to finish the entire job before returning.
        record.queued = false;
        record.attempted_activation = record.queued_activation;
        std::shared_ptr<VersionDiscoveryAttempt> attempt;
        try {
            attempt = std::make_shared<VersionDiscoveryAttempt>();
            attempt->id = ++next_attempt;
            attempt->failure = VersionDiscoveryFailure::start;
            auto job = std::make_shared<Job>();
            job->attempt = attempt;
            VersionDiscoveryExecutor::Work work = [job, scan = scanner, search = key]() noexcept {
                auto& result = *job->attempt;
                result.failure = VersionDiscoveryFailure::scanner;
                try {
                    result.observation = scan(search);
                    result.failure = VersionDiscoveryFailure::none;
                    result.error.clear();
                } catch (const std::exception& error) {
                    describe_failure(result, error);
                } catch (...) {
                    // Keep the preallocated failed observation and message.
                }
                // Everything read by the owner is immutable after publication.
                job->ready.store(true, std::memory_order_release);
            };
            record.job = std::move(job);
            ++active;
            executor.start(std::move(work));
        } catch (const std::exception& error) {
            if (attempt) { describe_failure(*attempt, error); }
            record.last_attempt = attempt ? std::move(attempt) : allocation_failure;
            if (record.job) { record.job.reset(); --active; }
        } catch (...) {
            record.last_attempt = attempt ? std::move(attempt) : allocation_failure;
            if (record.job) { record.job.reset(); --active; }
        }
    }

    void admit() noexcept {
        while (active < limits.active) {
            auto selected = records.end();
            for (auto entry = records.begin(); entry != records.end(); ++entry) {
                const auto& record = entry->second;
                const bool requested = (record.visible && current_activation != 0) || record.pending_refs != 0;
                if (!record.configured || !requested || !record.queued) { continue; }
                if (selected == records.end() ||
                    (record.priority == VersionRequestReason::attention && selected->second.priority != record.priority) ||
                    (record.priority == selected->second.priority && record.queue_order < selected->second.queue_order)) {
                    selected = entry;
                }
            }
            if (selected == records.end()) { return; }
            if (selected->second.priority == VersionRequestReason::activation &&
                active >= limits.active - limits.attention_reserve) { return; }
            start(selected->first, selected->second);
        }
    }

    std::size_t collect() noexcept {
        std::size_t count = 0;
        for (auto& [key, record] : records) {
            if (!record.job || !record.job->ready.load(std::memory_order_acquire)) { continue; }
            std::shared_ptr<const VersionDiscoveryAttempt> attempt = record.job->attempt;
            if (attempt->failure == VersionDiscoveryFailure::none && attempt->observation.status == TargetScanStatus::complete) {
                const auto& versions = attempt->observation.versions;
                if (!record.snapshot || *record.snapshot != versions) {
                    // An alias keeps the immutable worker-owned catalog alive
                    // without copying entries or allocating during collection.
                    record.snapshot = {attempt, &attempt->observation.versions};
                    ++record.catalog_revision;
                }
            }
            record.last_attempt = std::move(attempt);
            record.job.reset();
            --active;
            ++count;
        }
        erase_unreferenced();
        return count;
    }
};

VersionDiscoveryService::VersionDiscoveryService(Scanner scanner, VersionDiscoveryExecutor& executor, VersionDiscoveryLimits limits)
    : impl_(std::make_unique<Impl>(std::move(scanner), executor, limits)) {}

VersionDiscoveryService::~VersionDiscoveryService() = default;

void VersionDiscoveryService::set_searches(std::span<const VersionSearchKey> keys) {
    const std::set<VersionSearchKey> configured(keys.begin(), keys.end());
    // Finish potentially allocating inserts before changing existing bindings.
    // If insertion fails, remove only the unconfigured, idle nodes just added.
    try {
        for (const auto& key : configured) { impl_->records.try_emplace(key); }
    } catch (...) {
        impl_->erase_unreferenced();
        throw;
    }
    for (auto& [key, record] : impl_->records) {
        record.configured = configured.contains(key);
        if (!record.configured) { record.visible = false; record.queued = false; record.pending_refs = 0; }
    }
    impl_->erase_unreferenced();
}

void VersionDiscoveryService::set_visible_searches(std::span<const VersionSearchKey> visible_keys) {
    impl_->update_visibility(impl_->visible_set(visible_keys));
}

VersionDiscoveryService::Activation VersionDiscoveryService::begin_activation(std::span<const VersionSearchKey> visible_keys, bool collect_published) {
    const auto visible = impl_->visible_set(visible_keys);
    if (impl_->next_activation == (std::numeric_limits<Activation>::max)()) {
        throw std::overflow_error("Version discovery activation tokens exhausted.");
    }
    impl_->update_visibility(visible);
    impl_->current_activation = ++impl_->next_activation;
    // Visibility must change before collecting: a just-finished old job must
    // not admit queued work from the tab that is being hidden.
    if (collect_published) { impl_->collect(); }
    for (auto& [key, record] : impl_->records) {
        if (!record.visible) { continue; }
        if (record.job) {
            record.attempted_activation = impl_->current_activation;
        } else {
            // Attention belongs to the visit in which it was requested.
            if (record.queued) {
                record.queued_activation = impl_->current_activation;
                record.priority = record.pending_refs != 0 ? VersionRequestReason::attention : VersionRequestReason::activation;
            }
            impl_->queue(record, VersionRequestReason::activation);
        }
    }
    impl_->admit();
    return impl_->current_activation;
}

void VersionDiscoveryService::deactivate() {
    impl_->current_activation = 0;
    for (auto& [key, record] : impl_->records) {
        record.visible = false;
        if (record.pending_refs == 0) { record.queued = false; }
    }
    collect_ready();
}

VersionRequestResult VersionDiscoveryService::ensure_checked(const VersionSearchKey& key, Activation activation, VersionRequestReason reason) {
    if (activation == 0 || activation != impl_->current_activation) { return VersionRequestResult::ignored; }
    auto entry = impl_->records.find(key);
    if (entry == impl_->records.end() || !entry->second.configured || !entry->second.visible) {
        return VersionRequestResult::ignored;
    }
    // Honor attention before the completion drain admits queued replacements.
    // This also prevents a newly free speculative slot from skipping its icon.
    const bool was_running = entry->second.job != nullptr;
    const bool was_attempted = entry->second.attempted_activation == activation;
    if (entry->second.queued && reason == VersionRequestReason::attention) { entry->second.priority = reason; }
    collect_ready();
    auto& record = entry->second;
    if (!was_running && !was_attempted && record.attempted_activation == activation) {
        const bool rejected = !record.job && record.last_attempt && record.last_attempt->failure == VersionDiscoveryFailure::start;
        return rejected ? VersionRequestResult::start_failed : VersionRequestResult::started;
    }
    if (record.job) {
        record.attempted_activation = activation;
        return VersionRequestResult::joined;
    }
    if (record.attempted_activation == activation) { return VersionRequestResult::already_checked; }
    impl_->queue(record, reason);
    impl_->admit();
    if (record.job) { return VersionRequestResult::started; }
    return record.queued ? VersionRequestResult::queued : VersionRequestResult::start_failed;
}

bool VersionDiscoveryService::retain_pending(const VersionSearchKey& key) {
    const auto found = impl_->records.find(key);
    if (found == impl_->records.end()) { return false; }
    auto& record = found->second;
    if (!record.configured || (!record.queued && !record.job) || record.pending_refs == (std::numeric_limits<std::size_t>::max)()) {
        return false;
    }
    ++record.pending_refs;
    if (record.queued) {
        record.priority = VersionRequestReason::attention;
        impl_->admit();
    }
    return true;
}

void VersionDiscoveryService::release_pending(const VersionSearchKey& key) {
    const auto found = impl_->records.find(key);
    if (found == impl_->records.end() || found->second.pending_refs == 0) { return; }
    auto& record = found->second;
    --record.pending_refs;
    if (record.pending_refs == 0 && !record.visible) { record.queued = false; }
}

bool VersionDiscoveryService::discard_queued(const VersionSearchKey& key) {
    const auto found = impl_->records.find(key);
    if (found == impl_->records.end() || !found->second.configured || !found->second.queued || found->second.pending_refs != 0) {
        return false;
    }
    found->second.queued = false;
    return true;
}

std::size_t VersionDiscoveryService::collect_ready(bool admit_queued) {
    if (!admit_queued) { return impl_->collect(); }
    std::size_t count = 0;
    for (;;) {
        count += impl_->collect();
        impl_->admit();
        // Inline executors may have completed freshly admitted work. Drain it
        // too, without waiting for asynchronous jobs or rescanning this visit.
        bool ready = false;
        for (const auto& [key, record] : impl_->records) {
            if (record.job && record.job->ready.load(std::memory_order_acquire)) { ready = true; break; }
        }
        if (!ready) { return count; }
    }
}

std::optional<VersionDiscoveryState> VersionDiscoveryService::state(const VersionSearchKey& key) const {
    const auto found = impl_->records.find(key);
    if (found == impl_->records.end() || !found->second.configured) { return std::nullopt; }
    const auto& record = found->second;
    const auto activity = record.job ? VersionDiscoveryActivity::running :
        record.queued ? VersionDiscoveryActivity::queued : VersionDiscoveryActivity::idle;
    return VersionDiscoveryState{activity, record.snapshot, record.last_attempt, record.catalog_revision,
        impl_->current_activation != 0 && record.visible && record.attempted_activation == impl_->current_activation};
}

VersionDiscoveryService::Activation VersionDiscoveryService::activation() const noexcept { return impl_->current_activation; }
std::size_t VersionDiscoveryService::active_count() const noexcept { return impl_->active; }

std::size_t VersionDiscoveryService::queued_count() const noexcept {
    std::size_t count = 0;
    for (const auto& [key, record] : impl_->records) { if (record.queued) { ++count; } }
    return count;
}

} // namespace launcher
