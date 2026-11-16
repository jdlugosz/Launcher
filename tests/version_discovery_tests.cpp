#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/version_discovery.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using Key = launcher::VersionSearchKey;
using Kind = launcher::VersionSearchKind;
using Service = launcher::VersionDiscoveryService;
using Executor = launcher::VersionDiscoveryExecutor;
using Scan = launcher::TargetVersionScanResult;
using ScanStatus = launcher::TargetScanStatus;
using Activity = launcher::VersionDiscoveryActivity;
using Failure = launcher::VersionDiscoveryFailure;
using Reason = launcher::VersionRequestReason;
using Request = launcher::VersionRequestResult;
using namespace std::chrono_literals;

Key key(std::wstring name, Kind kind = Kind::target_pattern) {
    return {kind, {L"C:\\Version discovery fixtures\\" + name}, 1};
}

launcher::TargetVersion version(std::wstring name, std::uint16_t major = 1) {
    launcher::TargetVersion result;
    result.name = name;
    result.executable = std::filesystem::path(L"C:\\Version discovery fixtures") / name / L"app.exe";
    result.version = {major, 0, 0, 0};
    return result;
}

Scan complete(std::wstring name = L"first", std::uint16_t major = 1) {
    Scan result;
    result.versions.push_back(version(std::move(name), major));
    return result;
}

Scan unsuccessful(ScanStatus status) {
    auto result = complete(L"partial evidence");
    result.status = status;
    result.diagnostics.push_back({launcher::TargetScanOperation::enumerate, L"C:\\unavailable", std::make_error_code(std::errc::permission_denied)});
    return result;
}

class ManualExecutor final : public Executor {
public:
    std::deque<Work> pending;
    bool reject_next = false;
    std::size_t starts = 0;

    void start(Work work) override {
        ++starts;
        if (std::exchange(reject_next, false)) { throw std::runtime_error("Scripted start failure"); }
        pending.push_back(std::move(work));
    }

    void run(std::size_t index = 0) {
        REQUIRE(index < pending.size());
        auto position = pending.begin() + static_cast<std::ptrdiff_t>(index);
        auto work = std::move(*position);
        pending.erase(position);
        work();
    }
};

class InlineExecutor final : public Executor {
public:
    std::size_t starts = 0;
    void start(Work work) override { ++starts; work(); }
};

struct Script final {
    using Reply = std::variant<Scan, std::exception_ptr>;
    std::map<Key, std::deque<Reply>> replies;
    std::vector<Key> calls;

    Scan scan(const Key& search) {
        calls.push_back(search);
        auto& queue = replies[search];
        if (queue.empty()) { return complete(search.locations.front()); }
        auto reply = std::move(queue.front());
        queue.pop_front();
        if (auto error = std::get_if<std::exception_ptr>(&reply)) { std::rethrow_exception(*error); }
        return std::move(std::get<Scan>(reply));
    }

};

Service::Scanner scanner(const std::shared_ptr<Script>& script) {
    return [script](const Key& search) { return script->scan(search); };
}

struct CopyFaultScanner final {
    explicit CopyFaultScanner(std::shared_ptr<bool> reject_copy) : reject(std::move(reject_copy)) {}
    CopyFaultScanner(const CopyFaultScanner& other) : reject(other.reject) {
        if (*reject) { throw std::bad_alloc{}; }
    }
    Scan operator()(const Key&) const { return complete(L"copy-safe result"); }
    std::shared_ptr<bool> reject;
};

void configure(Service& service, std::vector<Key> keys) { service.set_searches(keys); }
Service::Activation activate(Service& service, std::vector<Key> keys) { return service.begin_activation(keys); }

launcher::VersionDiscoveryState state(const Service& service, const Key& search) {
    auto result = service.state(search);
    REQUIRE(result.has_value());
    return *result;
}

// This adapter owns and joins threads independently of the service. Completion
// futures acknowledge return from Work, after its publication store has run.
class ThreadExecutor final : public Executor {
public:
    ThreadExecutor() { threads_.reserve(8); finished_.reserve(8); }
    ~ThreadExecutor() override { for (auto& thread : threads_) { thread.join(); } }

    void start(Work work) override {
        // Finish every potentially allocating container change before a worker
        // can accept Work, satisfying the executor's no-throw-after-accept rule.
        threads_.reserve(threads_.size() + 1);
        finished_.reserve(finished_.size() + 1);
        auto done = std::make_shared<std::promise<void>>();
        auto finished = done->get_future().share();
        std::thread worker([work = std::move(work), done]() mutable {
            try { work(); work = {}; done->set_value(); }
            catch (...) { done->set_exception(std::current_exception()); }
        });
        threads_.push_back(std::move(worker));
        finished_.push_back(std::move(finished));
    }

    bool finished(std::size_t index) {
        if (finished_.at(index).wait_for(5s) != std::future_status::ready) { return false; }
        finished_[index].get();
        return true;
    }

private:
    std::vector<std::thread> threads_;
    std::vector<std::shared_future<void>> finished_;
};

// Declare gates after the executor. Assertion unwinding opens them before the
// executor joins, so a failed assertion cannot strand one of its workers.
class Gate final {
public:
    Gate() : future_(promise_.get_future().share()) {}
    ~Gate() { open(); }
    std::shared_future<void> wait_handle() const { return future_; }
    void open() { if (!std::exchange(open_, true)) { promise_.set_value(); } }

private:
    std::promise<void> promise_;
    std::shared_future<void> future_;
    bool open_ = false;
};

} // namespace

