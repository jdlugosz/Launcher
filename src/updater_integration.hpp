#pragma once

#include <windows.h>
#include <memory>
#include <string>

namespace launcher {

class FileLookup;

// Starts the manual check or its fallback. The bootstrap completes off the UI thread.
bool check_for_updates(HWND owner, const FileLookup& files) noexcept;
// The caller owns the returned process handle. A null handle means unavailable.
HANDLE start_review_status_query(const FileLookup& files) noexcept;
std::wstring last_automatic_check_text();

// Keeps the normal Launcher timer and follows automatic failure reports from the
// Updater, including checks resumed by a detached self-update worker.
class AutomaticUpdateSchedule final {
public:
    AutomaticUpdateSchedule() noexcept;
    ~AutomaticUpdateSchedule();

    void check(HWND window, UINT_PTR timer, UINT retry_changed_message, const FileLookup& files) noexcept;
    void retry_changed(HWND window, UINT_PTR timer) noexcept;
    void stop() noexcept;

private:
    struct Watch;
    bool refresh_retry(ULONGLONG now) noexcept;
    void schedule_next(HWND window, UINT_PTR timer, ULONGLONG now) const noexcept;

    std::unique_ptr<Watch> watch_;
    const wchar_t* review_reminder_value_ = nullptr;
    ULONGLONG feed_wake_ = 0;
    ULONGLONG review_retry_after_ = 0;
    ULONGLONG observed_retry_ = 0;
    ULONGLONG retry_deadline_ = 0;
    bool retry_initialized_ = false;
};

} // namespace launcher
