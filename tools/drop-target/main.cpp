#include <windows.h>
#include <shlobj.h>
#include <wrl/client.h>

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "entry.hpp"
#include "editors.hpp"
#include "drop-zone.hpp"
#include "configuration.hpp"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
constexpr wchar_t window_class[] = L"LauncherDropWindow";
constexpr wchar_t window_title[] = L"Launcher Drop";
constexpr int add_id = 201, copy_id = 202, clear_id = 203, notepad_id = 204, editors_id = 206, zone_id = 207;
constexpr std::size_t text_limit = 16 * 1024 * 1024;

std::wstring wide_error(const std::exception& error) {
    const std::string text(error.what());
    return {text.begin(), text.end()};
}

void check(HRESULT result, const char* message) {
    if (FAILED(result)) { throw std::runtime_error(message); }
}

std::wstring select_configuration(const std::filesystem::path& argument) {
    std::wstring executable;
    executable.resize_and_overwrite(32768, [](wchar_t* buffer, std::size_t capacity) noexcept {
        const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(capacity));
        return length < capacity ? static_cast<std::size_t>(length) : std::size_t{0};
    });
    if (executable.empty()) { throw std::runtime_error("Cannot find the helper executable."); }
    return launcher::drop::default_configuration(std::filesystem::path(executable).parent_path(), [] {
        PWSTR folder = nullptr;
        check(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &folder), "Cannot find the local AppData folder.");
        launcher::drop::TaskString owned(folder);
        return std::filesystem::path(folder) / L"D\u0142ugosz" / L"Launcher";
    }, argument, [](const auto& destination) {
        const auto text = L"Launcher.toml was not found.\n\nIf you use a configuration with another name, exit and start Launcher Drop with --config followed by that filename.\n\nCreate a starter configuration here?\n\n" + destination.wstring();
        return MessageBoxW(nullptr, text.c_str(), window_title, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
    }).wstring();
}

class Window;

class DropTarget final : public IDropTarget {
public:
    explicit DropTarget(Window& owner) : owner_(owner) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) { return E_POINTER; }
        *result = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDropTarget) { return E_NOINTERFACE; }
        *result = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) { delete this; }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect = launcher::drop::drop_effect(*effect, accepted_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override;

private:
    std::atomic<ULONG> references_{1};
    Window& owner_;
    bool accepted_ = false;
};

class Window final {
public:
    explicit Window(std::wstring configuration) : configuration_(std::move(configuration)) {}
    ~Window() {
        if (font_) { DeleteObject(font_); }
        if (editor_font_) { DeleteObject(editor_font_); }
        if (zone_font_) { DeleteObject(zone_font_); }
        if (launcher_icon_) { DestroyIcon(launcher_icon_); }
    }

    HWND create(HINSTANCE instance) {
        instance_ = instance;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = procedure;
        wc.hInstance = instance;
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
        wc.hIconSm = wc.hIcon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = window_class;
        if (!RegisterClassExW(&wc)) { throw std::runtime_error("Cannot register the helper window."); }
        const UINT dpi = GetDpiForSystem();
        RECT bounds{0, 0, MulDiv(780, dpi, 96), MulDiv(510, dpi, 96)};
        AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_CONTROLPARENT, dpi);
        const HWND created = CreateWindowExW(WS_EX_CONTROLPARENT, window_class, window_title, WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, instance, this);
        if (!created) { throw std::runtime_error("Cannot create the helper window."); }
        return created;
    }

    void status(const std::wstring& text) { SetWindowTextW(status_, text.c_str()); }
    void drag_feedback(bool active) {
        drag_active_ = active;
        SetWindowTextW(zone_, active ? L"Release to append Launcher entries" : L"Drop programs, documents, folders, or shortcuts here");
        InvalidateRect(zone_, nullptr, FALSE);
    }

    bool append_items(IShellItemArray* items) {
        DWORD count = 0;
        check(items->GetCount(&count), "Cannot read the dropped item list.");
        std::wstring entries, errors;
        unsigned added = 0, skipped = 0;
        for (DWORD index = 0; index < count; ++index) {
            ComPtr<IShellItem> item;
            try {
                check(items->GetItemAt(index, &item), "Cannot read this Shell item.");
                entries += launcher::drop::format_entry(launcher::drop::shell_target(item.Get()));
                ++added;
            } catch (const std::exception& error) {
                ++skipped;
                PWSTR caption = nullptr;
                if (item) { item->GetDisplayName(SIGDN_NORMALDISPLAY, &caption); }
                const launcher::drop::TaskString owned(caption);
                errors += (caption ? std::wstring(caption) : L"Item " + std::to_wstring(index + 1)) + L": " + wide_error(error) + L"\r\n\r\n";
            }
        }
        if (!entries.empty()) { append(entries); }
        status(L"Added " + std::to_wstring(added) + L" item(s)." + (skipped ? L" Skipped " + std::to_wstring(skipped) + L" unsupported item(s)." : L" Ready for another drop or Copy All."));
        if (!errors.empty()) { MessageBoxW(window_, errors.c_str(), L"Some items could not be added", MB_OK | MB_ICONINFORMATION); }
        return added != 0;
    }

    void append_path(const wchar_t* path) {
        ComPtr<IShellItem> item;
        check(SHCreateItemFromParsingName(path, nullptr, IID_PPV_ARGS(&item)), "Cannot find the item passed to Launcher Drop.");
        append(launcher::drop::format_entry(launcher::drop::shell_target(item.Get())));
    }

