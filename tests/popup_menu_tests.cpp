#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/popup_menu.hpp"

#include <catch2/catch_test_macros.hpp>
#include <oleacc.h>
#include <uxtheme.h>

#include <array>
#include <string>
#include <utility>

namespace {

constexpr ULONG_PTR heading_tag = 0x1a201007;
constexpr LRESULT forwarded_menu_character = MAKELRESULT(41, MNC_SELECT);

struct MenuFixture final {
    HMENU root = CreatePopupMenu();
    HWND window = nullptr;
    HMENU lazy = nullptr;
    unsigned forwarded_characters = 0;
    bool inserted_lazy_row = false;

    MenuFixture()
    {
        static const ATOM window_class = [] {
            WNDCLASSW type{};
            type.lpfnWndProc = procedure;
            type.hInstance = GetModuleHandleW(nullptr);
            type.lpszClassName = L"LauncherPopupMenuTestOwner";
            return RegisterClassW(&type);
        }();
        REQUIRE(window_class != 0);
        REQUIRE(root != nullptr);
        window = CreateWindowExW(0, MAKEINTATOM(window_class), L"Owned menu test", WS_POPUP,
            0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        REQUIRE(window != nullptr);
    }

    ~MenuFixture()
    {
        if (window) { DestroyWindow(window); }
        if (root) { DestroyMenu(root); }
    }

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM word, LPARAM parameter)
    {
        auto* fixture = reinterpret_cast<MenuFixture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            fixture = static_cast<MenuFixture*>(reinterpret_cast<const CREATESTRUCTW*>(parameter)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(fixture));
        }
        if (fixture && message == WM_MENUCHAR) {
            ++fixture->forwarded_characters;
            return forwarded_menu_character;
        }
        if (fixture && message == WM_INITMENUPOPUP && reinterpret_cast<HMENU>(word) == fixture->lazy &&
                GetMenuItemCount(fixture->lazy) == 0) {
            fixture->inserted_lazy_row = AppendMenuW(fixture->lazy, MF_STRING, 91, L"&Lazy command") != FALSE;
        }
        return DefWindowProcW(window, message, word, parameter);
    }
};

MENUITEMINFOW row(HMENU menu, UINT position)
{
    MENUITEMINFOW item{sizeof(item)};
    item.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_DATA | MIIM_SUBMENU;
    REQUIRE(GetMenuItemInfoW(menu, position, TRUE, &item));
    return item;
}

std::wstring row_text(HMENU menu, UINT position)
{
    std::array<wchar_t, 256> text{};
    MENUITEMINFOW item{sizeof(item)};
    item.fMask = MIIM_STRING;
    item.dwTypeData = text.data();
    item.cch = static_cast<UINT>(text.size());
    REQUIRE(GetMenuItemInfoW(menu, position, TRUE, &item));
    return text.data();
}

void set_data(HMENU menu, UINT position, ULONG_PTR data)
{
    MENUITEMINFOW item{sizeof(item)};
    item.fMask = MIIM_DATA;
    item.dwItemData = data;
    REQUIRE(SetMenuItemInfoW(menu, position, TRUE, &item));
}

void highlight(HWND owner, HMENU menu, int selected)
{
    const int count = GetMenuItemCount(menu);
    REQUIRE(count >= 0);
    for (int position = 0; position < count; ++position) {
        REQUIRE(HiliteMenuItem(owner, menu, static_cast<UINT>(position), MF_BYPOSITION | MF_UNHILITE));
    }
    if (selected >= 0) {
        REQUIRE(HiliteMenuItem(owner, menu, static_cast<UINT>(selected), MF_BYPOSITION | MF_HILITE));
    }
    for (int position = 0; position < count; ++position) {
        CAPTURE(position, selected);
        REQUIRE(((row(menu, static_cast<UINT>(position)).fState & MFS_HILITE) != 0) == (position == selected));
    }
}

LRESULT character(HWND window, HMENU menu, wchar_t value)
{
    return SendMessageW(window, WM_MENUCHAR, MAKEWPARAM(value, MF_POPUP), reinterpret_cast<LPARAM>(menu));
}

} // namespace