TEST_CASE("Version discovery starts only configured visible distinct searches", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor);
    const auto visible = key(L"visible*");
    const auto hidden = key(L"hidden*");
    const auto unknown = key(L"unknown*");
    configure(service, {visible, hidden, visible});

    REQUIRE(service.activation() == 0);
    CHECK(service.active_count() == 0);
    CHECK_FALSE(state(service, visible).snapshot);
    CHECK_FALSE(state(service, visible).last_attempt);
    CHECK_FALSE(service.state(unknown));

    const auto visit = activate(service, {visible, visible});
    CHECK(visit != 0);
    CHECK(service.activation() == visit);
    CHECK(executor.starts == 1);
    CHECK(service.active_count() == 1);
    CHECK(service.queued_count() == 0);
    CHECK(state(service, visible).activity == Activity::running);
    CHECK(state(service, visible).checked_this_activation);
    CHECK(state(service, hidden).activity == Activity::idle);
    CHECK_FALSE(state(service, hidden).checked_this_activation);
    CHECK(service.ensure_checked(hidden, visit, Reason::attention) == Request::ignored);
    CHECK(service.ensure_checked(unknown, visit, Reason::attention) == Request::ignored);
    CHECK(service.ensure_checked(visible, visit, Reason::attention) == Request::joined);

    executor.run();
    CHECK(script->calls == std::vector<Key>{visible});
    CHECK(service.active_count() == 1); // Publication is collected explicitly.
    REQUIRE(service.collect_ready() == 1);
    CHECK(service.active_count() == 0);
    CHECK(service.ensure_checked(visible, visit, Reason::attention) == Request::already_checked);
    CHECK(service.collect_ready() == 0);
}

TEST_CASE("Version discovery ignores stale visits and shares work across a new visit", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor);
    const auto search = key(L"shared*");
    configure(service, {search});
    const auto first = activate(service, {search});
    const auto second = activate(service, {search});
    CHECK(second > first);
    CHECK(executor.starts == 1);
    CHECK(service.ensure_checked(search, first, Reason::attention) == Request::ignored);
    CHECK(service.ensure_checked(search, second, Reason::attention) == Request::joined);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(service.ensure_checked(search, second, Reason::activation) == Request::already_checked);
    service.deactivate();
    CHECK(service.ensure_checked(search, second, Reason::attention) == Request::ignored);
    const auto third = activate(service, {search});
    CHECK(third > second);
    CHECK(executor.starts == 2);
}

TEST_CASE("Version discovery validates limits and visible membership before changing state", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    CHECK_THROWS_AS(Service(scanner(script), executor, {0, 0}), std::invalid_argument);
    CHECK_THROWS_AS(Service(scanner(script), executor, {2, 3}), std::invalid_argument);
    Service service(scanner(script), executor, {1, 0});
    const auto first = key(L"first*");
    const auto queued = key(L"queued*");
    const std::vector<Key> unknown{key(L"unconfigured*")};
    configure(service, {first, queued});
    const auto visit = activate(service, {first, queued});
    CHECK_THROWS_AS(service.begin_activation(unknown), std::invalid_argument);
    CHECK(service.activation() == visit);
    CHECK(service.active_count() == 1);
    CHECK(service.queued_count() == 1);
    CHECK_THROWS_AS(service.set_visible_searches(unknown), std::invalid_argument);
    CHECK(service.activation() == visit);
    CHECK(service.queued_count() == 1);
    CHECK(state(service, first).activity == Activity::running);
    CHECK(state(service, queued).activity == Activity::queued);
}

TEST_CASE("Version discovery updates visible membership within a visit without scanning", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor, {2, 1});
    const auto first = key(L"first*");
    const auto second = key(L"second*");
    const auto added = key(L"added by reload*");
    configure(service, {first, second});
    const auto visit = activate(service, {first});
    const std::vector<Key> second_visible{second};
    service.set_visible_searches(second_visible);
    CHECK(service.activation() == visit);
    CHECK(executor.starts == 1);
    CHECK(service.ensure_checked(first, visit, Reason::attention) == Request::ignored);
    CHECK(service.ensure_checked(second, visit, Reason::attention) == Request::started);
    configure(service, {first, second, added});
    const std::vector<Key> reloaded_visible{second, added};
    service.set_visible_searches(reloaded_visible);
    CHECK(service.activation() == visit);
    CHECK(executor.starts == 2);
    CHECK(service.ensure_checked(added, visit, Reason::attention) == Request::queued);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(state(service, added).activity == Activity::running);
}

TEST_CASE("Version discovery publishes complete empty catalogs distinctly from never checked", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor);
    const auto search = key(L"empty*");
    script->replies[search].push_back(Scan{});
    configure(service, {search});
    activate(service, {search});
    CHECK_FALSE(state(service, search).snapshot);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    const auto result = state(service, search);
    REQUIRE(result.snapshot);
    CHECK(result.snapshot->empty());
    CHECK(result.catalog_revision == 1);
    REQUIRE(result.last_attempt);
    CHECK(result.last_attempt->observation.status == ScanStatus::complete);
    CHECK(result.last_attempt->failure == Failure::none);
    CHECK(result.last_attempt->error.empty());
}

