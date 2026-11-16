#include "launcher/windows_pch.hpp"

#include "about_review_status.hpp"
#include "launcher_resource.h"
#include "updater_integration.hpp"

namespace launcher {
namespace {

constexpr UINT query_poll_interval = 100;
constexpr DWORD review_pending = 3;
constexpr wchar_t check_now_label[] = L"Check now";

} // namespace

AboutReviewStatus::~AboutReviewStatus()
{
    detach();
}

void AboutReviewStatus::attach(HWND dialog) noexcept
{
    const auto id = reinterpret_cast<UINT_PTR>(this);
    if (!SetWindowSubclass(dialog, message, id, reinterpret_cast<DWORD_PTR>(this))) { return; }
    dialog_ = dialog;
    refresh();
}

void AboutReviewStatus::refresh() noexcept
{
    if (query_) { return; }
    query_ = start_review_status_query(files_);
    if (query_ && SetTimer(dialog_, reinterpret_cast<UINT_PTR>(this), query_poll_interval, nullptr)) { return; }
    close_query();
    SetDlgItemTextW(dialog_, IDC_ABOUT_CHECK_NOW, check_now_label);
}

void AboutReviewStatus::poll() noexcept
{
    if (!query_) { return; }
    const auto wait = WaitForSingleObject(query_, 0);
    if (wait == WAIT_TIMEOUT) { return; }
    DWORD code = 0;
    const bool pending = wait == WAIT_OBJECT_0 && GetExitCodeProcess(query_, &code) && code == review_pending;
    close_query();
    SetDlgItemTextW(dialog_, IDC_ABOUT_CHECK_NOW, pending ? L"Review pending" : check_now_label);
}

void AboutReviewStatus::close_query() noexcept
{
    if (!query_) { return; }
    KillTimer(dialog_, reinterpret_cast<UINT_PTR>(this));
    CloseHandle(query_);
    query_ = nullptr;
}

void AboutReviewStatus::detach() noexcept
{
    if (!dialog_) { return; }
    close_query();
    RemoveWindowSubclass(dialog_, message, reinterpret_cast<UINT_PTR>(this));
    dialog_ = nullptr;
}

LRESULT CALLBACK AboutReviewStatus::message(HWND dialog, UINT message, WPARAM word, LPARAM parameter, UINT_PTR id, DWORD_PTR data) noexcept
{
    auto& status = *reinterpret_cast<AboutReviewStatus*>(data);
    if (message == WM_ACTIVATE && LOWORD(word) != WA_INACTIVE) { status.refresh(); }
    if (message == WM_TIMER && word == id) { status.poll(); return 0; }
    if (message == WM_NCDESTROY) { status.detach(); }
    return DefSubclassProc(dialog, message, word, parameter);
}

} // namespace launcher
