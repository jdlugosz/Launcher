#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/target_version_cache.hpp"
#include "launcher/adobe_cc.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

using Cache = launcher::TargetVersionCache;
using namespace std::chrono_literals;

Cache::Catalog catalog(std::uint16_t major) {
    launcher::AdobeCCInstallation installation;
    installation.name = L"Adobe After Effects";
    installation.product_name = L"Adobe After Effects 2026";
    installation.product_version = std::to_wstring(major) + L".5";
    installation.file_version = installation.product_version;
    installation.version = {major, 5, 0, 1};
    installation.file_version_numbers = installation.version;
    installation.executable = L"C:\\Adobe\\AfterFX-" + std::to_wstring(major) + L".exe";
    return {std::move(installation)};
}

std::optional<Cache::RefreshResult> await_refresh(Cache& cache) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        if (auto result = cache.poll()) { return result; }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return std::nullopt;
}

struct Gate final {
    std::promise<void> entered;
    std::promise<void> permission;
    std::shared_future<void> allowed = permission.get_future().share();
    std::atomic<bool> released = false;

    void open() {
        if (!released.exchange(true)) { permission.set_value(); }
    }
};

// Assertions may throw while the scanner is blocked. Always release it during
// unwinding so a failed test cannot strand its independent worker.
struct ReleaseGuard final {
    std::shared_ptr<Gate> gate;
    ~ReleaseGuard() { gate->open(); }
};

} // namespace

TEST_CASE("Adobe cache starts discovery without waiting and coalesces refresh requests", "[adobe-cache]") {
    Cache cache;
    REQUIRE_FALSE(cache.initialized());
    REQUIRE_FALSE(cache.refreshing());
    REQUIRE(cache.snapshot().empty());
    REQUIRE_FALSE(cache.poll().has_value());
    const auto gate = std::make_shared<Gate>();
    const ReleaseGuard release{gate};
    auto entered = gate->entered.get_future();
    const auto discovered = catalog(26);
    REQUIRE(cache.refresh([gate, discovered] {
        gate->entered.set_value();
        gate->allowed.wait();
        return discovered;
    }));
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    REQUIRE(cache.refreshing());
    REQUIRE_FALSE(cache.initialized());
    REQUIRE(cache.snapshot().empty());
    REQUIRE_FALSE(cache.poll().has_value());
    const auto redundant_scans = std::make_shared<std::atomic<unsigned>>(0);
    REQUIRE_FALSE(cache.refresh([redundant_scans] {
        ++*redundant_scans;
        return Cache::Catalog{};
    }));
    REQUIRE(redundant_scans->load() == 0);
    gate->open();
    const auto result = await_refresh(cache);
    REQUIRE(result.has_value());
    REQUIRE(result->changed);
    REQUIRE(result->error.empty());
    REQUIRE(cache.initialized());
    REQUIRE_FALSE(cache.refreshing());
    REQUIRE(cache.snapshot() == discovered);
    REQUIRE_FALSE(cache.poll().has_value());
}

TEST_CASE("Adobe cache preserves the usable snapshot until a changed refresh is adopted", "[adobe-cache]") {
    Cache cache;
    const auto original = catalog(25);
    REQUIRE(cache.refresh([original] { return original; }));
    REQUIRE(await_refresh(cache).has_value());
    const auto gate = std::make_shared<Gate>();
    const ReleaseGuard release{gate};
    auto entered = gate->entered.get_future();
    const auto updated = catalog(26);
    REQUIRE(cache.refresh([gate, updated] {
        gate->entered.set_value();
        gate->allowed.wait();
        return updated;
    }));
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    REQUIRE(cache.snapshot() == original);
    const auto usable = launcher::adobe_cc_latest(cache.snapshot(), L"After Effects");
    REQUIRE(usable.has_value());
    REQUIRE(usable->version[0] == 25);
    REQUIRE_FALSE(cache.poll().has_value());
    gate->open();
    const auto changed = await_refresh(cache);
    REQUIRE(changed.has_value());
    REQUIRE(changed->changed);
    REQUIRE(cache.snapshot() == updated);
    const auto* retained_storage = cache.snapshot().data();
    REQUIRE(cache.refresh([updated] { return updated; }));
    const auto unchanged = await_refresh(cache);
    REQUIRE(unchanged.has_value());
    REQUIRE_FALSE(unchanged->changed);
    REQUIRE(unchanged->error.empty());
    REQUIRE(cache.snapshot().data() == retained_storage);
    REQUIRE(cache.refresh([] { return Cache::Catalog{}; }));
    const auto removed = await_refresh(cache);
    REQUIRE(removed.has_value());
    REQUIRE(removed->changed);
    REQUIRE(cache.initialized());
    REQUIRE(cache.snapshot().empty());
}