TEST_CASE("Popup drawing owns only Launcher rows and restores native menu metadata", "[popup-menu]")
{
    MenuFixture fixture;
    const HMENU child = CreatePopupMenu();
    const HMENU shell = CreatePopupMenu();
    REQUIRE(child != nullptr);
    REQUIRE(shell != nullptr);
    REQUIRE(AppendMenuW(fixture.root, MF_STRING | MF_CHECKED, 10, L"&Open"));
    REQUIRE(AppendMenuW(fixture.root, MF_STRING | MF_DISABLED, 0, L"Installed Versions"));
    REQUIRE(AppendMenuW(fixture.root, MF_SEPARATOR, 0, nullptr));
    REQUIRE(AppendMenuW(fixture.root, MF_POPUP, reinterpret_cast<UINT_PTR>(child), L"Owned submenu"));
    REQUIRE(AppendMenuW(fixture.root, MF_POPUP, reinterpret_cast<UINT_PTR>(shell), L"Windows commands"));
    REQUIRE(AppendMenuW(child, MF_STRING, 11, L"&Child command"));
    REQUIRE(AppendMenuW(shell, MF_STRING, 12, L"Shell command"));
    set_data(fixture.root, 0, 0x1120);
    set_data(fixture.root, 1, heading_tag);
    set_data(child, 0, 0x2230);
    set_data(shell, 0, 0x3340);
    const auto original_open = row(fixture.root, 0);
    const auto original_heading = row(fixture.root, 1);
    const auto original_separator = row(fixture.root, 2);
    const auto original_child = row(child, 0);
    const auto original_shell = row(shell, 0);
    {
        launcher::PopupMenuDrawing drawing(fixture.window, fixture.root, 96, heading_tag, shell);
        REQUIRE((row(fixture.root, 0).fType & MFT_OWNERDRAW) != 0);
        CHECK((row(fixture.root, 1).fType & MFT_OWNERDRAW) != 0);
        CHECK((row(fixture.root, 2).fType & MFT_OWNERDRAW) != 0);
        CHECK((row(child, 0).fType & MFT_OWNERDRAW) != 0);
        CHECK(row(shell, 0).fType == original_shell.fType);
        CHECK(row(shell, 0).dwItemData == original_shell.dwItemData);
        CHECK(row(fixture.root, 0).fState == original_open.fState);
        CHECK(row_text(fixture.root, 0) == L"&Open");
        std::array<wchar_t, 256> native_text{};
        REQUIRE(GetMenuStringW(fixture.root, 0, native_text.data(), static_cast<int>(native_text.size()), MF_BYPOSITION) > 0);
        CHECK(std::wstring(native_text.data()) == L"&Open");
        const auto* accessible = reinterpret_cast<const MSAAMENUINFO*>(row(fixture.root, 0).dwItemData);
        REQUIRE(accessible != nullptr);
        CHECK(accessible->dwMSAASignature == MSAA_MENU_SIG);
        REQUIRE(accessible->pszWText != nullptr);
        CHECK(std::wstring(accessible->pszWText).find(L"Open") != std::wstring::npos);
        CHECK(accessible->cchWText > 0);
    }
    for (const auto& pair : {std::pair{row(fixture.root, 0), original_open},
            std::pair{row(fixture.root, 1), original_heading}, std::pair{row(fixture.root, 2), original_separator},
            std::pair{row(child, 0), original_child}, std::pair{row(shell, 0), original_shell}}) {
        CHECK(pair.first.fType == pair.second.fType);
        CHECK(pair.first.dwItemData == pair.second.dwItemData);
        CHECK(pair.first.fState == pair.second.fState);
    }
}

