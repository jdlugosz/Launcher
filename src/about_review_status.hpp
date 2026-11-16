#pragma once

#include <windows.h>

namespace launcher {

class FileLookup;

// Lives for the duration of the modal About dialog.
class AboutReviewStatus final {
public:
    explicit AboutReviewStatus(const FileLookup& files) noexcept : files_(files) {}
    ~AboutReviewStatus();

    AboutReviewStatus(const AboutReviewStatus&) = delete;
    AboutReviewStatus& operator=(const AboutReviewStatus&) = delete;

    void attach(HWND dialog) noexcept;

private:
    static LRESULT CALLBACK message(HWND dialog, UINT message, WPARAM word, LPARAM parameter, UINT_PTR id, DWORD_PTR data) noexcept;
    void refresh() noexcept;
    void poll() noexcept;
    void close_query() noexcept;
    void detach() noexcept;

    const FileLookup& files_;
    HWND dialog_ = nullptr;
    HANDLE query_ = nullptr;
};

} // namespace launcher
