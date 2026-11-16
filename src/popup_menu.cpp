#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/popup_menu.hpp"

#include <oleacc.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace launcher {
namespace {

struct SavedDC final {
    HDC dc;
    int saved = SaveDC(dc);
    ~SavedDC() { if (saved) { RestoreDC(dc, saved); } }
};

struct WindowDC final {
    HWND window;
    HDC value = GetDC(window);
    ~WindowDC() { if (value) { ReleaseDC(window, value); } }
};

std::pair<std::wstring_view, std::wstring_view> columns(const std::wstring& text)
{
    const std::wstring_view view{text};
    const auto tab = view.find(L'\t');
    return tab == view.npos ? std::pair{view, std::wstring_view{}} : std::pair{view.substr(0, tab), view.substr(tab + 1)};
}

bool mnemonic(std::wstring_view text, wchar_t key)
{
    CharUpperBuffW(&key, 1);
    for (std::size_t index = 0; index + 1 < text.size(); ++index) {
        if (text[index] != L'&') { continue; }
        wchar_t letter = text[++index];
        if (letter == L'&') { continue; }
        CharUpperBuffW(&letter, 1);
        if (letter == key) { return true; }
    }
    return false;
}

bool initial(std::wstring_view text, wchar_t key)
{
    const auto first = text.find_first_not_of(L" \t");
    if (first == text.npos) { return false; }
    text.remove_prefix(first);
    if (text.front() == L'&') {
        text.remove_prefix(1);
        if (text.empty()) { return false; }
    }
    wchar_t letter = text.front();
    CharUpperBuffW(&letter, 1); CharUpperBuffW(&key, 1);
    return letter == key;
}

} // namespace

struct PopupMenuDrawing::State {
    struct Row {
        MSAAMENUINFO accessibility{};
        HMENU menu = nullptr;
        UINT original_type = 0;
        ULONG_PTR original_data = 0;
        HMENU submenu = nullptr;
        HBITMAP bitmap = nullptr; // Borrowed from the caller's persistent image cache.
        std::wstring text;
        bool heading = false;
        SIZE label{}, shortcut{}, bitmap_size{};
        UINT height = 0;
    };
    struct Layout { int width = 0, gutter = 0; };

    HWND owner = nullptr;
    HMENU excluded = nullptr;
    UINT dpi = 96;
    ULONG_PTR heading_tag = 0;
    HTHEME theme = nullptr;
    HFONT normal_font = nullptr, bold_font = nullptr;
    int item_part = MENU_POPUPITEM;
    MARGINS item_margin{}, check_margin{}, check_background_margin{}, separator_margin{}, arrow_margin{};
    SIZE check_size{}, arrow_size{}, separator_size{};
    int gutter = 0, text_gap = 0, row_height = 0;
    bool subclassed = false;
    std::unordered_map<HMENU, Layout> menus;
    std::unordered_map<ULONG_PTR, std::unique_ptr<Row>> rows;

    ~State()
    {
        if (subclassed && owner) { RemoveWindowSubclass(owner, callback, reinterpret_cast<UINT_PTR>(this)); }
        // A lazy page may have removed or replaced rows while tracking. Restore
        // only live items that still carry one of our exact ownership tokens.
        for (const auto& [menu, layout] : menus) {
            const int count = GetMenuItemCount(menu);
            for (int position = 0; position < count; ++position) {
                MENUITEMINFOW item{sizeof(item)};
                item.fMask = MIIM_DATA;
                if (!GetMenuItemInfoW(menu, position, TRUE, &item)) { continue; }
                const auto found = rows.find(item.dwItemData);
                if (found == rows.end()) { continue; }
                restore_row(menu, static_cast<UINT>(position), *found->second);
            }
        }
        if (theme) { CloseThemeData(theme); }
        if (bold_font) { DeleteObject(bold_font); }
        if (normal_font) { DeleteObject(normal_font); }
    }