TEST_CASE("Popup drawing preserves menu mnemonic execution and duplicate selection", "[popup-menu]")
{
    MenuFixture fixture;
    const HMENU shell = CreatePopupMenu();
    REQUIRE(shell != nullptr);
    REQUIRE(AppendMenuW(fixture.root, MF_STRING, 10, L"&Open"));
    REQUIRE(AppendMenuW(fixture.root, MF_STRING, 11, L"&First"));
    REQUIRE(AppendMenuW(fixture.root, MF_STRING, 12, L"&Further"));
    REQUIRE(AppendMenuW(fixture.root, MF_STRING, 13, L"Save && Go"));
    REQUIRE(AppendMenuW(fixture.root, MF_STRING | MF_DISABLED, 0, L"&Ignored heading"));
    REQUIRE(AppendMenuW(fixture.root, MF_POPUP, reinterpret_cast<UINT_PTR>(shell), L"Windows commands"));
    REQUIRE(AppendMenuW(fixture.root, MF_STRING, 14, L"Plain"));
    REQUIRE(AppendMenuW(fixture.root, MF_STRING, 15, L"Proceed"));
    REQUIRE(AppendMenuW(fixture.root, MF_STRING, 16, L"Open plain"));
    set_data(fixture.root, 4, heading_tag);
    launcher::PopupMenuDrawing drawing(fixture.window, fixture.root, 96, heading_tag, shell);
    const auto unique = character(fixture.window, fixture.root, L'o');
    CHECK(LOWORD(unique) == 0);
    CHECK(HIWORD(unique) == MNC_EXECUTE);
    const auto first_letter = character(fixture.window, fixture.root, L's');
    CHECK(LOWORD(first_letter) == 3);
    CHECK(HIWORD(first_letter) == MNC_EXECUTE);
    const auto duplicate_initial = character(fixture.window, fixture.root, L'p');
    CHECK(LOWORD(duplicate_initial) == 6);
    CHECK(HIWORD(duplicate_initial) == MNC_SELECT);
    highlight(fixture.window, fixture.root, 6);
    const auto next_initial = character(fixture.window, fixture.root, L'P');
    CHECK(LOWORD(next_initial) == 7);
    CHECK(HIWORD(next_initial) == MNC_SELECT);
    highlight(fixture.window, fixture.root, -1);
    const auto duplicate = character(fixture.window, fixture.root, L'f');
    CHECK(LOWORD(duplicate) == 1);
    CHECK(HIWORD(duplicate) == MNC_SELECT);
    highlight(fixture.window, fixture.root, 1);
    const auto next = character(fixture.window, fixture.root, L'F');
    CHECK(LOWORD(next) == 2);
    CHECK(HIWORD(next) == MNC_SELECT);
    highlight(fixture.window, fixture.root, 2);
    const auto wrapped = character(fixture.window, fixture.root, L'f');
    CHECK(LOWORD(wrapped) == 1);
    CHECK(HIWORD(wrapped) == MNC_SELECT);
    CHECK(HIWORD(character(fixture.window, fixture.root, L'g')) == MNC_IGNORE);
    CHECK(HIWORD(character(fixture.window, fixture.root, L'&')) == MNC_IGNORE);
    CHECK(HIWORD(character(fixture.window, fixture.root, L'i')) == MNC_IGNORE);
    CHECK(fixture.forwarded_characters == 0);
    CHECK(character(fixture.window, shell, L'o') == forwarded_menu_character);
    CHECK(fixture.forwarded_characters == 1);
}

TEST_CASE("Popup drawing adopts a lazy version page after its owner populates it", "[popup-menu]")
{
    MenuFixture fixture;
    fixture.lazy = CreatePopupMenu();
    REQUIRE(fixture.lazy != nullptr);
    REQUIRE(AppendMenuW(fixture.root, MF_POPUP, reinterpret_cast<UINT_PTR>(fixture.lazy), L"More versions"));
    {
        launcher::PopupMenuDrawing drawing(fixture.window, fixture.root, 96, heading_tag);
        REQUIRE(GetMenuItemCount(fixture.lazy) == 0);
        SendMessageW(fixture.window, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(fixture.lazy), 0);
        REQUIRE(fixture.inserted_lazy_row);
        REQUIRE(GetMenuItemCount(fixture.lazy) == 1);
        CHECK((row(fixture.lazy, 0).fType & MFT_OWNERDRAW) != 0);
        CHECK(row_text(fixture.lazy, 0) == L"&Lazy command");
        const auto chosen = character(fixture.window, fixture.lazy, L'l');
        CHECK(LOWORD(chosen) == 0);
        CHECK(HIWORD(chosen) == MNC_EXECUTE);
    }
    CHECK((row(fixture.lazy, 0).fType & MFT_OWNERDRAW) == 0);
    CHECK(row(fixture.lazy, 0).dwItemData == 0);
}

