#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/version_discovery_runtime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Runtime = launcher::NativeVersionDiscoveryExecutor;
using Service = launcher::VersionDiscoveryService;
using namespace std::chrono_literals;

constexpr DWORD wait_limit = 10'000;
constexpr auto child_mode_name = L"LAUNCHER_TEST_VERSION_RUNTIME_CHILD";
constexpr auto child_marker_name = L"LAUNCHER_TEST_VERSION_RUNTIME_MARKER";

std::wstring environment_value(const wchar_t* name) {
    const auto size = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) { return {}; }
    std::wstring value(size, L'\0');
    value.resize(::GetEnvironmentVariableW(name, value.data(), size));
    return value;
}

class EnvironmentValue final {
public:
    EnvironmentValue(const wchar_t* name, const std::wstring& value) : name_(name), previous_(environment_value(name)) {
        if (!::SetEnvironmentVariableW(name_, value.c_str())) { throw std::runtime_error("Set runtime child environment"); }
    }
    ~EnvironmentValue() { ::SetEnvironmentVariableW(name_, previous_.empty() ? nullptr : previous_.c_str()); }
private:
    const wchar_t* name_;
    std::wstring previous_;
};

class Gate final {
public:
    Gate() : future_(promise_.get_future().share()) {}
    void wait() const { future_.wait(); }
    void release() noexcept {
        if (!released_.exchange(true)) { promise_.set_value(); }
    }
private:
    std::promise<void> promise_;
    std::shared_future<void> future_;
    std::atomic<bool> released_ = false;
};

HANDLE duplicate_thread_exit() {
    HANDLE exit = nullptr;
    if (!::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &exit, SYNCHRONIZE, FALSE, 0)) {
        throw std::runtime_error("Duplicate runtime test thread handle");
    }
    return exit;
}

// Test waits use independently duplicated real thread handles. Completion
// publication and future readiness are deliberately not treated as thread exit.
class RuntimeFixture final {
public:
    Runtime runtime;

    ~RuntimeFixture() {
        for (const auto& gate : gates_) { gate->release(); }
        for (const auto handle : exits_) {
            if (::WaitForSingleObject(handle, wait_limit) != WAIT_OBJECT_0) { std::terminate(); }
            ::CloseHandle(handle);
        }
        runtime.reap_exited();
    }

    std::shared_ptr<Gate> gate() {
        auto value = std::make_shared<Gate>();
        gates_.push_back(value);
        return value;
    }

    HANDLE start(launcher::VersionDiscoveryExecutor::Work work) {
        auto started = std::make_shared<std::promise<HANDLE>>();
        auto future = started->get_future();
        runtime.start([started, work = std::move(work)] {
            try { started->set_value(duplicate_thread_exit()); }
            catch (...) { started->set_exception(std::current_exception()); return; }
            work();
        });
        if (future.wait_for(10s) != std::future_status::ready) { std::terminate(); }
        const auto exit = future.get();
        exits_.push_back(exit);
        return exit;
    }

    void remember_exit(HANDLE handle) { exits_.push_back(handle); }
    void await_exit(HANDLE handle) { REQUIRE(::WaitForSingleObject(handle, wait_limit) == WAIT_OBJECT_0); }

private:
    std::vector<std::shared_ptr<Gate>> gates_;
    std::vector<HANDLE> exits_;
};

launcher::VersionSearchKey key(const wchar_t* name) { return {launcher::VersionSearchKind::target_pattern, {name}, 1}; }

std::filesystem::path temporary_marker() {
    wchar_t directory[MAX_PATH]{};
    if (!::GetTempPathW(MAX_PATH, directory)) { throw std::runtime_error("Find runtime test temporary directory"); }
    wchar_t name[MAX_PATH]{};
    if (!::GetTempFileNameW(directory, L"lvr", 0, name)) { throw std::runtime_error("Create runtime test marker"); }
    return name;
}

class MarkerFile final {
public:
    std::filesystem::path path = temporary_marker();
    ~MarkerFile() { std::error_code ignored; std::filesystem::remove(path, ignored); }
};

