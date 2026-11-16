#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/shell_context_menu.hpp"

#include <array>
#include <climits>
#include <memory>
#include <string>

namespace launcher {
namespace {

class ComApartment final {
public:
    ComApartment() noexcept : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }
    bool ready() const noexcept { return SUCCEEDED(result_); }

private:
    HRESULT result_;
};

struct PidlDeleter {
    using pointer = PIDLIST_ABSOLUTE;
    void operator()(PIDLIST_ABSOLUTE value) const noexcept { CoTaskMemFree(value); }
};

bool contains_menu(HMENU parent, HMENU candidate) noexcept {
    if (parent == candidate) {
        return true;
    }
    const int count = GetMenuItemCount(parent);
    for (int index = 0; index < count; ++index) {
        if (const HMENU child = GetSubMenu(parent, index);
                child && contains_menu(child, candidate)) {
            return true;
        }
    }
    return false;
}

// Supply ANSI equivalents too for old Shell extensions; modern handlers use
// the Unicode members of CMINVOKECOMMANDINFOEX.
HRESULT ansi_text(const std::wstring& source, std::string& destination) {
    if (source.empty()) {
        return S_OK;
    }
    if (source.size() >= INT_MAX || source.find(L'\0') != std::wstring::npos) {
        return E_INVALIDARG;
    }
    const int count = WideCharToMultiByte(CP_ACP, 0, source.c_str(), -1,
        nullptr, 0, nullptr, nullptr);
    if (count == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    destination.resize(static_cast<std::size_t>(count));
    if (WideCharToMultiByte(CP_ACP, 0, source.c_str(), -1,
            destination.data(), count, nullptr, nullptr) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    destination.resize(static_cast<std::size_t>(count - 1));
    return S_OK;
}

} // namespace

struct ShellContextMenu::State {
    // Declared first so all Shell interfaces are released before COM teardown.
    ComApartment apartment;
    std::unique_ptr<ITEMIDLIST, PidlDeleter> pidl;
    Microsoft::WRL::ComPtr<IShellFolder> parent;
    Microsoft::WRL::ComPtr<IContextMenu> context;
    Microsoft::WRL::ComPtr<IContextMenu2> context2;
    Microsoft::WRL::ComPtr<IContextMenu3> context3;
    HWND owner = nullptr;
    HMENU menu = nullptr;
    UINT first_id = 0;
    UINT count = 0;
};

ShellContextMenu::ShellContextMenu(HWND owner, const std::wstring& target)
    : state_(std::make_unique<State>()) {
    state_->owner = owner;
    if (!state_->apartment.ready() || target.empty() ||
            target.find(L'\0') != std::wstring::npos) {
        return;
    }
    PIDLIST_ABSOLUTE parsed = nullptr;
    const HRESULT parsed_result = SHParseDisplayName(target.c_str(), nullptr, &parsed, 0, nullptr);
    state_->pidl.reset(parsed);
    if (FAILED(parsed_result) || !parsed) {
        return;
    }
    PCUITEMID_CHILD child = nullptr;
    if (FAILED(SHBindToParent(parsed, IID_PPV_ARGS(state_->parent.GetAddressOf()), &child))) {
        return;
    }
    if (FAILED(state_->parent->GetUIObjectOf(owner, 1, &child,
            IID_IContextMenu, nullptr, reinterpret_cast<void**>(state_->context.GetAddressOf())))) {
        return;
    }
    state_->context.As(&state_->context3);
    state_->context.As(&state_->context2);
}

ShellContextMenu::~ShellContextMenu() = default;

bool ShellContextMenu::available() const noexcept {
    return state_->context != nullptr;
}

HRESULT ShellContextMenu::append(HMENU menu, UINT first_id, UINT last_id, bool extended) {
    if (!available()) {
        return E_NOINTERFACE;
    }
    // Commands are returned as 16-bit offsets by InvokeCommand.
    if (!menu || first_id == 0 || first_id > last_id || last_id > 0xffffU || state_->menu) {
        return E_INVALIDARG;
    }
    const int index = GetMenuItemCount(menu);
    if (index < 0) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_MENU_HANDLE);
    }
    const UINT flags = CMF_NORMAL | CMF_ITEMMENU | (extended ? CMF_EXTENDEDVERBS : 0U);
    const HRESULT result = state_->context->QueryContextMenu(menu,
        static_cast<UINT>(index), first_id, last_id, flags);
    if (SUCCEEDED(result)) {
        const UINT count = HRESULT_CODE(result);
        if (count > last_id - first_id + 1U) {
            return E_UNEXPECTED;
        }
        state_->menu = menu;
        state_->first_id = first_id;
        state_->count = count;
    }
    return result;
}