TEST_CASE("Popup drawing measures actual menu content and restores the drawing context", "[popup-menu]")
{
    MenuFixture fixture;
    REQUIRE(AppendMenuW(fixture.root, MF_STRING | MF_DISABLED, 0, L"Installed Versions"));
    REQUIRE(AppendMenuW(fixture.root, MF_STRING | MF_CHECKED, 10, L"&Open\tCtrl+O"));
    set_data(fixture.root, 0, heading_tag);
    struct ThemeProperties final {
        DWORD previous = GetThemeAppProperties();
        ~ThemeProperties() { SetThemeAppProperties(previous); }
    } properties;
    SECTION("normal theme") { }
    SECTION("visual styles disabled for this process") {
        SetThemeAppProperties(0);
        SendMessageW(fixture.window, WM_THEMECHANGED, 0, 0);
        REQUIRE(GetThemeAppProperties() == 0);
    }
    launcher::PopupMenuDrawing drawing(fixture.window, fixture.root, 96, heading_tag);
    MEASUREITEMSTRUCT heading{ODT_MENU};
    heading.itemID = 0;
    heading.itemData = row(fixture.root, 0).dwItemData;
    REQUIRE(SendMessageW(fixture.window, WM_MEASUREITEM, 0, reinterpret_cast<LPARAM>(&heading)) != FALSE);
    CHECK(heading.itemWidth > 80);
    REQUIRE(heading.itemWidth < 600);
    CHECK(heading.itemHeight >= 12);
    REQUIRE(heading.itemHeight < 100);
    MEASUREITEMSTRUCT command{ODT_MENU};
    command.itemID = 10;
    command.itemData = row(fixture.root, 1).dwItemData;
    REQUIRE(SendMessageW(fixture.window, WM_MEASUREITEM, 0, reinterpret_cast<LPARAM>(&command)) != FALSE);
    CHECK(command.itemWidth > 80);
    CHECK(command.itemHeight >= 12);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 600;
    info.bmiHeader.biHeight = -100;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    const HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    REQUIRE(bitmap != nullptr);
    const HDC dc = CreateCompatibleDC(nullptr);
    REQUIRE(dc != nullptr);
    const auto previous_bitmap = SelectObject(dc, bitmap);
    const auto original_font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(dc, RGB(12, 34, 56));
    SetBkColor(dc, RGB(78, 90, 12));
    SetBkMode(dc, OPAQUE);
    const auto selected_font = GetCurrentObject(dc, OBJ_FONT);
    DRAWITEMSTRUCT paint{ODT_MENU};
    paint.itemID = 0;
    paint.itemAction = ODA_DRAWENTIRE;
    paint.itemState = ODS_DISABLED;
    paint.hwndItem = reinterpret_cast<HWND>(fixture.root);
    paint.hDC = dc;
    paint.rcItem = RECT{0, 0, static_cast<LONG>(heading.itemWidth), static_cast<LONG>(heading.itemHeight)};
    paint.itemData = heading.itemData;
    const auto painted = SendMessageW(fixture.window, WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&paint));
    const auto restored_font = GetCurrentObject(dc, OBJ_FONT);
    const auto restored_text = GetTextColor(dc);
    const auto restored_background = GetBkColor(dc);
    const auto restored_mode = GetBkMode(dc);
    const auto background_pixel = GetPixel(dc, 1, 1);
    GdiFlush();
    unsigned dark = 0;
    for (LONG y = 0; y < paint.rcItem.bottom; ++y) {
        for (LONG x = 0; x < paint.rcItem.right; ++x) {
            const auto value = static_cast<const DWORD*>(pixels)[y * 600 + x] & 0x00ffffff;
            if (value == 0) { ++dark; }
        }
    }
    SelectObject(dc, original_font);
    SelectObject(dc, previous_bitmap);
    DeleteDC(dc);
    DeleteObject(bitmap);
    CHECK(painted != FALSE);
    CHECK(restored_font == selected_font);
    CHECK(restored_text == RGB(12, 34, 56));
    CHECK(restored_background == RGB(78, 90, 12));
    CHECK(restored_mode == OPAQUE);
    CHECK(dark > 20);
    if (!(GetThemeAppProperties() & STAP_ALLOW_CONTROLS)) { CHECK(background_pixel == GetSysColor(COLOR_MENU)); }
}

