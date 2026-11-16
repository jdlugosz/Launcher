#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "win32/window_text_reply.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct Probe final {
    UINT request = 0;
    std::wstring mode;
    std::optional<win32::WindowTextReply> reply;
};

LRESULT packet(HWND source, WPARAM receiver, ULONG_PTR cookie, const void* text, DWORD bytes) noexcept {
    COPYDATASTRUCT data{cookie, bytes, const_cast<void*>(text)};
    DWORD_PTR result = 0;
    SendMessageTimeoutW(reinterpret_cast<HWND>(receiver), WM_COPYDATA, reinterpret_cast<WPARAM>(source),
        reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 1000, &result);
    return 1; // Acknowledged, even when our deliberately bad reply was rejected.
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM word, LPARAM data) noexcept {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(data);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* probe = reinterpret_cast<Probe*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (probe && message == probe->request) {
        if (probe->mode == L"destroy") { DestroyWindow(window); return 1; }
        if (probe->mode == L"unsupported") { return 0; }
        if (probe->mode == L"no-reply") { return 1; }
        const auto cookie = static_cast<ULONG_PTR>(data);
        constexpr wchar_t valid[] = L"reply";
        if (probe->mode == L"wrong-source") {
            return packet(GetDesktopWindow(), word, cookie, valid, sizeof(valid));
        }
        if (probe->mode == L"wrong-cookie") {
            return packet(window, word, cookie + 1, valid, sizeof(valid));
        }
        if (probe->mode == L"malformed") {
            constexpr std::array<std::byte, 3> odd{};
            return packet(window, word, cookie, odd.data(), static_cast<DWORD>(odd.size()));
        }
        if (probe->mode == L"no-nul") {
            return packet(window, word, cookie, valid, sizeof(valid) - sizeof(wchar_t));
        }
        if (probe->mode == L"embedded-nul") {
            constexpr std::array text{L'x', L'\0', L'y', L'\0'};
            return packet(window, word, cookie, text.data(), sizeof(text));
        }
        if (probe->mode == L"oversized") {
            // Static storage is allocated before dispatch so no exception can
            // escape the native procedure, even for this malformed response.
            static constexpr std::array<wchar_t, win32::window_text_reply_limit + 1> text{};
            return packet(window, word, cookie, text.data(), sizeof(text));
        }
        if (probe->mode == L"delay") { Sleep(1000); }
        if (probe->reply) {
            if (const auto result = probe->reply->handle(window, message, word, data)) { return *result; }
        }
    }
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, word, data);
}

int run(int count, wchar_t** arguments) {
    if (count != 4 && count != 5) { return 2; }
    Probe probe;
    probe.mode = arguments[2];
    probe.request = RegisterWindowMessageW(arguments[1]);
    if (!probe.request) { return 3; }
    std::wstring response = probe.mode == L"max" ? std::wstring(win32::window_text_reply_limit - 1, L'x') :
        probe.mode == L"empty" ? std::wstring{} : std::wstring(arguments[3]);
    probe.reply.emplace(arguments[1], std::move(response));
    WNDCLASSW type{};
    type.lpfnWndProc = window_proc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"Dlugosz.WindowTextReply.Probe.v1";
    if (!RegisterClassW(&type)) { return 4; }
    const HWND window = CreateWindowExW(0, type.lpszClassName, L"Window text reply probe", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, type.hInstance, &probe);
    if (!window) { return 5; }
    std::wprintf(L"%llu\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(window)));
    std::fflush(stdout);
    if (count == 5) {
        const HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, arguments[4]);
        if (!ready) { DestroyWindow(window); return 6; }
        const BOOL signalled = SetEvent(ready);
        CloseHandle(ready);
        if (!signalled) { DestroyWindow(window); return 7; }
    }
    MSG message{};
    for (;;) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) { return result == 0 ? 0 : 8; }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

} // namespace

int wmain(int count, wchar_t** arguments) {
    try { return run(count, arguments); }
    catch (...) { return 9; }
}