TEST_CASE("Version discovery preserves immutable snapshots and revisions across observations", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor);
    const auto search = key(L"versions*");
    script->replies[search] = {complete(L"first", 1), complete(L"first", 1), complete(L"second", 2), Scan{}};
    configure(service, {search});
    activate(service, {search});
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    const auto first = state(service, search);
    REQUIRE(first.snapshot);
    REQUIRE(first.last_attempt);
    CHECK(first.catalog_revision == 1);

    activate(service, {search});
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    const auto unchanged = state(service, search);
    CHECK(unchanged.snapshot == first.snapshot);
    CHECK(unchanged.catalog_revision == first.catalog_revision);
    REQUIRE(unchanged.last_attempt);
    CHECK(unchanged.last_attempt != first.last_attempt);
    CHECK(unchanged.last_attempt->id > first.last_attempt->id);

    activate(service, {search});
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    const auto changed = state(service, search);
    REQUIRE(changed.snapshot);
    CHECK(changed.snapshot != first.snapshot);
    CHECK(changed.catalog_revision == first.catalog_revision + 1);
    CHECK(changed.snapshot->front().name == L"second");
    CHECK(first.snapshot->front().name == L"first");
    CHECK(first.last_attempt->observation.versions.front().name == L"first");

    activate(service, {search});
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    const auto empty = state(service, search);
    REQUIRE(empty.snapshot);
    CHECK(empty.snapshot->empty());
    CHECK(empty.catalog_revision == changed.catalog_revision + 1);
    CHECK(changed.snapshot->front().name == L"second");
}

TEST_CASE("Version discovery keeps incomplete and failed observations as evidence only", "[version-discovery]") {
    const auto status = GENERATE(ScanStatus::incomplete, ScanStatus::failed);
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor);
    const auto search = key(L"unreliable*");
    script->replies[search] = {unsuccessful(status), complete(L"known good"), unsuccessful(status)};
    configure(service, {search});

    activate(service, {search});
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    const auto never_successful = state(service, search);
    CHECK_FALSE(never_successful.snapshot);
    CHECK(never_successful.catalog_revision == 0);
    REQUIRE(never_successful.last_attempt);
    CHECK(never_successful.last_attempt->failure == Failure::none);
    CHECK(never_successful.last_attempt->observation.status == status);
    CHECK(never_successful.last_attempt->observation.versions.size() == 1);

    activate(service, {search});
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    const auto good = state(service, search);
    REQUIRE(good.snapshot);
    const auto visit = activate(service, {search});
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    const auto failed = state(service, search);
    CHECK(failed.snapshot == good.snapshot);
    CHECK(failed.catalog_revision == good.catalog_revision);
    CHECK(failed.last_attempt->observation.status == status);
    CHECK(failed.last_attempt->observation.diagnostics.size() == 1);
    CHECK(service.ensure_checked(search, visit, Reason::attention) == Request::already_checked);
}

TEST_CASE("Version discovery contains scanner exceptions and keeps previous catalogs", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor);
    const auto search = key(L"throws*");
    SECTION("standard exception") { script->replies[search].push_back(std::make_exception_ptr(std::runtime_error("Scripted scanner failure"))); }
    SECTION("allocation exception") { script->replies[search].push_back(std::make_exception_ptr(std::bad_alloc{})); }
    SECTION("nonstandard exception") { script->replies[search].push_back(std::make_exception_ptr(17)); }
    script->replies[search].push_front(complete(L"known good"));
    configure(service, {search});
    activate(service, {search});
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    const auto before = state(service, search);
    activate(service, {search});
    CHECK_NOTHROW(executor.run());
    REQUIRE(service.collect_ready() == 1);
    const auto after = state(service, search);
    CHECK(after.snapshot == before.snapshot);
    CHECK(after.catalog_revision == before.catalog_revision);
    REQUIRE(after.last_attempt);
    CHECK(after.last_attempt->failure == Failure::scanner);
    CHECK(after.last_attempt->observation.status == ScanStatus::failed);
    CHECK_FALSE(after.last_attempt->error.empty());
}

TEST_CASE("Version discovery records rejected starts and retries only on a new visit", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor);
    const auto search = key(L"start failure*");
    configure(service, {search});
    executor.reject_next = true;
    const auto first = activate(service, {search});
    CHECK(executor.pending.empty());
    CHECK(service.active_count() == 0);
    CHECK(service.queued_count() == 0);
    const auto rejected = state(service, search);
    CHECK(rejected.activity == Activity::idle);
    CHECK_FALSE(rejected.snapshot);
    REQUIRE(rejected.last_attempt);
    CHECK(rejected.last_attempt->failure == Failure::start);
    CHECK_FALSE(rejected.last_attempt->error.empty());
    CHECK(service.ensure_checked(search, first, Reason::attention) == Request::already_checked);
    CHECK(executor.starts == 1);

    activate(service, {search});
    REQUIRE(executor.starts == 2);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(state(service, search).snapshot);
    CHECK(state(service, search).last_attempt->failure == Failure::none);

    const auto good = state(service, search);
    executor.reject_next = true;
    const auto failed_visit = activate(service, {search});
    const auto after_rejection = state(service, search);
    CHECK(after_rejection.snapshot == good.snapshot);
    CHECK(after_rejection.catalog_revision == good.catalog_revision);
    CHECK(after_rejection.last_attempt->failure == Failure::start);
    CHECK(after_rejection.last_attempt->id > good.last_attempt->id);
    CHECK(service.ensure_checked(search, failed_visit, Reason::attention) == Request::already_checked);

    // The request itself can drain a completion and attempt a queued start.
    ManualExecutor draining_executor;
    Service draining_service(scanner(script), draining_executor, {1, 0});
    const auto queued = key(L"z queued start failure*");
    configure(draining_service, {search, queued});
    const auto draining_visit = activate(draining_service, {search, queued});
    draining_executor.run();
    draining_executor.reject_next = true;
    CHECK(draining_service.ensure_checked(queued, draining_visit, Reason::attention) == Request::start_failed);
    CHECK(draining_executor.starts == 2);
    CHECK(draining_executor.pending.empty());
    CHECK(draining_service.active_count() == 0);
    CHECK(draining_service.queued_count() == 0);
    CHECK(state(draining_service, queued).last_attempt->failure == Failure::start);
    CHECK(draining_service.ensure_checked(queued, draining_visit, Reason::attention) == Request::already_checked);
    CHECK(draining_executor.starts == 2);
}