bool ShellContextMenu::owns(UINT command) const noexcept {
    return state_->menu && command >= state_->first_id &&
        command - state_->first_id < state_->count;
}

std::wstring ShellContextMenu::verb(UINT command) const {
    if (!owns(command)) {
        return {};
    }
    std::array<wchar_t, 512> name{};
    if (FAILED(state_->context->GetCommandString(command - state_->first_id,
            GCS_VERBW, nullptr, reinterpret_cast<char*>(name.data()),
            static_cast<UINT>(name.size())))) {
        return {};
    }
    name.back() = L'\0';
    return name.data();
}

HRESULT ShellContextMenu::invoke(UINT command, POINT screen,
        const std::wstring& arguments, const std::wstring& working_directory) {
    if (!owns(command)) {
        return E_INVALIDARG;
    }
    std::string arguments_ansi;
    std::string directory_ansi;
    HRESULT result = ansi_text(arguments, arguments_ansi);
    if (FAILED(result)) {
        return result;
    }
    result = ansi_text(working_directory, directory_ansi);
    if (FAILED(result)) {
        return result;
    }
    const UINT offset = command - state_->first_id;
    CMINVOKECOMMANDINFOEX info{};
    info.cbSize = sizeof(info);
    info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
    if (GetKeyState(VK_SHIFT) < 0) {
        info.fMask |= CMIC_MASK_SHIFT_DOWN;
    }
    if (GetKeyState(VK_CONTROL) < 0) {
        info.fMask |= CMIC_MASK_CONTROL_DOWN;
    }
    info.hwnd = state_->owner;
    info.lpVerb = MAKEINTRESOURCEA(offset);
    info.lpVerbW = MAKEINTRESOURCEW(offset);
    info.lpParameters = arguments.empty() ? nullptr : arguments_ansi.c_str();
    info.lpParametersW = arguments.empty() ? nullptr : arguments.c_str();
    info.lpDirectory = working_directory.empty() ? nullptr : directory_ansi.c_str();
    info.lpDirectoryW = working_directory.empty() ? nullptr : working_directory.c_str();
    info.nShow = SW_SHOWNORMAL;
    info.ptInvoke = screen;
    return state_->context->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info));
}

bool ShellContextMenu::handle_message(UINT message, WPARAM wparam, LPARAM lparam,
        LRESULT& result) const noexcept {
    if (!state_->menu) {
        return false;
    }
    switch (message) {
    case WM_INITMENUPOPUP:
    case WM_UNINITMENUPOPUP:
        if (!contains_menu(state_->menu, reinterpret_cast<HMENU>(wparam))) {
            return false;
        }
        break;
    case WM_MENUCHAR:
        if (!contains_menu(state_->menu, reinterpret_cast<HMENU>(lparam))) {
            return false;
        }
        break;
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (!item || item->CtlType != ODT_MENU ||
                (!owns(item->itemID) && !contains_menu(state_->menu,
                    reinterpret_cast<HMENU>(item->hwndItem)))) {
            return false;
        }
        break;
    }
    case WM_MEASUREITEM: {
        const auto* item = reinterpret_cast<const MEASUREITEMSTRUCT*>(lparam);
        // Shell popup headings may use the anonymous command ID. Launcher's
        // popup drawing helper intercepts its own rows before they reach here.
        if (!item || item->CtlType != ODT_MENU ||
                (!owns(item->itemID) && item->itemID != static_cast<UINT>(-1))) {
            return false;
        }
        break;
    }
    default:
        return false;
    }
    result = 0;
    if (state_->context3) {
        return state_->context3->HandleMenuMsg2(message, wparam, lparam, &result) == S_OK;
    }
    // WM_MENUCHAR requires IContextMenu3's returned result.
    if (message != WM_MENUCHAR && state_->context2 &&
            state_->context2->HandleMenuMsg(message, wparam, lparam) == S_OK) {
        result = (message == WM_DRAWITEM || message == WM_MEASUREITEM) ? TRUE : 0;
        return true;
    }
    return false;
}

} // namespace launcher
