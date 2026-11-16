#include "win32/window_text_reply.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace win32 {
namespace {

constexpr DWORD reply_timeout_ms = 1000;
constexpr wchar_t receiver_class[] = L"Dlugosz.WindowTextReply.Receiver.v1";

[[noreturn]] void fail(DWORD error, const char* message) {
    throw std::system_error(static_cast<int>(error), std::system_category(), message);
}

UINT register_request(std::wstring_view name) {
    if (name.empty() || name.find(L'\0') != name.npos) {
        throw std::invalid_argument("A window text request needs a nonempty name without NUL.");
    }
    const std::wstring terminated(name);
    const UINT message = RegisterWindowMessageW(terminated.c_str());
    if (!message) { fail(GetLastError(), "Cannot register the window text request"); }
    return message;
}

enum class ReplyState { waiting, received, malformed, allocation_failed };

struct Receiver final {
    HWND target = nullptr;
    DWORD process = 0;
    ULONG_PTR cookie = 0;
    ReplyState state = ReplyState::waiting;
    std::wstring response;

    LRESULT receive(WPARAM word, LPARAM data) noexcept {
        DWORD sender_process = 0;
        if (state != ReplyState::waiting || reinterpret_cast<HWND>(word) != target ||
                !GetWindowThreadProcessId(target, &sender_process) || sender_process != process || !data) {
            return FALSE;
        }
        const auto& packet = *reinterpret_cast<const COPYDATASTRUCT*>(data);
        if (packet.dwData != cookie) { return FALSE; }
        if (!packet.lpData || packet.cbData < sizeof(wchar_t) ||
                packet.cbData > window_text_reply_limit * sizeof(wchar_t) ||
                packet.cbData % sizeof(wchar_t) != 0 ||
                reinterpret_cast<std::uintptr_t>(packet.lpData) % alignof(wchar_t) != 0) {
            state = ReplyState::malformed;
            return FALSE;
        }
        try {
            // Windows lends bytes for the duration of this message. Copy into
            // live wchar_t objects before interpreting or retaining the text.
            response.resize(packet.cbData / sizeof(wchar_t));
            std::memcpy(response.data(), packet.lpData, packet.cbData);
            if (response.back() != L'\0' || response.find(L'\0') != response.size() - 1) {
                response.clear();
                state = ReplyState::malformed;
                return FALSE;
            }
            response.pop_back();
            state = ReplyState::received;
            return TRUE;
        } catch (...) {
            state = ReplyState::allocation_failed;
            return FALSE;
        }
    }
};

LRESULT CALLBACK receiver_proc(HWND window, UINT message, WPARAM word, LPARAM data) noexcept {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(data);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* receiver = reinterpret_cast<Receiver*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_COPYDATA && receiver) { return receiver->receive(word, data); }
    if (message == WM_NCDESTROY) { SetWindowLongPtrW(window, GWLP_USERDATA, 0); }
    return DefWindowProcW(window, message, word, data);
}

ATOM receiver_atom() {
    // Initialization is synchronized by the language and the class lives for
    // the process lifetime. Each query owns its receiver HWND independently.
    static const ATOM atom = [] {
        WNDCLASSW type{};
        type.lpfnWndProc = receiver_proc;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = receiver_class;
        const ATOM registered = RegisterClassW(&type);
        if (!registered) { fail(GetLastError(), "Cannot register the window text receiver"); }
        return registered;
    }();
    return atom;
}

struct ReceiverWindow final {
    HWND value = nullptr;
    ~ReceiverWindow() { if (value) { DestroyWindow(value); } }
};

ULONG_PTR next_cookie() noexcept {
    static std::atomic<ULONG_PTR> sequence{1};
    auto cookie = sequence.fetch_add(1, std::memory_order_relaxed);
    if (!cookie) { cookie = sequence.fetch_add(1, std::memory_order_relaxed); }
    return cookie;
}

} // namespace

WindowTextReply::WindowTextReply(std::wstring_view request_name, std::wstring response) : response_(std::move(response)) {
    if (response_.size() >= window_text_reply_limit || response_.find(L'\0') != response_.npos) {
        throw std::invalid_argument("A window text reply must fit in 32767 UTF-16 characters including its terminating NUL.");
    }
    request_ = register_request(request_name);
}

std::optional<LRESULT> WindowTextReply::handle(HWND source, UINT message, WPARAM word, LPARAM data) const noexcept {
    if (message != request_) { return std::nullopt; }
    const auto receiver = reinterpret_cast<HWND>(word);
    if (!data || !IsWindow(source) || !IsWindow(receiver)) { return LRESULT{-1}; }
    COPYDATASTRUCT packet{static_cast<ULONG_PTR>(data),
        static_cast<DWORD>((response_.size() + 1) * sizeof(wchar_t)), const_cast<wchar_t*>(response_.c_str())};
    DWORD_PTR accepted = 0;
    const auto sent = SendMessageTimeoutW(receiver, WM_COPYDATA, reinterpret_cast<WPARAM>(source),
        reinterpret_cast<LPARAM>(&packet), SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, reply_timeout_ms, &accepted);
    return sent && accepted ? LRESULT{1} : LRESULT{-1};
}

std::wstring query_window_text(HWND target, std::wstring_view request_name, DWORD timeout_ms) {
    if (!timeout_ms) { throw std::invalid_argument("A window text query needs a nonzero timeout."); }
    const UINT message = register_request(request_name);
    Receiver receiver;
    receiver.target = target;
    if (!GetWindowThreadProcessId(target, &receiver.process)) {
        fail(ERROR_INVALID_WINDOW_HANDLE, "Cannot query a window that no longer exists");
    }
    receiver.cookie = next_cookie();
    const ATOM atom = receiver_atom();
    ReceiverWindow window{CreateWindowExW(0, MAKEINTATOM(atom), nullptr, 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), &receiver)};
    if (!window.value) { fail(GetLastError(), "Cannot create the window text receiver"); }
    DWORD_PTR result = 0;
    SetLastError(ERROR_SUCCESS);
    const auto sent = SendMessageTimeoutW(target, message, reinterpret_cast<WPARAM>(window.value),
        static_cast<LPARAM>(receiver.cookie), SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, timeout_ms, &result);
    const auto error = sent ? ERROR_SUCCESS : GetLastError();
    DWORD current_process = 0;
    if (!GetWindowThreadProcessId(target, &current_process) || current_process != receiver.process) {
        fail(ERROR_INVALID_WINDOW_HANDLE, "The responding window disappeared during the request");
    }
    if (!sent) { fail(error ? error : ERROR_TIMEOUT, "The window text request failed or timed out"); }
    if (receiver.state == ReplyState::allocation_failed) { throw std::bad_alloc(); }
    if (receiver.state == ReplyState::malformed) { fail(ERROR_INVALID_DATA, "The window returned malformed text"); }
    if (receiver.state == ReplyState::received) { return std::move(receiver.response); }
    if (!result) { fail(ERROR_NOT_SUPPORTED, "The window does not support this text request"); }
    fail(ERROR_INVALID_DATA, "The window acknowledged the request without returning matching text");
}

std::wstring quote_command_argument(std::wstring_view argument) {
    if (argument.find(L'\0') != argument.npos) { throw std::invalid_argument("A command-line argument cannot contain NUL."); }
    std::wstring quoted = L"\"";
    quoted.reserve(argument.size() + 2);
    std::size_t slashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        quoted.append(character == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        quoted.push_back(character);
        slashes = 0;
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

} // namespace win32
