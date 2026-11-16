#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/target_version_cache.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace launcher {

struct TargetVersionCache::PendingRefresh final {
    std::atomic<bool> ready = false;
    Catalog catalog;
    bool success = false;
    std::uint64_t elapsed_ms = 0;
    std::string error = "Version discovery failed.";
};

bool TargetVersionCache::refresh(Scanner scan) {
    if (pending_) { return false; }
    auto pending = std::make_shared<PendingRefresh>();
    // Capture neither the cache nor its UI owner. The shared publication state
    // remains alive even if the window/cache is destroyed before discovery ends.
    std::thread([pending, scan = std::move(scan)] {
        const auto started = std::chrono::steady_clock::now();
        try {
            pending->catalog = scan();
            pending->success = true;
            pending->error.clear();
        } catch (const std::exception& error) {
            // Keep the preallocated fallback if reporting an allocation failure
            // would itself allocate and throw out of the thread entry point.
            try { pending->error = error.what(); } catch (...) {}
        } catch (...) {
            // Retain the generic error for non-standard scanner exceptions.
        }
        pending->elapsed_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count());
        pending->ready.store(true, std::memory_order_release);
    }).detach();
    pending_ = std::move(pending);
    return true;
}

std::optional<TargetVersionCache::RefreshResult> TargetVersionCache::poll() {
    if (!pending_ || !pending_->ready.load(std::memory_order_acquire)) { return std::nullopt; }
    auto pending = std::exchange(pending_, {});
    RefreshResult result{false, pending->elapsed_ms, std::move(pending->error)};
    if (pending->success) {
        result.changed = !initialized_ || catalog_ != pending->catalog;
        if (result.changed) { catalog_ = std::move(pending->catalog); }
        initialized_ = true;
    }
    return result;
}

} // namespace launcher