private:
    HWND control(const wchar_t* type, const wchar_t* text, DWORD style, int id = 0, DWORD extended = 0) {
        const HWND result = CreateWindowExW(extended, type, text, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (!result) { throw std::runtime_error("Cannot create a helper control."); }
        return result;
    }

    void initialize() {
        zone_ = control(L"STATIC", L"Drop programs, documents, folders, or shortcuts here", SS_OWNERDRAW, zone_id);
        hint_ = control(L"STATIC", L"Entries append below. Paste them inside a tab's items = [ ... ] list.", 0);
        editor_ = control(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, 205, WS_EX_CLIENTEDGE);
        SendMessageW(editor_, EM_SETLIMITTEXT, text_limit, 0);
        SetWindowSubclass(editor_, editor_procedure, 1, 0);
        add_ = control(L"BUTTON", L"&Add files...", WS_TABSTOP, add_id);
        copy_ = control(L"BUTTON", L"&Copy All", WS_TABSTOP, copy_id);
        clear_ = control(L"BUTTON", L"C&lear", WS_TABSTOP, clear_id);
        notepad_ = control(L"BUTTON", L"Open Launcher.toml in &Notepad", WS_TABSTOP, notepad_id);
        editors_ = control(L"BUTTON", L"Other &editors", WS_TABSTOP | BS_SPLITBUTTON, editors_id);
        status_ = control(L"STATIC", L"Ready. Drop one or more items, then copy the entries into your configuration.", 0);
        update_fonts();
        layout();
        ComPtr<IDropTarget> target;
        target.Attach(new DropTarget(*this));
        check(RegisterDragDrop(zone_, target.Get()), "Cannot register the drop zone.");
        drop_registered_ = true;
        SetFocus(editor_);
    }

    int scaled(int value) const { return MulDiv(value, GetDpiForWindow(window_), 96); }

    void update_fonts() {
        const UINT dpi = GetDpiForWindow(window_);
        const HFONT regular = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        const HFONT mono = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        const HFONT title = CreateFontW(-MulDiv(16, dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HICON launcher = nullptr;
        const HRESULT icon_result = LoadIconWithScaleDown(instance_, MAKEINTRESOURCEW(102), MulDiv(96, dpi, 96), MulDiv(96, dpi, 96), &launcher);
        if (!regular || !mono || !title || FAILED(icon_result)) {
            if (regular) { DeleteObject(regular); }
            if (mono) { DeleteObject(mono); }
            if (title) { DeleteObject(title); }
            if (launcher) { DestroyIcon(launcher); }
            throw std::runtime_error("Cannot create the helper fonts or Launcher icon.");
        }
        for (const HWND child : {zone_, hint_, add_, copy_, clear_, notepad_, editors_, status_}) {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(regular), TRUE);
        }
        SendMessageW(editor_, WM_SETFONT, reinterpret_cast<WPARAM>(mono), TRUE);
        if (font_) { DeleteObject(font_); }
        if (editor_font_) { DeleteObject(editor_font_); }
        if (zone_font_) { DeleteObject(zone_font_); }
        if (launcher_icon_) { DestroyIcon(launcher_icon_); }
        font_ = regular;
        editor_font_ = mono;
        zone_font_ = title;
        launcher_icon_ = launcher;
        InvalidateRect(zone_, nullptr, FALSE);
    }

    void layout() {
        RECT bounds{};
        GetClientRect(window_, &bounds);
        const int margin = scaled(14), gap = scaled(8), button_height = scaled(30);
        const int width = bounds.right - 2 * margin;
        const int bottom = bounds.bottom - margin - scaled(32);
        MoveWindow(zone_, margin, margin, width, scaled(128), TRUE);
        MoveWindow(hint_, margin, scaled(152), width, scaled(26), TRUE);
        const int buttons_y = bottom - 2 * gap - 2 * button_height;
        MoveWindow(editor_, margin, scaled(186), width, std::max(scaled(70), buttons_y - gap - scaled(186)), TRUE);
        int x = margin;
        for (const auto [child, button_width] : {std::pair{add_, 104}, {copy_, 92}, {clear_, 76}}) {
            MoveWindow(child, x, buttons_y, scaled(button_width), button_height, TRUE);
            x += scaled(button_width) + gap;
        }
        MoveWindow(notepad_, bounds.right - margin - scaled(246), buttons_y, scaled(246), button_height, TRUE);
        MoveWindow(editors_, bounds.right - margin - scaled(246), buttons_y + button_height + gap, scaled(246), button_height, TRUE);
        MoveWindow(status_, margin, bottom, width, scaled(32), TRUE);
    }

    void append(const std::wstring& entries) {
        const int length = GetWindowTextLengthW(editor_);
        if (entries.size() + static_cast<std::size_t>(length) + 2 > text_limit) {
            throw std::runtime_error("The text area is full. Copy its contents before adding more items.");
        }
        std::wstring insertion;
        if (length) {
            std::wstring existing(static_cast<std::size_t>(length) + 1, L'\0');
            GetWindowTextW(editor_, existing.data(), length + 1);
            if (existing[static_cast<std::size_t>(length) - 1] != L'\n') { insertion = L"\r\n"; }
        }
        insertion += entries;
        SendMessageW(editor_, EM_SETSEL, length, length);
        SendMessageW(editor_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(insertion.c_str()));
        if (GetWindowTextLengthW(editor_) != length + static_cast<int>(insertion.size())) {
            throw std::runtime_error("Windows could not append all of the new text. Check the entries before copying.");
        }
        SendMessageW(editor_, EM_SCROLLCARET, 0, 0);
    }

    void add_files() {
        ComPtr<IFileOpenDialog> dialog;
        check(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)), "Cannot open the file chooser.");
        FILEOPENDIALOGOPTIONS options{};
        check(dialog->GetOptions(&options), "Cannot read the file chooser options.");
        options &= ~FOS_FORCEFILESYSTEM;
        check(dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_ALLNONSTORAGEITEMS | FOS_NODEREFERENCELINKS | FOS_DONTADDTORECENT | FOS_NOCHANGEDIR), "Cannot configure the file chooser.");
        dialog->SetTitle(L"Add programs, documents, or shortcuts");
        dialog->SetOkButtonLabel(L"Add");
        const HRESULT shown = dialog->Show(window_);
        if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) { return; }
        check(shown, "Cannot show the file chooser.");
        ComPtr<IShellItemArray> items;
        check(dialog->GetResults(&items), "Cannot read the selected items.");
        append_items(items.Get());
    }

    void copy_all() {
        const int length = GetWindowTextLengthW(editor_);
        if (!length) { status(L"There are no entries to copy yet."); return; }
        const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (static_cast<std::size_t>(length) + 1) * sizeof(wchar_t));
        if (!memory) { throw std::runtime_error("Cannot allocate clipboard text."); }
        auto* text = static_cast<wchar_t*>(GlobalLock(memory));
        if (!text) { GlobalFree(memory); throw std::runtime_error("Cannot prepare clipboard text."); }
        GetWindowTextW(editor_, text, length + 1);
        GlobalUnlock(memory);
        if (!OpenClipboard(window_)) { GlobalFree(memory); throw std::runtime_error("The clipboard is busy. Try Copy All again."); }
        const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
        CloseClipboard();
        if (!copied) { GlobalFree(memory); throw std::runtime_error("Cannot copy the entries to the clipboard."); }
        status(L"Copied all text. Paste it inside a tab's items = [ ... ] list in Launcher.toml.");
    }

    void open_configuration(const launcher::drop::Editor& editor) {
        const DWORD attributes = GetFileAttributesW(configuration_.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            const std::wstring message = L"The configuration file is unavailable:\r\n\r\n" + configuration_ + L"\r\n\r\nCheck the selected file, or start this helper with --config followed by an existing configuration path.";
            MessageBoxW(window_, message.c_str(), window_title, MB_OK | MB_ICONINFORMATION);
            return;
        }
        const std::wstring arguments = L"\"" + configuration_ + L"\"";
        SHELLEXECUTEINFOW execute{sizeof(execute)};
        execute.fMask = SEE_MASK_FLAG_NO_UI;
        execute.hwnd = window_;
        execute.lpFile = editor.executable.c_str();
        execute.lpParameters = arguments.c_str();
        execute.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&execute)) {
            MessageBoxW(window_, (L"Windows could not open the configuration in " + editor.name + L". Try the Notepad button or select another editor.").c_str(), window_title, MB_OK | MB_ICONERROR);
            return;
        }
        status(L"Opened in " + editor.name + L": " + configuration_);
    }

    void open_notepad() {
        std::wstring system(MAX_PATH, L'\0');
        const UINT length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
        if (!length || length >= system.size()) { throw std::runtime_error("Cannot find Windows Notepad."); }
        system.resize(length);
        open_configuration({L"Notepad", system + L"\\notepad.exe"});
    }

    void choose_editor() {
        // Refresh on each opening so installs/uninstalls do not leave stale choices.
        const auto editors = launcher::drop::discover_editors(launcher::drop::find_editor);
        struct MenuOwner {
            HMENU value = CreatePopupMenu();
            ~MenuOwner() { if (value) { DestroyMenu(value); } }
        } menu;
        if (!menu.value) { throw std::runtime_error("Cannot create the editor menu."); }
        if (editors.empty()) { AppendMenuW(menu.value, MF_STRING | MF_GRAYED, 0, L"No supported editors found"); }
        for (std::size_t index = 0; index < editors.size(); ++index) {
            AppendMenuW(menu.value, MF_STRING, index + 1, editors[index].name.c_str());
        }
        RECT bounds{};
        GetWindowRect(editors_, &bounds);
        const UINT selected = TrackPopupMenuEx(menu.value, TPM_RETURNCMD | TPM_RIGHTALIGN, bounds.right, bounds.bottom, window_, nullptr);
        if (selected && selected <= editors.size()) { open_configuration(editors[selected - 1]); }
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            initialize();
            return 0;
        case WM_SIZE:
            if (editor_) { layout(); }
            return 0;
        case WM_DRAWITEM: {
            const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (item.CtlID == zone_id) {
                launcher::drop::paint_drop_zone(item.hDC, item.rcItem, GetDpiForWindow(window_), launcher_icon_, zone_font_, font_, drag_active_);
                return TRUE;
            }
            break;
        }
        case WM_DWMCOLORIZATIONCOLORCHANGED:
        case WM_SETTINGCHANGE:
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED:
            if (zone_) { InvalidateRect(zone_, nullptr, FALSE); }
            break;
        case WM_GETMINMAXINFO: {
            auto& limits = *reinterpret_cast<MINMAXINFO*>(lparam);
            limits.ptMinTrackSize = {scaled(660), scaled(440)};
            return 0;
        }
        case WM_DPICHANGED: {
            const auto& rect = *reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window_, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOACTIVATE | SWP_NOZORDER);
            update_fonts();
            layout();
            return 0;
        }
        case WM_COMMAND:
            if (HIWORD(wparam) == BN_CLICKED) {
                switch (LOWORD(wparam)) {
                case add_id: add_files(); return 0;
                case copy_id: copy_all(); return 0;
                case clear_id:
                    SendMessageW(editor_, EM_SETSEL, 0, -1);
                    SendMessageW(editor_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
                    status(L"Text cleared. Ctrl+Z in the text area can undo this.");
                    SetFocus(editor_);
                    return 0;
                case notepad_id: open_notepad(); return 0;
                case editors_id: choose_editor(); return 0;
                }
            }
            break;
        case WM_NOTIFY: {
            const auto& notification = *reinterpret_cast<NMHDR*>(lparam);
            if (notification.idFrom == editors_id && notification.code == BCN_DROPDOWN) { choose_editor(); return 0; }
            break;
        }
        case WM_SETFOCUS:
            if (editor_) { SetFocus(editor_); }
            return 0;
        case WM_DESTROY:
            if (drop_registered_) { RevokeDragDrop(zone_); drop_registered_ = false; }
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        Window* self;
        if (message == WM_NCCREATE) {
            self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        try {
            if (self) { return self->handle(message, wparam, lparam); }
        } catch (const std::exception& error) {
            MessageBoxW(window, wide_error(error).c_str(), window_title, MB_OK | MB_ICONERROR);
            if (message == WM_CREATE) { return -1; }
        } catch (...) {
            MessageBoxW(window, L"An unexpected error occurred.", window_title, MB_OK | MB_ICONERROR);
            if (message == WM_CREATE) { return -1; }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    static LRESULT CALLBACK editor_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
        if (message == WM_KEYDOWN && wparam == L'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(window, EM_SETSEL, 0, -1);
            return 0;
        }
        if (message == WM_CHAR && wparam == 1) { return 0; }
        if (message == WM_NCDESTROY) { RemoveWindowSubclass(window, editor_procedure, 1); }
        return DefSubclassProc(window, message, wparam, lparam);
    }

    HWND window_ = nullptr, zone_ = nullptr, hint_ = nullptr, editor_ = nullptr;
    HWND add_ = nullptr, copy_ = nullptr, clear_ = nullptr, notepad_ = nullptr, editors_ = nullptr, status_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HFONT font_ = nullptr, editor_font_ = nullptr, zone_font_ = nullptr;
    HICON launcher_icon_ = nullptr;
    bool drop_registered_ = false, drag_active_ = false;
    std::wstring configuration_;
};

HRESULT DropTarget::DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) {
    accepted_ = false;
    for (const CLIPFORMAT format : {static_cast<CLIPFORMAT>(CF_HDROP), static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_SHELLIDLIST))}) {
        FORMATETC request{format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        if (SUCCEEDED(data->QueryGetData(&request))) { accepted_ = true; break; }
    }
    DragOver(0, {}, effect);
    owner_.drag_feedback(*effect != DROPEFFECT_NONE);
    return S_OK;
}