TEST_CASE("Version discovery contains per-job scanner copy allocation failure before acceptance", "[version-discovery]") {
    ManualExecutor executor;
    auto reject_copy = std::make_shared<bool>(false);
    Service service(Service::Scanner{CopyFaultScanner(reject_copy)}, executor, {1, 1});
    const auto search = key(L"copy failure*");
    configure(service, {search});
    const auto first = activate(service, {search});
    REQUIRE(service.ensure_checked(search, first, Reason::attention) == Request::started);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    const auto good = state(service, search);

    *reject_copy = true;
    const auto second = activate(service, {search});
    CHECK(service.ensure_checked(search, second, Reason::attention) == Request::start_failed);
    CHECK(executor.starts == 1); // The throwing capture never reaches the executor.
    CHECK(executor.pending.empty());
    CHECK(service.active_count() == 0);
    CHECK(service.queued_count() == 0);
    const auto failed = state(service, search);
    CHECK(failed.snapshot == good.snapshot);
    CHECK(failed.catalog_revision == good.catalog_revision);
    REQUIRE(failed.last_attempt);
    CHECK(failed.last_attempt->failure == Failure::start);
    CHECK_FALSE(failed.last_attempt->error.empty());
    CHECK(failed.checked_this_activation);
    CHECK(service.ensure_checked(search, second, Reason::attention) == Request::already_checked);

    *reject_copy = false;
    const auto third = activate(service, {search});
    CHECK(service.ensure_checked(search, third, Reason::attention) == Request::started);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(state(service, search).snapshot == good.snapshot);
    CHECK(state(service, search).last_attempt->failure == Failure::none);
}

TEST_CASE("Version discovery collects inline execution through the same publication boundary", "[version-discovery]") {
    InlineExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor, {2, 1});
    const auto first = key(L"inline first*");
    const auto second = key(L"inline second*");
    configure(service, {first, second});
    activate(service, {first, second});
    CHECK(executor.starts == 1);
    CHECK(service.active_count() == 1);
    CHECK(service.queued_count() == 1);
    CHECK_FALSE(state(service, first).snapshot);
    REQUIRE(service.collect_ready() >= 1);
    // Collection may also drain the inline job it just admitted. Either way,
    // this second explicit drain leaves both catalogs available without a timer.
    service.collect_ready();
    CHECK(executor.starts == 2);
    CHECK(service.active_count() == 0);
    CHECK(state(service, first).snapshot);
    CHECK(state(service, second).snapshot);

    InlineExecutor draining_executor;
    Service draining_service(scanner(script), draining_executor, {1, 0});
    configure(draining_service, {first, second});
    const auto visit = activate(draining_service, {first, second});
    CHECK(draining_executor.starts == 1);
    CHECK(draining_service.queued_count() == 1);
    // This call drains A, admits B, and drains its inline completion. It still
    // reports that it started B rather than reporting joined/already_checked.
    CHECK(draining_service.ensure_checked(second, visit, Reason::attention) == Request::started);
    CHECK(draining_executor.starts == 2);
    CHECK(draining_service.active_count() == 0);
    CHECK(draining_service.queued_count() == 0);
    CHECK(state(draining_service, first).snapshot);
    CHECK(state(draining_service, second).snapshot);
    CHECK(draining_service.ensure_checked(second, visit, Reason::attention) == Request::already_checked);
    CHECK(draining_executor.starts == 2);
}

TEST_CASE("Version discovery reserves default capacity for attention and promotes queued work", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor);
    std::vector<Key> keys;
    for (unsigned index = 0; index < 18; ++index) {
        keys.push_back(key(L"search " + std::wstring(index < 10 ? L"0" : L"") + std::to_wstring(index)));
    }
    service.set_searches(keys);
    const auto visit = service.begin_activation(keys);
    CHECK(service.active_count() == 12);
    CHECK(service.queued_count() == 6);
    CHECK(executor.starts == 12);
    for (unsigned index = 12; index < 16; ++index) {
        CHECK(service.ensure_checked(keys[index], visit, Reason::attention) == Request::started);
    }
    CHECK(service.active_count() == 16);
    CHECK(service.queued_count() == 2);
    CHECK(service.ensure_checked(keys[17], visit, Reason::attention) == Request::queued);
    CHECK(service.queued_count() == 2); // Promotion does not duplicate a reservation.
    executor.run(15); // Complete an attention job before the older speculative jobs.
    REQUIRE(service.collect_ready() == 1);
    CHECK(service.active_count() == 16);
    CHECK(state(service, keys[17]).activity == Activity::running);
    CHECK(state(service, keys[16]).activity == Activity::queued);
    executor.run(15);
    REQUIRE(service.collect_ready() == 1);
    CHECK(service.active_count() == 15);
    CHECK(state(service, keys[16]).activity == Activity::queued); // Total active, not speculative count.
}