class ProcessHandles final {
public:
    PROCESS_INFORMATION process{};
    ~ProcessHandles() {
        if (process.hProcess) {
            if (::WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
                ::TerminateProcess(process.hProcess, 99);
                ::WaitForSingleObject(process.hProcess, wait_limit);
            }
            ::CloseHandle(process.hProcess);
        }
        if (process.hThread) { ::CloseHandle(process.hThread); }
    }
};

DWORD run_exit_child(const std::wstring& mode, const std::filesystem::path& marker) {
    std::wstring executable(32'768, L'\0');
    executable.resize(::GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())));
    std::wstring command = L"\"" + executable + L"\" \"Version discovery runtime child exit fixture\" --reporter compact";
    EnvironmentValue child_mode(child_mode_name, mode);
    EnvironmentValue child_marker(child_marker_name, marker.wstring());
    STARTUPINFOW startup{sizeof(startup)};
    ProcessHandles handles;
    REQUIRE(::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &handles.process));
    REQUIRE(::WaitForSingleObject(handles.process.hProcess, wait_limit) == WAIT_OBJECT_0);
    DWORD code = 0;
    REQUIRE(::GetExitCodeProcess(handles.process.hProcess, &code));
    return code;
}

std::vector<std::string> marker_lines(const std::filesystem::path& marker) {
    std::ifstream input(marker);
    std::vector<std::string> result;
    for (std::string line; std::getline(input, line);) { result.push_back(std::move(line)); }
    return result;
}

void record_crt_cleanup() {
    std::ofstream output(std::filesystem::path(environment_value(child_marker_name)), std::ios::app);
    output << "CRT cleanup\n";
}

} // namespace

TEST_CASE("Version runtime handles drive errors without changing the UI error mode", "[version-runtime]") {
    RuntimeFixture fixture;
    const auto owner_mode = GetThreadErrorMode();
    const auto process_mode = GetErrorMode();
    std::promise<DWORD> observed;
    auto result = observed.get_future();
    const auto exit = fixture.start([&] { observed.set_value(GetThreadErrorMode()); });
    REQUIRE(result.wait_for(5s) == std::future_status::ready);
    CHECK((result.get() & SEM_FAILCRITICALERRORS) != 0);
    CHECK(GetThreadErrorMode() == owner_mode);
    CHECK(GetErrorMode() == process_mode);
    REQUIRE(WaitForSingleObject(fixture.runtime.completion_event(), wait_limit) == WAIT_OBJECT_0);
    fixture.await_exit(exit);
    CHECK(fixture.runtime.shutdown_ready());
}

TEST_CASE("Version runtime signals completed work and reaps only real thread exit", "[version-runtime]") {
    RuntimeFixture fixture;
    const auto gate = fixture.gate();
    std::atomic<bool> ran = false;
    const auto exit = fixture.start([gate, &ran] { gate->wait(); ran = true; });
    CHECK(fixture.runtime.worker_count() == 1);
    CHECK(fixture.runtime.reap_exited() == 0);
    CHECK_FALSE(fixture.runtime.shutdown_ready());
    CHECK(::WaitForSingleObject(fixture.runtime.completion_event(), 0) == WAIT_TIMEOUT);
    gate->release();
    REQUIRE(::WaitForSingleObject(fixture.runtime.completion_event(), wait_limit) == WAIT_OBJECT_0);
    CHECK(ran.load());
    fixture.await_exit(exit);
    CHECK(fixture.runtime.reap_exited() == 1);
    CHECK(fixture.runtime.shutdown_ready());
    fixture.runtime.reset_completion();
    CHECK(::WaitForSingleObject(fixture.runtime.completion_event(), 0) == WAIT_TIMEOUT);
}

