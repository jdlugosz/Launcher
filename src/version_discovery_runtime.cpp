#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/version_discovery_runtime.hpp"

#include <cstdlib>
#include <exception>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace launcher {
namespace {

[[noreturn]] void end_process(unsigned int exit_code) noexcept {
    // No allocation, logging dependency or DLL/CRT teardown on this path.
    ::TerminateProcess(::GetCurrentProcess(), exit_code);
    std::abort(); // A failed self-termination must not resume normal teardown.
}

struct CompletionSignal final {
    HANDLE handle = nullptr;

    CompletionSignal() {
        handle = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!handle) { throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "Create version discovery completion event"); }
    }

    ~CompletionSignal() { ::CloseHandle(handle); }

    void publish() const noexcept {
        if (!::SetEvent(handle)) {
            ::OutputDebugStringW(L"Launcher: version discovery completion event failed.\n");
            end_process(0xE0000001U);
        }
    }
};

struct ThreadFilesystemErrorMode final {
    DWORD previous = GetThreadErrorMode();
    bool changed = SetThreadErrorMode(previous | SEM_FAILCRITICALERRORS, nullptr) != FALSE;
    ~ThreadFilesystemErrorMode() { if (changed) { SetThreadErrorMode(previous, nullptr); } }
};

} // namespace

struct NativeVersionDiscoveryExecutor::Impl final {
    std::shared_ptr<CompletionSignal> signal = std::make_shared<CompletionSignal>();
    std::vector<std::thread> workers;
    bool accepting = true;
};

NativeVersionDiscoveryExecutor::NativeVersionDiscoveryExecutor() : impl_(std::make_unique<Impl>()) {}

NativeVersionDiscoveryExecutor::~NativeVersionDiscoveryExecutor() {
    try {
        if (shutdown_ready()) { return; }
    } catch (...) {}
    // Returning through C++ teardown with a live C++ worker is not safe. The
    // explicit process boundary must run before destruction of this owner.
    std::terminate();
}

void NativeVersionDiscoveryExecutor::start(Work work) {
    if (!impl_->accepting) { throw std::logic_error("Version discovery execution is stopping."); }
    if (!work) { throw std::invalid_argument("Version discovery requires owned work."); }
    reap_exited();
    // Complete every potentially throwing registry allocation before starting
    // a thread. Once accepted, start must not report failure to the service.
    impl_->workers.emplace_back();
    try {
        impl_->workers.back() = std::thread([work = std::move(work), signal = impl_->signal]() mutable noexcept {
            // A removed drive must return its error to this worker, without a
            // system critical-error modal. Keep the process/UI policy unchanged.
            const ThreadFilesystemErrorMode error_mode;
            try { work(); } catch (...) {
                // Service work reports its own exceptions as observations.
                // This boundary also contains other application-owned work.
            }
            // Release arbitrary Work captures while the runtime is intact.
            // Notification follows their destruction, but native exit remains
            // a separate observation through the retained thread handle.
            work = {};
            signal->publish();
        });
    } catch (...) {
        impl_->workers.pop_back();
        throw;
    }
}

void NativeVersionDiscoveryExecutor::stop_admission() noexcept { impl_->accepting = false; }

HANDLE NativeVersionDiscoveryExecutor::completion_event() const noexcept { return impl_->signal->handle; }

void NativeVersionDiscoveryExecutor::reset_completion() {
    if (!::ResetEvent(impl_->signal->handle)) {
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "Reset version discovery completion event");
    }
}

std::size_t NativeVersionDiscoveryExecutor::reap_exited() {
    std::size_t count = 0;
    for (auto worker = impl_->workers.begin(); worker != impl_->workers.end();) {
        const auto status = ::WaitForSingleObject(worker->native_handle(), 0);
        if (status == WAIT_TIMEOUT) { ++worker; continue; }
        if (status != WAIT_OBJECT_0) {
            const auto error = status == WAIT_FAILED ? ::GetLastError() : ERROR_GEN_FAILURE;
            throw std::system_error(static_cast<int>(error), std::system_category(), "Observe version discovery worker exit");
        }
        worker->join();
        worker = impl_->workers.erase(worker);
        ++count;
    }
    return count;
}

std::size_t NativeVersionDiscoveryExecutor::worker_count() const noexcept { return impl_->workers.size(); }

bool NativeVersionDiscoveryExecutor::shutdown_ready() {
    reap_exited();
    return impl_->workers.empty();
}

void NativeVersionDiscoveryExecutor::finish_process_if_busy(unsigned int exit_code) noexcept {
    stop_admission();
    try {
        if (shutdown_ready()) { return; }
    } catch (...) {
        ::OutputDebugStringW(L"Launcher: version discovery worker exit observation failed.\n");
        end_process(0xE0000002U);
    }
    end_process(exit_code);
}

} // namespace launcher