TEST_CASE("Version discovery admits attention ahead of speculative work after completion", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor, {2, 1});
    const auto first = key(L"0 first*");
    const auto speculative = key(L"1 speculative*");
    const auto attention = key(L"2 attention*");
    const auto later = key(L"3 later attention*");
    configure(service, {first, speculative, attention, later});
    const auto visit = activate(service, {first, speculative, attention, later});
    CHECK(service.ensure_checked(attention, visit, Reason::attention) == Request::started);
    CHECK(service.ensure_checked(later, visit, Reason::attention) == Request::queued);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(state(service, later).activity == Activity::running);
    CHECK(state(service, speculative).activity == Activity::queued);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(service.active_count() == 1);
    CHECK(state(service, speculative).activity == Activity::queued);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(state(service, speculative).activity == Activity::running);
}

TEST_CASE("Version discovery discards unstarted reservations without consuming their visit", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor, {2, 1});
    const auto running = key(L"0 running*");
    const auto queued = key(L"1 queued*");
    configure(service, {running, queued});
    const auto visit = activate(service, {running, queued});
    REQUIRE(service.queued_count() == 1);
    CHECK_FALSE(state(service, queued).checked_this_activation);
    CHECK_FALSE(service.discard_queued(running));
    CHECK(service.discard_queued(queued));
    CHECK_FALSE(service.discard_queued(queued));
    CHECK(service.queued_count() == 0);
    CHECK_FALSE(state(service, queued).checked_this_activation);
    CHECK_FALSE(state(service, queued).last_attempt);
    CHECK(service.ensure_checked(queued, visit, Reason::activation) == Request::queued);
    CHECK(service.ensure_checked(queued, visit, Reason::attention) == Request::started);
    CHECK(service.active_count() == 2);
}

TEST_CASE("Version discovery changes visibility before admitting any obsolete queue entries", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor, {1, 0});
    const auto old_running = key(L"0 old running*");
    const auto old_queued = key(L"1 old queued*");
    const auto new_visible = key(L"new visible*");
    configure(service, {old_running, old_queued, new_visible});
    const auto old_visit = activate(service, {old_running, old_queued});
    executor.run(); // Ready but not collected when the user switches tabs.
    const auto new_visit = activate(service, {new_visible});
    CHECK(new_visit > old_visit);
    CHECK(state(service, old_queued).activity == Activity::idle);
    CHECK(service.ensure_checked(old_queued, old_visit, Reason::attention) == Request::ignored);
    service.collect_ready();
    CHECK(executor.starts == 2);
    CHECK(state(service, new_visible).activity == Activity::running);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(script->calls == std::vector<Key>{old_running, new_visible});
    CHECK_FALSE(state(service, old_queued).last_attempt);
}

TEST_CASE("Version discovery can collect terminal results before changing tabs without admitting old queues", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor, {1, 0});
    const auto old_running = key(L"0 old running*");
    const auto old_queued = key(L"1 old queued*");
    const auto new_visible = key(L"2 new visible*");
    configure(service, {old_running, old_queued, new_visible});
    activate(service, {old_running, old_queued});
    REQUIRE(service.retain_pending(old_running));
    executor.run();
    REQUIRE(service.collect_ready(false) == 1);
    CHECK(state(service, old_running).snapshot);
    CHECK(state(service, old_queued).activity == Activity::queued);
    CHECK_FALSE(state(service, old_queued).last_attempt);
    CHECK(service.active_count() == 0);
    CHECK(executor.starts == 1);

    // The UI can now freeze the completed intent's exact result and release
    // its pin before the next activation is allowed to admit any work.
    service.release_pending(old_running);
    activate(service, {new_visible});
    CHECK(state(service, old_queued).activity == Activity::idle);
    CHECK(state(service, new_visible).activity == Activity::running);
    CHECK(executor.starts == 2);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(script->calls == std::vector<Key>{old_running, new_visible});
}

TEST_CASE("Version discovery joins publication racing a user-intent drain and activation transition", "[version-discovery]") {
    const auto successful = GENERATE(false, true);
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor, {1, 0});
    const auto search = key(L"pending at transition*");
    script->replies[search] = {successful ? complete(L"original exact result") : unsuccessful(ScanStatus::failed), complete(L"later attempt")};
    configure(service, {search});
    const auto first_visit = activate(service, {search});
    REQUIRE(service.retain_pending(search));
    const auto previous_attempt = state(service, search).last_attempt;
    REQUIRE(service.collect_ready(false) == 0);

    // Publication arrives after the UI's pre-transition drain. Leaving the
    // ready job registered makes this visit join its outcome rather than start
    // a replacement and make the original intent follow that newer attempt.
    executor.run();
    const std::vector<Key> visible{search};
    const auto second_visit = service.begin_activation(visible, false);
    CHECK(second_visit > first_visit);
    CHECK(executor.starts == 1);
    CHECK(state(service, search).activity == Activity::running);
    CHECK(state(service, search).checked_this_activation);
    REQUIRE(service.collect_ready(false) == 1);
    const auto terminal = state(service, search);
    CHECK(terminal.activity == Activity::idle);
    REQUIRE(terminal.last_attempt);
    CHECK(terminal.last_attempt != previous_attempt);
    CHECK(terminal.last_attempt->observation.status == (successful ? ScanStatus::complete : ScanStatus::failed));
    if (successful) {
        REQUIRE(terminal.snapshot);
        CHECK(terminal.snapshot->front().name == L"original exact result");
    } else {
        CHECK_FALSE(terminal.snapshot);
    }
    service.release_pending(search);
    CHECK(service.ensure_checked(search, second_visit, Reason::attention) == Request::already_checked);
    CHECK(executor.starts == 1);
    CHECK(service.collect_ready() == 0);

    // Only a later distinct visit may attempt the next scan; it cannot become
    // the completion belonging to the already-consumed first user intent.
    const auto third_visit = service.begin_activation(visible, false);
    CHECK(third_visit > second_visit);
    CHECK(executor.starts == 2);
}

