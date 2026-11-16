#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/desktop_icons.hpp"

namespace launcher {
namespace {

using Microsoft::WRL::ComPtr;

class ComScope final {
public:
    ComScope() noexcept : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }
    bool available() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }
private:
    HRESULT result_;
};

UINT desktop_dpi() noexcept {
    UINT dpi = GetDpiForWindow(GetShellWindow());
    if (dpi == 0) {
        dpi = GetDpiForSystem();
    }
    return dpi == 0 ? 96 : dpi;
}

NativeDesktopIconSize metric_fallback() noexcept {
    const UINT dpi = desktop_dpi();
    int pixels = GetSystemMetricsForDpi(SM_CXICON, dpi);
    if (pixels <= 0) {
        pixels = 32;
    }
    return {static_cast<float>(pixels) * 96.0F / static_cast<float>(dpi),
            pixels, dpi, false};
}

} // namespace

NativeDesktopIconSize sample_desktop_icon_size() noexcept {
    const auto fallback = metric_fallback();
    const ComScope com;
    if (!com.available()) {
        return fallback;
    }

    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(windows.GetAddressOf())))) {
        return fallback;
    }
    VARIANT location{};
    location.vt = VT_I4;
    location.lVal = CSIDL_DESKTOP;
    VARIANT root{};
    long unused_window = 0;
    ComPtr<IDispatch> dispatch;
    if (windows->FindWindowSW(&location, &root, SWC_DESKTOP,
            &unused_window, SWFO_NEEDDISPATCH, dispatch.GetAddressOf()) != S_OK || !dispatch) {
        return fallback;
    }
    ComPtr<IServiceProvider> provider;
    if (FAILED(dispatch.As(&provider))) {
        return fallback;
    }
    ComPtr<IShellBrowser> browser;
    if (FAILED(provider->QueryService(SID_STopLevelBrowser,
            IID_PPV_ARGS(browser.GetAddressOf())))) {
        return fallback;
    }
    ComPtr<IShellView> view;
    if (FAILED(browser->QueryActiveShellView(view.GetAddressOf()))) {
        return fallback;
    }
    HWND window = nullptr;
    if (FAILED(view->GetWindow(&window)) || window == nullptr) {
        return fallback;
    }
    const UINT dpi = GetDpiForWindow(window);
    if (dpi == 0) {
        return fallback;
    }
    ComPtr<IFolderView2> folder;
    if (FAILED(view.As(&folder))) {
        return fallback;
    }
    FOLDERVIEWMODE mode = FVM_AUTO;
    int nominal_size = 0;
    if (FAILED(folder->GetViewModeAndIconSize(&mode, &nominal_size)) || nominal_size <= 0) {
        return fallback;
    }
    // The desktop view reports its nominal size (48 at 125% draws as 60 px).
    // Preserve that size in DIPs and apply DPI only for physical diagnostics.
    return {static_cast<float>(nominal_size),
            MulDiv(nominal_size, static_cast<int>(dpi), 96), dpi, true};
}

} // namespace launcher