TEST_CASE("Version runtime resets before collection without losing a later completion", "[version-runtime]") {
    RuntimeFixture fixture;
    const auto first = fixture.gate();
    const auto second = fixture.gate();
    const auto first_exit = fixture.start([first] { first->wait(); });
    const auto second_exit = fixture.start([second] { second->wait(); });
    first->release();
    REQUIRE(::WaitForSingleObject(fixture.runtime.completion_event(), wait_limit) == WAIT_OBJECT_0);
    fixture.runtime.reset_completion();
    fixture.await_exit(first_exit);
    CHECK(fixture.runtime.reap_exited() == 1);
    second->release();
    REQUIRE(::WaitForSingleObject(fixture.runtime.completion_event(), wait_limit) == WAIT_OBJECT_0);
    fixture.await_exit(second_exit);
    CHECK(fixture.runtime.reap_exited() == 1);
}

TEST_CASE("Version runtime contains work exceptions and still notifies completion", "[version-runtime]") {
    RuntimeFixture fixture;
    const auto exit = fixture.start([] { throw std::runtime_error("Injected work exception"); });
    REQUIRE(::WaitForSingleObject(fixture.runtime.completion_event(), wait_limit) == WAIT_OBJECT_0);
    fixture.await_exit(exit);
    CHECK(fixture.runtime.shutdown_ready());
}

TEST_CASE("Version runtime releases owned captures before completion notification", "[version-runtime]") {
    RuntimeFixture fixture;
    auto owned = std::make_shared<int>(42);
    const std::weak_ptr<int> weak = owned;
    const auto gate = fixture.gate();
    const auto exit = fixture.start([owned = std::move(owned), gate] { gate->wait(); });
    CHECK_FALSE(weak.expired());
    gate->release();
    REQUIRE(::WaitForSingleObject(fixture.runtime.completion_event(), wait_limit) == WAIT_OBJECT_0);
    CHECK(weak.expired());
    fixture.await_exit(exit);
}

TEST_CASE("Version runtime stops admission without waiting for blocked work", "[version-runtime]") {
    RuntimeFixture fixture;
    const auto gate = fixture.gate();
    const auto exit = fixture.start([gate] { gate->wait(); });
    fixture.runtime.stop_admission();
    CHECK_THROWS_AS(fixture.runtime.start([] {}), std::logic_error);
    CHECK_FALSE(fixture.runtime.shutdown_ready());
    CHECK(fixture.runtime.worker_count() == 1);
    gate->release();
    fixture.await_exit(exit);
    CHECK(fixture.runtime.shutdown_ready());
    fixture.runtime.finish_process_if_busy(57); // Empty runtime returns normally.
}

TEST_CASE("Version runtime rejects empty work before accepting a thread", "[version-runtime]") {
    RuntimeFixture fixture;
    CHECK_THROWS_AS(fixture.runtime.start({}), std::invalid_argument);
    CHECK(fixture.runtime.worker_count() == 0);
    CHECK(::WaitForSingleObject(fixture.runtime.completion_event(), 0) == WAIT_TIMEOUT);
}