TEST_CASE("Version discovery deactivation drops queued work and still collects running results", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor, {1, 0});
    const auto running = key(L"0 running*");
    const auto queued = key(L"1 queued*");
    configure(service, {running, queued});
    const auto visit = activate(service, {running, queued});
    service.deactivate();
    CHECK(service.queued_count() == 0);
    CHECK(state(service, queued).activity == Activity::idle);
    CHECK_FALSE(state(service, queued).checked_this_activation);
    CHECK(service.ensure_checked(queued, visit, Reason::attention) == Request::ignored);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(state(service, running).snapshot);
    CHECK(executor.starts == 1);
    activate(service, {queued});
    CHECK(executor.starts == 2);
}

TEST_CASE("Version discovery pending intents retain hidden queued work until capacity is free", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor, {1, 0});
    const auto running = key(L"0 held slot*");
    const auto requested = key(L"1 explicit pending*");
    configure(service, {running, requested});
    const auto visit = activate(service, {running, requested});
    REQUIRE(state(service, requested).activity == Activity::queued);
    REQUIRE(service.retain_pending(requested));
    CHECK_FALSE(service.discard_queued(requested));
    service.deactivate();
    CHECK(service.activation() == 0);
    CHECK(service.queued_count() == 1);
    CHECK(service.ensure_checked(requested, visit, Reason::attention) == Request::ignored);

    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(executor.starts == 2);
    CHECK(state(service, requested).activity == Activity::running);
    CHECK_FALSE(state(service, requested).checked_this_activation);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    REQUIRE(state(service, requested).snapshot);
    CHECK(state(service, requested).activity == Activity::idle);
    CHECK_FALSE(service.retain_pending(requested));
    service.release_pending(requested);
    CHECK(service.collect_ready() == 0);
    CHECK(executor.starts == 2);
}

TEST_CASE("Version discovery pending queues retain their own visit when another tab is shown", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor, {1, 0});
    const auto running = key(L"0 held slot*");
    const auto requested = key(L"1 explicit pending*");
    configure(service, {running, requested});
    const auto first_visit = activate(service, {running, requested});
    REQUIRE(service.retain_pending(requested));
    const auto second_visit = activate(service, {}); // A shown tab without version searches.
    REQUIRE(second_visit > first_visit);
    CHECK(state(service, requested).activity == Activity::queued);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(state(service, requested).activity == Activity::running);
    CHECK_FALSE(state(service, requested).checked_this_activation);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    service.release_pending(requested);

    // A binding update can expose this key during that same tab visit. The
    // hidden pinned job belonged to the first visit, so it must not have used
    // the second visit's allowance to perform its own explicit check.
    const std::vector<Key> newly_visible{requested};
    service.set_visible_searches(newly_visible);
    CHECK_FALSE(state(service, requested).checked_this_activation);
    CHECK(service.ensure_checked(requested, second_visit, Reason::attention) == Request::started);
    CHECK(executor.starts == 3);

    // A new visit while that replacement runs joins it without a duplicate.
    const auto third_visit = activate(service, {requested});
    CHECK(service.ensure_checked(requested, third_visit, Reason::attention) == Request::joined);
    CHECK(executor.starts == 3);
    CHECK(state(service, requested).checked_this_activation);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(service.ensure_checked(requested, third_visit, Reason::attention) == Request::already_checked);
    CHECK(executor.starts == 3);
}

TEST_CASE("Version discovery pending intents promote queues into reserved attention capacity", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor, {2, 1});
    const auto running = key(L"0 speculative*");
    const auto requested = key(L"1 explicit pending*");
    configure(service, {running, requested});
    activate(service, {running, requested});
    REQUIRE(service.active_count() == 1);
    REQUIRE(state(service, requested).activity == Activity::queued);
    CHECK(service.retain_pending(requested));
    CHECK(service.active_count() == 2);
    CHECK(state(service, requested).activity == Activity::running);
    service.deactivate();
    CHECK(state(service, requested).activity == Activity::running);
    CHECK(service.queued_count() == 0);
    executor.run(1);
    REQUIRE(service.collect_ready() == 1);
    CHECK(state(service, requested).snapshot);
    service.release_pending(requested);
    CHECK(service.active_count() == 1);
}

TEST_CASE("Version discovery releases hidden pending queues only after their last intent", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor, {1, 0});
    const auto running = key(L"0 held slot*");
    const auto requested = key(L"1 two pending actions*");
    configure(service, {running, requested});
    const auto visit = activate(service, {running, requested});
    REQUIRE(service.retain_pending(requested));
    REQUIRE(service.retain_pending(requested));
    service.deactivate();
    service.release_pending(requested);
    CHECK(state(service, requested).activity == Activity::queued);
    CHECK_FALSE(state(service, requested).last_attempt);
    service.release_pending(requested);
    CHECK(state(service, requested).activity == Activity::idle);
    CHECK(service.queued_count() == 0);
    CHECK_FALSE(state(service, requested).last_attempt);
    CHECK_FALSE(service.retain_pending(requested));
    CHECK_NOTHROW(service.release_pending(requested));
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(executor.starts == 1);
    CHECK(service.ensure_checked(requested, visit, Reason::attention) == Request::ignored);
    const auto next_visit = activate(service, {requested});
    CHECK(next_visit > visit);
    CHECK(executor.starts == 2);
}

