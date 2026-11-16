#pragma once

#include <windows.h>
#include <dwmapi.h>
#include <gdiplus.h>

namespace launcher::drop {

inline COLORREF accent_tint(COLORREF accent, unsigned strength) {
    const auto channel = [strength](unsigned value) { return static_cast<BYTE>((value * strength + 255 * (255 - strength) + 127) / 255); };
    return RGB(channel(GetRValue(accent)), channel(GetGValue(accent)), channel(GetBValue(accent)));
}

inline void paint_drop_zone(HDC dc, const RECT& bounds, UINT dpi, HICON launcher, HFONT title_font, HFONT text_font, bool active) {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    const bool high_contrast = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    DWORD color = 0;
    BOOL opaque = FALSE;
    COLORREF accent = GetSysColor(COLOR_HIGHLIGHT);
    if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque))) { accent = RGB((color >> 16) & 255, (color >> 8) & 255, color & 255); }
    const COLORREF fill = high_contrast ? GetSysColor(COLOR_WINDOW) : accent_tint(accent, active ? 64 : 30);
    const COLORREF edge = high_contrast ? GetSysColor(COLOR_WINDOWTEXT) : accent_tint(accent, active ? 255 : 115);
    const auto rgba = [](COLORREF rgb) { return Gdiplus::Color(255, GetRValue(rgb), GetGValue(rgb), GetBValue(rgb)); };
    const float scale = static_cast<float>(dpi) / 96.0f;
    FillRect(dc, &bounds, GetSysColorBrush(COLOR_BTNFACE));
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath outline;
    const float inset = 1.0f * scale, diameter = 18.0f * scale;
    const float left = static_cast<float>(bounds.left) + inset, top = static_cast<float>(bounds.top) + inset;
    const float right = static_cast<float>(bounds.right) - inset, bottom = static_cast<float>(bounds.bottom) - inset;
    outline.AddArc(left, top, diameter, diameter, 180, 90);
    outline.AddArc(right - diameter, top, diameter, diameter, 270, 90);
    outline.AddArc(right - diameter, bottom - diameter, diameter, diameter, 0, 90);
    outline.AddArc(left, bottom - diameter, diameter, diameter, 90, 90);
    outline.CloseFigure();
    Gdiplus::SolidBrush background(rgba(fill));
    Gdiplus::Pen border(rgba(edge), (active ? 2.0f : 1.0f) * scale);
    graphics.FillPath(&background, &outline);
    graphics.DrawPath(&border, &outline);

    // Only the target rings belong in the zone; the paddle is the program icon.
    const float center_x = right - 64.0f * scale, center_y = (top + bottom) / 2;
    const auto target_color = high_contrast ? rgba(GetSysColor(COLOR_WINDOWTEXT)) : Gdiplus::Color(255, 188, 22, 58);
    Gdiplus::Pen rings(target_color, 4.0f * scale);
    for (const float radius : {36.0f * scale, 23.0f * scale}) {
        graphics.DrawEllipse(&rings, center_x - radius, center_y - radius, radius * 2, radius * 2);
    }
    Gdiplus::SolidBrush center(target_color);
    graphics.FillEllipse(&center, center_x - 10 * scale, center_y - 10 * scale, 20 * scale, 20 * scale);
    const HDC native = graphics.GetHDC();
    if (!native) { return; }

    const int icon_size = MulDiv(96, dpi, 96);
    if (launcher) { DrawIconEx(native, bounds.left + MulDiv(16, dpi, 96), (bounds.top + bounds.bottom - icon_size) / 2, launcher, icon_size, icon_size, 0, nullptr, DI_NORMAL); }
    const int saved = SaveDC(native);
    SetBkMode(native, TRANSPARENT);
    SetTextColor(native, high_contrast ? GetSysColor(COLOR_WINDOWTEXT) : RGB(28, 32, 38));
    RECT title{bounds.left + MulDiv(126, dpi, 96), bounds.top + MulDiv(30, dpi, 96), bounds.right - MulDiv(126, dpi, 96), bounds.top + MulDiv(62, dpi, 96)};
    SelectObject(native, title_font);
    DrawTextW(native, active ? L"Release to add" : L"Drop here", -1, &title, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT description{title.left, bounds.top + MulDiv(69, dpi, 96), title.right, bounds.bottom - MulDiv(14, dpi, 96)};
    SelectObject(native, text_font);
    DrawTextW(native, L"Programs, documents, folders, or shortcuts", -1, &description, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    if (saved) { RestoreDC(native, saved); }
    graphics.ReleaseHDC(native);
}

} // namespace launcher::drop