TEST_CASE("Adobe cache initializes after an empty successful discovery", "[adobe-cache]") {
    Cache cache;
    REQUIRE(cache.refresh([] { return Cache::Catalog{}; }));
    const auto initial = await_refresh(cache);
    REQUIRE(initial.has_value());
    REQUIRE(initial->changed);
    REQUIRE(initial->error.empty());
    REQUIRE(cache.initialized());
    REQUIRE(cache.snapshot().empty());
    REQUIRE(cache.refresh([] { return Cache::Catalog{}; }));
    const auto repeated = await_refresh(cache);
    REQUIRE(repeated.has_value());
    REQUIRE_FALSE(repeated->changed);
}

TEST_CASE("Adobe cache reports scanner failures and retains its last good snapshot", "[adobe-cache]") {
    Cache cache;
    REQUIRE(cache.refresh([]() -> Cache::Catalog { throw std::runtime_error("initial scanner failure"); }));
    const auto initial = await_refresh(cache);
    REQUIRE(initial.has_value());
    REQUIRE_FALSE(initial->changed);
    REQUIRE(initial->error == "initial scanner failure");
    REQUIRE_FALSE(cache.initialized());
    REQUIRE_FALSE(cache.refreshing());
    const auto good = catalog(26);
    REQUIRE(cache.refresh([good] { return good; }));
    REQUIRE(await_refresh(cache).has_value());
    REQUIRE(cache.initialized());
    REQUIRE(cache.refresh([]() -> Cache::Catalog { throw std::runtime_error("later scanner failure"); }));
    const auto failed = await_refresh(cache);
    REQUIRE(failed.has_value());
    REQUIRE_FALSE(failed->changed);
    REQUIRE(failed->error == "later scanner failure");
    REQUIRE(cache.initialized());
    REQUIRE(cache.snapshot() == good);
    REQUIRE(cache.refresh([]() -> Cache::Catalog { throw 42; }));
    const auto unknown = await_refresh(cache);
    REQUIRE(unknown.has_value());
    REQUIRE_FALSE(unknown->changed);
    REQUIRE_FALSE(unknown->error.empty());
    REQUIRE(cache.snapshot() == good);
}

TEST_CASE("Adobe cache can be destroyed while its independent scanner is blocked", "[adobe-cache]") {
    auto cache = std::make_unique<Cache>();
    const auto gate = std::make_shared<Gate>();
    const ReleaseGuard release{gate};
    auto entered = gate->entered.get_future();
    const auto completion = std::make_shared<std::promise<void>>();
    auto finished = completion->get_future();
    // Only the worker's scanner capture retains this token after refresh.
    // Its destruction confirms the detached worker has released that capture.
    auto lifetime = std::shared_ptr<int>(new int(0), [completion](int* value) {
        delete value;
        completion->set_value();
    });
    REQUIRE(cache->refresh([gate, lifetime] {
        gate->entered.set_value();
        gate->allowed.wait();
        return catalog(26);
    }));
    lifetime.reset();
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    cache.reset(); // Must return without waiting for the blocked scanner.
    REQUIRE(finished.wait_for(0ms) == std::future_status::timeout);
    gate->open();
    REQUIRE(finished.wait_for(2s) == std::future_status::ready);
}