TEST_CASE("Version discovery pending failures are terminal until a new explicit visit", "[version-discovery]") {
    const auto reject_start = GENERATE(false, true);
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor, {1, 0});
    const auto running = key(L"0 held slot*");
    const auto requested = key(L"1 failing pending*");
    script->replies[requested].push_back(unsuccessful(ScanStatus::failed));
    configure(service, {running, requested});
    activate(service, {running, requested});
    REQUIRE(service.retain_pending(requested));
    service.deactivate();
    executor.run();
    executor.reject_next = reject_start;
    REQUIRE(service.collect_ready() == 1);
    if (!reject_start) {
        executor.run();
        REQUIRE(service.collect_ready() == 1);
    }
    const auto terminal = state(service, requested);
    CHECK(terminal.activity == Activity::idle);
    CHECK_FALSE(terminal.snapshot);
    REQUIRE(terminal.last_attempt);
    CHECK(terminal.last_attempt->failure == (reject_start ? Failure::start : Failure::none));
    CHECK(terminal.last_attempt->observation.status == ScanStatus::failed);
    CHECK_FALSE(service.retain_pending(requested));
    CHECK(service.collect_ready() == 0);
    CHECK(executor.starts == 2);
    service.release_pending(requested);
    script->replies[requested].clear();
    activate(service, {requested});
    CHECK(executor.starts == 3);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(state(service, requested).snapshot);
}

TEST_CASE("Version discovery removing configured searches cancels pending pins and reservations", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor, {1, 0});
    const auto running = key(L"0 pinned worker*");
    const auto queued = key(L"1 pinned reservation*");
    const auto unknown = key(L"unknown*");
    configure(service, {running, queued});
    activate(service, {running, queued});
    REQUIRE(service.retain_pending(running));
    REQUIRE(service.retain_pending(queued));
    CHECK_FALSE(service.retain_pending(unknown));
    CHECK_NOTHROW(service.release_pending(unknown));
    configure(service, {});
    CHECK_FALSE(service.state(running));
    CHECK_FALSE(service.state(queued));
    CHECK_FALSE(service.retain_pending(running));
    CHECK(service.active_count() == 1);
    CHECK(service.queued_count() == 0);
    CHECK_NOTHROW(service.release_pending(running));
    CHECK_NOTHROW(service.release_pending(queued));
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(executor.starts == 1);
    CHECK(service.active_count() == 0);
    configure(service, {queued});
    CHECK_FALSE(state(service, queued).snapshot);
    CHECK_FALSE(state(service, queued).last_attempt);
    CHECK_FALSE(service.retain_pending(queued));
}

TEST_CASE("Version discovery retains configured snapshots independently of visible searches", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor);
    const auto first = key(L"first*");
    const auto second = key(L"second*");
    configure(service, {first, second});
    activate(service, {first});
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    const auto held = state(service, first).snapshot;
    configure(service, {second, first, first});
    activate(service, {second});
    CHECK(state(service, first).snapshot == held);
    CHECK_FALSE(state(service, first).checked_this_activation);
    configure(service, {second});
    CHECK_FALSE(service.state(first));
    REQUIRE(held);
    CHECK(held->size() == 1);
    configure(service, {first, second});
    CHECK_FALSE(state(service, first).snapshot);
    CHECK(state(service, first).catalog_revision == 0);
}

TEST_CASE("Version discovery keeps removed running searches as private tombstones", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor, {1, 0});
    const auto search = key(L"removed and readded*");
    configure(service, {search});
    activate(service, {search});
    configure(service, {});
    CHECK_FALSE(service.state(search));
    CHECK(service.active_count() == 1);
    CHECK(service.queued_count() == 0);
    configure(service, {search});
    const auto readded_visit = activate(service, {search});
    CHECK(executor.starts == 1);
    CHECK(state(service, search).activity == Activity::running);
    CHECK(service.ensure_checked(search, readded_visit, Reason::attention) == Request::joined);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(state(service, search).snapshot);
    CHECK(service.active_count() == 0);

    activate(service, {search});
    configure(service, {});
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK_FALSE(service.state(search));
    CHECK(service.active_count() == 0);
    configure(service, {search});
    CHECK_FALSE(state(service, search).snapshot);
}

TEST_CASE("Version discovery removes queued configuration before freeing capacity", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor, {1, 0});
    const auto running = key(L"0 running*");
    const auto removed = key(L"1 remove queued*");
    configure(service, {running, removed});
    activate(service, {running, removed});
    configure(service, {running});
    CHECK(service.queued_count() == 0);
    CHECK_FALSE(service.state(removed));
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(executor.starts == 1);
}

TEST_CASE("Version discovery can reserve every slot until an icon receives attention", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor, {1, 1});
    const auto search = key(L"attention only*");
    configure(service, {search});
    const auto visit = activate(service, {search});
    CHECK(executor.starts == 0);
    CHECK(service.active_count() == 0);
    CHECK(service.queued_count() == 1);
    CHECK(service.ensure_checked(search, visit, Reason::activation) == Request::queued);
    CHECK(service.ensure_checked(search, visit, Reason::attention) == Request::started);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(state(service, search).snapshot);
}