HRESULT DropTarget::DragLeave() {
    accepted_ = false;
    owner_.drag_feedback(false);
    return S_OK;
}

HRESULT DropTarget::Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) {
    const DWORD allowed = launcher::drop::drop_effect(*effect, accepted_);
    *effect = DROPEFFECT_NONE;
    DragLeave();
    if (!allowed) { return S_OK; }
    try {
        ComPtr<IShellItemArray> items;
        check(SHCreateShellItemArrayFromDataObject(data, IID_PPV_ARGS(&items)), "Cannot read the dropped Shell items.");
        if (owner_.append_items(items.Get())) { *effect = allowed; }
        return S_OK;
    } catch (const std::exception& error) {
        owner_.status(L"Nothing added: " + wide_error(error));
        return E_FAIL;
    } catch (...) {
        owner_.status(L"Nothing added: an unexpected drop error occurred.");
        return E_FAIL;
    }
}

struct OleSession {
    OleSession() { check(OleInitialize(nullptr), "Cannot initialize Windows drag and drop."); }
    ~OleSession() { OleUninitialize(); }
};

struct DrawingSession {
    ULONG_PTR token = 0;
    DrawingSession() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) { throw std::runtime_error("Cannot initialize drop-zone drawing."); }
    }
    ~DrawingSession() { Gdiplus::GdiplusShutdown(token); }
};

