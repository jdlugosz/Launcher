#pragma once

#include "launcher/version_discovery.hpp"

#include <cstddef>
#include <memory>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace launcher {

// Application-scope execution and notification boundary. Workers retain only
// their owned Work and the shared completion event. The executor never calls
// back into a service or window; callers must give Work independently valid
// captures. All public methods run on the owner thread.
class NativeVersionDiscoveryExecutor final : public VersionDiscoveryExecutor {
public:
    NativeVersionDiscoveryExecutor();
    // The application must finish_process_if_busy after its explicit cleanup
    // and before destroying this owner. Destruction never joins live threads.
    ~NativeVersionDiscoveryExecutor() override;
    NativeVersionDiscoveryExecutor(const NativeVersionDiscoveryExecutor&) = delete;
    NativeVersionDiscoveryExecutor& operator=(const NativeVersionDiscoveryExecutor&) = delete;
    NativeVersionDiscoveryExecutor(NativeVersionDiscoveryExecutor&&) = delete;
    NativeVersionDiscoveryExecutor& operator=(NativeVersionDiscoveryExecutor&&) = delete;

    void start(Work work) override;
    void stop_admission() noexcept;

    // Borrowed handle: keep this owner alive throughout every UI wait. Reset
    // BEFORE inspecting published jobs, never after the collection pass.
    HANDLE completion_event() const noexcept;
    void reset_completion();

    // A published job need not have exited its native thread yet. Reap only
    // handles already observed signaled; no method waits for a live worker.
    std::size_t reap_exited();
    std::size_t worker_count() const noexcept;
    bool shutdown_ready();

    // Call after settings, restart bookkeeping, UI/COM owners and logging have
    // received their explicit cleanup. With live workers this ends the entire
    // process before CRT/static teardown; it does not terminate one thread or
    // promise bounded kernel/provider reaping. Returns only when none remain;
    // an exit-observation failure uses a distinct emergency exit code. Safe to
    // call from the application's final noexcept scope guard.
    void finish_process_if_busy(unsigned int exit_code) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace launcher