TEST_CASE("Version discovery owns configured keys and copied job inputs", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor);
    const auto original = key(L"owned input*");
    std::vector<Key> caller_keys{original};
    service.set_searches(caller_keys);
    service.begin_activation(caller_keys);
    caller_keys.front().locations.front() = L"C:\\caller changed this";
    caller_keys.clear();
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    CHECK(script->calls == std::vector<Key>{original});
    CHECK(state(service, original).snapshot);
}

TEST_CASE("Version discovery distinguishes provider kind roots and semantic revision", "[version-discovery]") {
    ManualExecutor executor;
    auto script = std::make_shared<Script>();
    Service service(scanner(script), executor);
    const auto generic = key(L"same location");
    const auto adobe = key(L"same location", Kind::adobe_catalog);
    auto revised = generic;
    ++revised.semantic_revision;
    auto multi_root = adobe;
    multi_root.locations.push_back(L"D:\\Additional Adobe root");
    script->replies[generic].push_back(complete(L"generic"));
    script->replies[adobe].push_back(unsuccessful(ScanStatus::failed));
    configure(service, {generic, adobe, revised, multi_root});
    activate(service, {generic, adobe, revised, multi_root});
    REQUIRE(executor.starts == 4);
    executor.run(2); // Adobe failure cannot suppress the generic result.
    REQUIRE(service.collect_ready() == 1);
    CHECK_FALSE(state(service, adobe).snapshot);
    executor.run();
    REQUIRE(service.collect_ready() == 1);
    REQUIRE(state(service, generic).snapshot);
    CHECK(state(service, generic).snapshot->front().name == L"generic");
    CHECK(state(service, revised).activity == Activity::running);
    CHECK(state(service, multi_root).activity == Activity::running);
}

TEST_CASE("Version discovery accepted work owns scanner dependencies after service destruction", "[version-discovery]") {
    ManualExecutor executor;
    std::weak_ptr<int> weak_dependency;
    std::weak_ptr<const std::vector<launcher::TargetVersion>> weak_catalog;
    std::shared_ptr<const std::vector<launcher::TargetVersion>> held_catalog;
    {
        auto dependency = std::make_shared<int>(7);
        weak_dependency = dependency;
        Service service([dependency](const Key&) { return complete(L"owned " + std::to_wstring(*dependency)); }, executor);
        const auto search = key(L"owned*");
        configure(service, {search});
        activate(service, {search});
        executor.run();
        REQUIRE(service.collect_ready() == 1);
        held_catalog = state(service, search).snapshot;
        weak_catalog = held_catalog;
        activate(service, {search});
    }
    CHECK_FALSE(weak_dependency.expired());
    CHECK_NOTHROW(executor.run());
    CHECK(weak_dependency.expired());
    REQUIRE(held_catalog);
    CHECK(held_catalog->front().name == L"owned 7");
    held_catalog.reset();
    CHECK(weak_catalog.expired());
}

TEST_CASE("Version discovery real workers publish independently while another scan is held", "[version-discovery]") {
    ThreadExecutor executor;
    Gate slow_gate;
    auto slow_entered = std::make_shared<std::promise<void>>();
    auto timed_out = std::make_shared<std::atomic_bool>(false);
    auto entered = slow_entered->get_future();
    const auto slow = key(L"0 slow network*");
    const auto fast = key(L"1 fast local*");
    Service service([slow, slow_entered, timed_out, wait = slow_gate.wait_handle()](const Key& search) {
        if (search == slow) {
            slow_entered->set_value();
            if (wait.wait_for(5s) != std::future_status::ready) { timed_out->store(true); }
        }
        return complete(search == slow ? L"slow result" : L"fast result");
    }, executor);
    configure(service, {slow, fast});
    activate(service, {slow, fast});
    REQUIRE(entered.wait_for(5s) == std::future_status::ready);
    REQUIRE(executor.finished(1));
    REQUIRE(service.collect_ready() == 1);
    CHECK(service.active_count() == 1);
    CHECK_FALSE(state(service, slow).snapshot);
    REQUIRE(state(service, fast).snapshot);
    CHECK(state(service, fast).snapshot->front().name == L"fast result");
    slow_gate.open();
    REQUIRE(executor.finished(0));
    REQUIRE(service.collect_ready() == 1);
    CHECK(state(service, slow).snapshot->front().name == L"slow result");
    CHECK_FALSE(timed_out->load());
}

TEST_CASE("Version discovery destruction does not wait for a blocked externally owned worker", "[version-discovery]") {
    ThreadExecutor executor;
    Gate gate;
    auto started = std::make_shared<std::promise<void>>();
    auto entered = started->get_future();
    auto dependency = std::make_shared<int>(42);
    auto timed_out = std::make_shared<std::atomic_bool>(false);
    std::weak_ptr<int> weak_dependency = dependency;
    auto service = std::make_unique<Service>([dependency, started, timed_out, wait = gate.wait_handle()](const Key&) {
        started->set_value();
        if (wait.wait_for(5s) != std::future_status::ready) { timed_out->store(true); }
        return complete(L"late " + std::to_wstring(*dependency));
    }, executor);
    dependency.reset();
    const auto search = key(L"late worker*");
    configure(*service, {search});
    activate(*service, {search});
    REQUIRE(entered.wait_for(5s) == std::future_status::ready);
    service.reset(); // Returning here proves destruction did not join the gate.
    CHECK_FALSE(weak_dependency.expired());
    gate.open();
    REQUIRE(executor.finished(0));
    CHECK(weak_dependency.expired());
    CHECK_FALSE(timed_out->load());
}