struct LocalMemoryDeleter {
    void operator()(wchar_t** value) const noexcept { LocalFree(value); }
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    try {
        OleSession ole;
        DrawingSession drawing;
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        if (!InitCommonControlsEx(&controls)) { throw std::runtime_error("Cannot initialize Windows controls."); }
        int count = 0;
        const std::unique_ptr<wchar_t*, LocalMemoryDeleter> arguments(CommandLineToArgvW(GetCommandLineW(), &count));
        if (!arguments) { throw std::runtime_error("Cannot read the command line."); }
        std::wstring configuration;
        std::vector<std::wstring> paths;
        for (int index = 1; index < count; ++index) {
            const std::wstring_view argument(arguments.get()[index]);
            if (argument == L"--config") {
                if (++index == count) { throw std::runtime_error("--config needs a configuration file path."); }
                if (!configuration.empty()) { throw std::runtime_error("Supply only one configuration path."); }
                configuration = arguments.get()[index];
                if (configuration.empty()) { throw std::runtime_error("Configuration filename must not be empty."); }
            } else if (argument.starts_with(L"--")) {
                throw std::runtime_error("Usage: LauncherDrop.exe [--config configuration-file] [files or Shell names...]");
            } else { paths.emplace_back(argument); }
        }
        configuration = select_configuration(configuration);
        if (configuration.empty()) { return 0; }
        Window app(std::move(configuration));
        const HWND window = app.create(instance);
        for (const auto& path : paths) { app.append_path(path.c_str()); }
        ShowWindow(window, show);
        UpdateWindow(window);
        MSG message{};
        BOOL result = 0;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        if (result == -1) { throw std::runtime_error("The Windows message loop failed."); }
        return static_cast<int>(message.wParam);
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, wide_error(error).c_str(), window_title, MB_OK | MB_ICONERROR);
        return 1;
    }
}