TEST_CASE("Version runtime independently wakes the message wait while another search is held", "[version-runtime]") {
    RuntimeFixture fixture;
    const auto gate = fixture.gate();
    const auto held_exit = fixture.start([gate] { gate->wait(); });
    const auto quick_exit = fixture.start([] {});
    const HANDLE event = fixture.runtime.completion_event();
    REQUIRE(::WaitForSingleObject(event, wait_limit) == WAIT_OBJECT_0);
    REQUIRE(::MsgWaitForMultipleObjectsEx(1, &event, wait_limit, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0);
    fixture.await_exit(quick_exit);
    CHECK(fixture.runtime.reap_exited() == 1);
    CHECK_FALSE(fixture.runtime.shutdown_ready());
    gate->release();
    fixture.await_exit(held_exit);
}

TEST_CASE("Version service can disappear while runtime retains owned blocked work", "[version-runtime]") {
    RuntimeFixture fixture;
    const auto gate = fixture.gate();
    auto called = std::make_shared<std::promise<HANDLE>>();
    auto entered = called->get_future();
    auto owner = std::make_shared<int>(17);
    const std::weak_ptr<int> weak = owner;
    auto service = std::make_unique<Service>([gate, called, owner = std::move(owner)](const launcher::VersionSearchKey&) {
        called->set_value(duplicate_thread_exit());
        gate->wait();
        return launcher::TargetVersionScanResult{};
    }, fixture.runtime);
    const std::vector keys{key(L"owned search")};
    service->set_searches(keys);
    service->begin_activation(keys);
    REQUIRE(entered.wait_for(10s) == std::future_status::ready);
    const auto exit = entered.get();
    fixture.remember_exit(exit);
    service.reset();
    CHECK_FALSE(weak.expired());
    gate->release();
    REQUIRE(::WaitForSingleObject(fixture.runtime.completion_event(), wait_limit) == WAIT_OBJECT_0);
    CHECK(weak.expired());
    fixture.await_exit(exit);
    CHECK(fixture.runtime.shutdown_ready());
}

TEST_CASE("Version runtime process boundary preserves explicit cleanup and skips CRT for blocked workers", "[version-runtime]") {
    MarkerFile marker;
    CHECK(run_exit_child(L"blocked", marker.path) == 57);
    CHECK(marker_lines(marker.path) == std::vector<std::string>{"explicit cleanup"});
}

TEST_CASE("Version runtime normal process exit performs CRT cleanup after completed workers", "[version-runtime]") {
    MarkerFile marker;
    CHECK(run_exit_child(L"normal", marker.path) == 0);
    CHECK(marker_lines(marker.path) == std::vector<std::string>{"explicit cleanup", "CRT cleanup"});
}

TEST_CASE("Version runtime failed completion signaling takes the explicit emergency process boundary", "[version-runtime]") {
    MarkerFile marker;
    CHECK(run_exit_child(L"signal-failure", marker.path) == 0xE0000001U);
    CHECK(marker_lines(marker.path) == std::vector<std::string>{"before injected signal failure"});
}

TEST_CASE("Version discovery runtime child exit fixture", "[.][version-runtime-child]") {
    const auto mode = environment_value(child_mode_name);
    REQUIRE((mode == L"normal" || mode == L"blocked" || mode == L"signal-failure"));
    REQUIRE(std::atexit(record_crt_cleanup) == 0);
    if (mode == L"normal") {
        RuntimeFixture fixture;
        fixture.await_exit(fixture.start([] {}));
        {
            std::ofstream marker(std::filesystem::path(environment_value(child_marker_name)), std::ios::app);
            marker << "explicit cleanup\n";
        }
        fixture.runtime.finish_process_if_busy(57);
        return;
    }
    if (mode == L"signal-failure") {
        Runtime runtime;
        const auto gate = std::make_shared<Gate>();
        const auto started = std::make_shared<std::promise<void>>();
        auto entered = started->get_future();
        runtime.start([gate, started] { started->set_value(); gate->wait(); });
        REQUIRE(entered.wait_for(10s) == std::future_status::ready);
        {
            std::ofstream marker(std::filesystem::path(environment_value(child_marker_name)), std::ios::app);
            marker << "before injected signal failure\n";
        }
        // This child deliberately violates the borrowed-handle contract to
        // inject the infrastructure failure. Production keeps shared ownership
        // until all waiters and publishers have finished.
        REQUIRE(::CloseHandle(runtime.completion_event()));
        gate->release();
        ::Sleep(INFINITE);
        FAIL("Failed completion signaling did not terminate");
    }
    Runtime runtime;
    const auto started = std::make_shared<std::promise<void>>();
    auto entered = started->get_future();
    runtime.start([started] {
        started->set_value();
        ::Sleep(INFINITE);
    });
    REQUIRE(entered.wait_for(10s) == std::future_status::ready);
    {
        std::ofstream marker(std::filesystem::path(environment_value(child_marker_name)), std::ios::app);
        marker << "explicit cleanup\n";
    }
    runtime.finish_process_if_busy(57);
    FAIL("Blocked runtime exit boundary returned");
}