TEST_CASE("Popup drawing paints borrowed icons and gives checks the shared gutter", "[popup-menu]")
{
    struct Bitmap final {
        HBITMAP value = nullptr;
        BYTE* pixels = nullptr;
        Bitmap(int width, int height) {
            BITMAPINFO info{};
            info.bmiHeader = BITMAPINFOHEADER{sizeof(BITMAPINFOHEADER), width, -height, 1, 32};
            void* storage = nullptr;
            value = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &storage, nullptr, 0);
            pixels = static_cast<BYTE*>(storage);
            REQUIRE(value != nullptr);
        }
        ~Bitmap() { if (value) { DeleteObject(value); } }
    } borrowed(16, 16), destination(600, 100);
    // An opaque colored center and transparent border form a premultiplied BGRA icon.
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            auto* pixel = borrowed.pixels + (y * 16 + x) * 4;
            const bool visible = x >= 2 && x < 14 && y >= 2 && y < 14;
            pixel[0] = visible ? 196 : 0; pixel[1] = visible ? 64 : 0;
            pixel[2] = visible ? 239 : 0; pixel[3] = visible ? 255 : 0;
        }
    }
    MenuFixture fixture;
    REQUIRE(AppendMenuW(fixture.root, MF_STRING | MF_DISABLED, 0, L"Installed Versions"));
    REQUIRE(AppendMenuW(fixture.root, MF_STRING, 10, L""));
    set_data(fixture.root, 0, heading_tag);
    MENUINFO style{sizeof(style)};
    style.fMask = MIM_STYLE; style.dwStyle = MNS_CHECKORBMP;
    REQUIRE(SetMenuInfo(fixture.root, &style));
    MENUITEMINFOW item{sizeof(item)};
    item.fMask = MIIM_BITMAP; item.hbmpItem = borrowed.value;
    REQUIRE(SetMenuItemInfoW(fixture.root, 1, TRUE, &item));
    {
        launcher::PopupMenuDrawing drawing(fixture.window, fixture.root, 96, heading_tag);
        MEASUREITEMSTRUCT measure{ODT_MENU};
        measure.itemID = 10; measure.itemData = row(fixture.root, 1).dwItemData;
        REQUIRE(SendMessageW(fixture.window, WM_MEASUREITEM, 0, reinterpret_cast<LPARAM>(&measure)) != FALSE);
        REQUIRE(measure.itemWidth > 16);
        REQUIRE(measure.itemWidth < 600);
        REQUIRE(measure.itemHeight >= 16);
        REQUIRE(measure.itemHeight < 100);
        const HDC dc = CreateCompatibleDC(nullptr);
        REQUIRE(dc != nullptr);
        const auto old_bitmap = SelectObject(dc, destination.value);
        const auto paint = [&](UINT state) {
            DRAWITEMSTRUCT draw{ODT_MENU};
            draw.itemID = 10; draw.itemAction = ODA_DRAWENTIRE; draw.itemState = state;
            draw.hwndItem = reinterpret_cast<HWND>(fixture.root); draw.hDC = dc;
            draw.rcItem = RECT{0, 0, static_cast<LONG>(measure.itemWidth), static_cast<LONG>(measure.itemHeight)};
            draw.itemData = measure.itemData;
            CHECK(SendMessageW(fixture.window, WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&draw)) != FALSE);
            GdiFlush();
            std::array<unsigned, 2> counts{};
            for (UINT y = 0; y < measure.itemHeight; ++y) {
                for (UINT x = 0; x < measure.itemWidth; ++x) {
                    const auto* pixel = destination.pixels + (y * 600 + x) * 4;
                    if (pixel[0] == 196 && pixel[1] == 64 && pixel[2] == 239) { ++counts[0]; }
                    if (pixel[0] < 128 && pixel[1] < 128 && pixel[2] < 128) { ++counts[1]; }
                }
            }
            return counts;
        };
        const auto unchecked = paint(0), checked = paint(ODS_CHECKED);
        SelectObject(dc, old_bitmap);
        DeleteDC(dc);
        CHECK(unchecked[0] > 0);
        CHECK(checked[0] == 0);
        CHECK(checked[1] > 0);
    }
    item = MENUITEMINFOW{sizeof(item)};
    item.fMask = MIIM_BITMAP;
    REQUIRE(GetMenuItemInfoW(fixture.root, 1, TRUE, &item));
    CHECK(item.hbmpItem == borrowed.value);
    BITMAP metadata{};
    constexpr int metadata_size = static_cast<int>(sizeof(metadata));
    CHECK(GetObjectW(borrowed.value, metadata_size, &metadata) == metadata_size);
    REQUIRE(DestroyMenu(fixture.root));
    fixture.root = nullptr;
    CHECK(GetObjectW(borrowed.value, metadata_size, &metadata) == metadata_size);
}