    int scale(int value) const { return MulDiv(value, static_cast<int>(dpi), 96); }
    HFONT font(bool bold) const { return (bold && bold_font) ? bold_font : normal_font ? normal_font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)); }

    static void restore_row(HMENU menu, UINT position, Row& row)
    {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_STRING;
        item.fType = row.original_type;
        item.dwItemData = row.original_data;
        item.dwTypeData = row.text.data();
        SetMenuItemInfoW(menu, position, TRUE, &item);
    }

    void initialize(HWND window, HMENU menu, UINT resolution, ULONG_PTR tag, HMENU native)
    {
        owner = window; excluded = native; dpi = resolution ? resolution : 96; heading_tag = tag;
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
            normal_font = CreateFontIndirectW(&metrics.lfMenuFont);
            metrics.lfMenuFont.lfWeight = FW_BOLD;
            bold_font = CreateFontIndirectW(&metrics.lfMenuFont);
        }
        HIGHCONTRASTW contrast{sizeof(contrast)};
        SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
        if (!(contrast.dwFlags & HCF_HIGHCONTRASTON) && (GetThemeAppProperties() & STAP_ALLOW_CONTROLS) &&
                IsAppThemed() && IsThemeActive()) {
            theme = OpenThemeDataForDpi(owner, L"Menu", dpi);
        }
        check_size = {GetSystemMetricsForDpi(SM_CXMENUCHECK, dpi), GetSystemMetricsForDpi(SM_CYMENUCHECK, dpi)};
        arrow_size = {scale(6), scale(10)};
        separator_size = {1, scale(1)};
        text_gap = scale(4);
        row_height = GetSystemMetricsForDpi(SM_CYMENU, dpi);
        if (theme) {
            // Windows 11 supplies the current gray selection; older themes use
            // the original popup item part and its own selection palette.
            if (IsThemePartDefined(theme, MENU_POPUPITEMFOCUSABLE, 0)) { item_part = MENU_POPUPITEMFOCUSABLE; }
            GetThemeMargins(theme, nullptr, item_part, 0, TMT_CONTENTMARGINS, nullptr, &item_margin);
            GetThemeMargins(theme, nullptr, MENU_POPUPCHECK, 0, TMT_CONTENTMARGINS, nullptr, &check_margin);
            GetThemeMargins(theme, nullptr, MENU_POPUPCHECKBACKGROUND, 0, TMT_CONTENTMARGINS, nullptr, &check_background_margin);
            GetThemeMargins(theme, nullptr, MENU_POPUPSEPARATOR, 0, TMT_SIZINGMARGINS, nullptr, &separator_margin);
            GetThemeMargins(theme, nullptr, MENU_POPUPSUBMENU, 0, TMT_CONTENTMARGINS, nullptr, &arrow_margin);
            GetThemePartSize(theme, nullptr, MENU_POPUPCHECK, 0, nullptr, TS_TRUE, &check_size);
            GetThemePartSize(theme, nullptr, MENU_POPUPSUBMENU, 0, nullptr, TS_TRUE, &arrow_size);
            GetThemePartSize(theme, nullptr, MENU_POPUPSEPARATOR, 0, nullptr, TS_TRUE, &separator_size);
        }
        gutter = item_margin.cxLeftWidth + check_background_margin.cxLeftWidth + check_margin.cxLeftWidth + check_size.cx +
            check_margin.cxRightWidth + check_background_margin.cxRightWidth;
        gutter = std::max(gutter, static_cast<int>(check_size.cx) + scale(8));
        if (!SetWindowSubclass(owner, callback, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this))) {
            throw std::runtime_error("Cannot prepare popup menu drawing.");
        }
        subclassed = true;
        adopt(menu);
    }

    void adopt(HMENU menu)
    {
        if (!menu || menu == excluded) { return; }
        auto& layout = menus.try_emplace(menu).first->second;
        int label_width = 0, shortcut_width = 0, bitmap_width = 0;
        const WindowDC acquired{owner};
        const HDC dc = acquired.value;
        const int count = GetMenuItemCount(menu);
        for (int position = 0; position < count; ++position) {
            MENUITEMINFOW item{sizeof(item)};
            item.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_STRING | MIIM_STATE | MIIM_SUBMENU | MIIM_BITMAP;
            if (!GetMenuItemInfoW(menu, position, TRUE, &item)) { continue; }
            auto existing = rows.find(item.dwItemData);
            Row* row = existing == rows.end() ? nullptr : existing->second.get();
            BITMAP bitmap{};
            constexpr int bitmap_bytes = static_cast<int>(sizeof(bitmap));
            if (item.hbmpItem && (GetObjectW(item.hbmpItem, bitmap_bytes, &bitmap) != bitmap_bytes ||
                    bitmap.bmWidth <= 0 || bitmap.bmHeight <= 0 || bitmap.bmPlanes != 1 || bitmap.bmBitsPixel != 32)) {
                // Callback and system pseudo-handles belong to native drawing.
                // A caller may also replace a previously adopted bitmap later.
                if (row) { restore_row(menu, static_cast<UINT>(position), *row); }
                continue;
            }
            if (!row) {
                // Leave any unrelated owner-drawn item with its existing owner.
                if ((item.fType & MFT_OWNERDRAW) && !(heading_tag && item.dwItemData == heading_tag)) { continue; }
                auto record = std::make_unique<Row>();
                record->menu = menu; record->original_type = item.fType; record->original_data = item.dwItemData;
                record->submenu = item.hSubMenu; record->heading = heading_tag && item.dwItemData == heading_tag;
                record->text.resize(static_cast<std::size_t>(item.cch) + 1);
                item.dwTypeData = record->text.data(); item.cch = static_cast<UINT>(record->text.size());
                if (!GetMenuItemInfoW(menu, position, TRUE, &item)) { continue; }
                record->text.resize(item.cch);
                record->accessibility = {MSAA_MENU_SIG, static_cast<DWORD>(record->text.size()), record->text.data()};
                const auto token = reinterpret_cast<ULONG_PTR>(&record->accessibility);
                row = rows.emplace(token, std::move(record)).first->second.get();
                item.fMask = MIIM_FTYPE | MIIM_DATA;
                item.fType = row->original_type | MFT_OWNERDRAW;
                item.dwItemData = token;
                if (!SetMenuItemInfoW(menu, position, TRUE, &item)) { rows.erase(token); continue; }
            }
            row->bitmap = item.hbmpItem;
            row->bitmap_size = {bitmap.bmWidth, bitmap.bmHeight};
            if (dc) {
                SavedDC saved{dc};
                if (saved.saved) {
                    SelectObject(dc, font(row->heading || (item.fState & MFS_DEFAULT)));
                    const auto [label, shortcut] = columns(row->text);
                    RECT extent{};
                    DrawTextW(dc, label.data(), static_cast<int>(label.size()), &extent, DT_SINGLELINE | DT_CALCRECT);
                    row->label = {extent.right, extent.bottom};
                    extent = {};
                    DrawTextW(dc, shortcut.data(), static_cast<int>(shortcut.size()), &extent, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
                    row->shortcut = {extent.right, extent.bottom};
                }
            }
            row->height = static_cast<UINT>((row->original_type & MFT_SEPARATOR)
                ? std::max(scale(6), static_cast<int>(separator_size.cy) + separator_margin.cyTopHeight + separator_margin.cyBottomHeight)
                : std::max({row_height, static_cast<int>(row->label.cy) + scale(4), static_cast<int>(row->bitmap_size.cy) + scale(4),
                    static_cast<int>(check_size.cy) + check_margin.cyTopHeight + check_margin.cyBottomHeight}));
            label_width = std::max(label_width, static_cast<int>(row->label.cx));
            shortcut_width = std::max(shortcut_width, static_cast<int>(row->shortcut.cx));
            bitmap_width = std::max(bitmap_width, static_cast<int>(row->bitmap_size.cx));
            if (row->submenu) { adopt(row->submenu); }
        }
        layout.gutter = std::max(gutter, bitmap_width + scale(8));
        layout.width = layout.gutter + text_gap + label_width + (shortcut_width ? scale(24) + shortcut_width : 0) +
            scale(12) + arrow_size.cx + arrow_margin.cxRightWidth;
    }

    Row* find(ULONG_PTR token) const
    {
        const auto row = rows.find(token);
        return row == rows.end() ? nullptr : row->second.get();
    }

    void draw(const DRAWITEMSTRUCT& item, const Row& row) const
    {
        const bool disabled = (item.itemState & (ODS_DISABLED | ODS_GRAYED)) != 0 && !row.heading;
        const bool selected = (item.itemState & ODS_SELECTED) != 0 && !row.heading;
        const int state = disabled ? (selected ? MPI_DISABLEDHOT : MPI_DISABLED) : selected ? MPI_HOT : MPI_NORMAL;
        const int row_gutter = menus.at(row.menu).gutter;
        RECT content = item.rcItem;
        content.left += row_gutter + text_gap;
        content.right -= scale(12) + arrow_size.cx + arrow_margin.cxRightWidth;
        RECT arrow{content.right + scale(8), (item.rcItem.top + item.rcItem.bottom - arrow_size.cy) / 2, 0, 0};
        arrow.right = arrow.left + arrow_size.cx; arrow.bottom = arrow.top + arrow_size.cy;
        {
            SavedDC saved{item.hDC};
            if (!saved.saved) { return; }
            SetBkMode(item.hDC, TRANSPARENT);
            const bool themed = theme && SUCCEEDED(DrawThemeBackground(theme, item.hDC, MENU_POPUPBACKGROUND, 0, &item.rcItem, nullptr));
            if (themed) {
                RECT strip = item.rcItem; strip.right = strip.left + row_gutter;
                DrawThemeBackground(theme, item.hDC, MENU_POPUPGUTTER, 0, &strip, nullptr);
                if (selected) { DrawThemeBackground(theme, item.hDC, item_part, state, &item.rcItem, nullptr); }
            } else { FillRect(item.hDC, &item.rcItem, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_MENU)); }
            if (row.original_type & MFT_SEPARATOR) {
                RECT separator = content;
                separator.top = (separator.top + separator.bottom - separator_size.cy) / 2;
                separator.bottom = separator.top + separator_size.cy;
                if (themed) { DrawThemeBackground(theme, item.hDC, MENU_POPUPSEPARATOR, 0, &separator, nullptr); }
                else { DrawEdge(item.hDC, &separator, EDGE_ETCHED, BF_TOP); }
                return;
            }
            const bool bold = row.heading || (item.itemState & ODS_DEFAULT);
            SelectObject(item.hDC, font(bold));
            COLORREF color = GetSysColor(disabled ? COLOR_GRAYTEXT : selected ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT);
            if (themed && !row.heading) { GetThemeColor(theme, item_part, state, TMT_TEXTCOLOR, &color); }
            SetTextColor(item.hDC, color);
            DWORD flags = DT_SINGLELINE | DT_VCENTER;
            if (row.heading) { flags |= DT_NOPREFIX; }
            else if (item.itemState & ODS_NOACCEL) { flags |= DT_HIDEPREFIX; }
            const auto [label, shortcut] = columns(row.text);
            const auto text = [&](std::wstring_view value, DWORD format) {
                DTTOPTS options{sizeof(options)}; options.dwFlags = DTT_TEXTCOLOR; options.crText = color;
                if (!themed || FAILED(DrawThemeTextEx(theme, item.hDC, bold ? 0 : item_part, bold ? 0 : state,
                        value.data(), static_cast<int>(value.size()), format, &content, &options))) {
                    DrawTextW(item.hDC, value.data(), static_cast<int>(value.size()), &content, format);
                }
            };
            text(label, flags);
            if (!shortcut.empty()) { text(shortcut, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX); }
            if (item.itemState & ODS_CHECKED) {
                RECT check{item.rcItem.left + (row_gutter - check_size.cx) / 2,
                    (item.rcItem.top + item.rcItem.bottom - check_size.cy) / 2, 0, 0};
                check.right = check.left + check_size.cx; check.bottom = check.top + check_size.cy;
                if (themed) {
                    RECT background{check.left - check_margin.cxLeftWidth, check.top - check_margin.cyTopHeight,
                        check.right + check_margin.cxRightWidth, check.bottom + check_margin.cyBottomHeight};
                    DrawThemeBackground(theme, item.hDC, MENU_POPUPCHECKBACKGROUND, disabled ? MCB_DISABLED : MCB_NORMAL, &background, nullptr);
                    const bool radio = (row.original_type & MFT_RADIOCHECK) != 0;
                    DrawThemeBackground(theme, item.hDC, MENU_POPUPCHECK, radio ? (disabled ? MC_BULLETDISABLED : MC_BULLETNORMAL)
                        : (disabled ? MC_CHECKMARKDISABLED : MC_CHECKMARKNORMAL), &check, nullptr);
                } else { DrawFrameControl(item.hDC, &check, DFC_MENU, (row.original_type & MFT_RADIOCHECK ? DFCS_MENUBULLET : DFCS_MENUCHECK) | (disabled ? DFCS_INACTIVE : 0)); }
            } else if (row.bitmap) {
                const HDC source = CreateCompatibleDC(item.hDC);
                if (source) {
                    const auto previous = SelectObject(source, row.bitmap);
                    if (previous && previous != HGDI_ERROR) {
                        const BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(disabled ? 128 : 255), AC_SRC_ALPHA};
                        AlphaBlend(item.hDC, item.rcItem.left + (row_gutter - row.bitmap_size.cx) / 2,
                            (item.rcItem.top + item.rcItem.bottom - row.bitmap_size.cy) / 2,
                            row.bitmap_size.cx, row.bitmap_size.cy, source, 0, 0, row.bitmap_size.cx, row.bitmap_size.cy, blend);
                        SelectObject(source, previous);
                    }
                    DeleteDC(source);
                }
            }
            if (row.submenu) {
                if (themed) { DrawThemeBackground(theme, item.hDC, MENU_POPUPSUBMENU, disabled ? MSM_DISABLED : MSM_NORMAL, &arrow, nullptr); }
                else { DrawFrameControl(item.hDC, &arrow, DFC_MENU, DFCS_MENUARROW | (disabled ? DFCS_INACTIVE : 0)); }
            }
        }
        // USER draws a classic submenu arrow after WM_DRAWITEM unless clipped.
        if (row.submenu) { ExcludeClipRect(item.hDC, arrow.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom); }
    }

    LRESULT menu_character(HMENU menu, wchar_t key) const
    {
        std::vector<UINT> matches, initials;
        int selected = -1;
        const int count = GetMenuItemCount(menu);
        for (int position = 0; position < count; ++position) {
            MENUITEMINFOW item{sizeof(item)};
            item.fMask = MIIM_DATA | MIIM_FTYPE | MIIM_STATE;
            if (!GetMenuItemInfoW(menu, position, TRUE, &item)) { continue; }
            if (item.fState & MFS_HILITE) { selected = position; }
            const auto* row = find(item.dwItemData);
            if (row && !row->heading && !(item.fState & MFS_DISABLED) && !(item.fType & MFT_SEPARATOR)) {
                const auto label = columns(row->text).first;
                if (mnemonic(label, key)) { matches.push_back(static_cast<UINT>(position)); }
                if (initial(label, key)) { initials.push_back(static_cast<UINT>(position)); }
            }
        }
        if (matches.empty()) { matches = std::move(initials); }
        if (matches.empty()) { return MAKELRESULT(0, MNC_IGNORE); }
        if (matches.size() == 1) { return MAKELRESULT(matches.front(), MNC_EXECUTE); }
        const auto next = std::find_if(matches.begin(), matches.end(), [selected](UINT position) { return static_cast<int>(position) > selected; });
        return MAKELRESULT(next == matches.end() ? matches.front() : *next, MNC_SELECT);
    }

    static LRESULT CALLBACK callback(HWND window, UINT message, WPARAM word, LPARAM parameter, UINT_PTR id, DWORD_PTR data) noexcept
    {
        auto& self = *reinterpret_cast<State*>(data);
        if (message == WM_NCDESTROY) {
            self.owner = nullptr; self.subclassed = false;
            RemoveWindowSubclass(window, callback, id);
            return DefSubclassProc(window, message, word, parameter);
        }
        if (message == WM_INITMENUPOPUP) {
            const auto result = DefSubclassProc(window, message, word, parameter);
            const auto menu = reinterpret_cast<HMENU>(word);
            if (self.menus.contains(menu)) { try { self.adopt(menu); } catch (...) {} }
            return result;
        }
        if (message == WM_MEASUREITEM) {
            auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(parameter);
            if (item && item->CtlType == ODT_MENU) {
                if (const auto* row = self.find(item->itemData)) {
                    item->itemWidth = static_cast<UINT>(self.menus.at(row->menu).width); item->itemHeight = row->height;
                    return TRUE;
                }
            }
        } else if (message == WM_DRAWITEM) {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(parameter);
            if (item && item->CtlType == ODT_MENU) {
                if (const auto* row = self.find(item->itemData)) { self.draw(*item, *row); return TRUE; }
            }
        } else if (message == WM_MENUCHAR && self.menus.contains(reinterpret_cast<HMENU>(parameter))) {
            try { return self.menu_character(reinterpret_cast<HMENU>(parameter), LOWORD(word)); } catch (...) {}
        }
        return DefSubclassProc(window, message, word, parameter);
    }
};

PopupMenuDrawing::PopupMenuDrawing(HWND owner, HMENU menu, UINT dpi, ULONG_PTR heading_tag, HMENU native_submenu)
    : state_(std::make_unique<State>())
{
    state_->initialize(owner, menu, dpi, heading_tag, native_submenu);
}

PopupMenuDrawing::~PopupMenuDrawing() = default;

} // namespace launcher
