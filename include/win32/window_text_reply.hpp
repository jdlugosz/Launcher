#pragma once

#include <windows.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace win32 {

inline constexpr wchar_t restart_arguments_request[] = L"Dlugosz.RestartArguments.v1";
inline constexpr std::size_t window_text_reply_limit = 32767; // Includes the terminating NUL.

// Request: wParam is the reply HWND; lParam is a nonzero correlation cookie.
// Reply: WM_COPYDATA, wParam is the source HWND, dwData echoes the cookie,
// and lpData is a NUL-terminated UTF-16 string including its NUL in cbData.
// The request returns 1 after an accepted reply, -1 if replying failed, or the
// window's normal default result (usually zero) when it is not supported.
class WindowTextReply final {
public:
    WindowTextReply(std::wstring_view request_name, std::wstring response);

    // Call from the responding window's procedure, supplying that window as
    // source. The text is immutable and remains owned throughout the send.
    std::optional<LRESULT> handle(HWND source, UINT message, WPARAM word, LPARAM data) const noexcept;
    std::wstring_view response() const noexcept { return response_; }

private:
    UINT request_ = 0;
    std::wstring response_;
};

// Uses a temporary receiver on the calling thread. The outbound request must
// permit nested sent messages: SMTO_BLOCK would prevent the COPYDATA reply.
// Windows ignores send timeouts for windows sharing the caller's input queue.
// Unsupported, malformed and timed-out replies throw distinct system errors.
std::wstring query_window_text(HWND target, std::wstring_view request_name, DWORD timeout_ms = 5000);

// Quotes one argument using the Windows command-line backslash/quote rules.
// Always quotes, including empty arguments; embedded NUL is invalid.
std::wstring quote_command_argument(std::wstring_view argument);

} // namespace win32
