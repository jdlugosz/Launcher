#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/menu_icons.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>

namespace {

constexpr std::array icon_kinds{launcher::MenuIcon::edit, launcher::MenuIcon::folder,
    launcher::MenuIcon::information, launcher::MenuIcon::quit, launcher::MenuIcon::shield};

class WicFixture final {
public:
    WicFixture()
    {
        apartment_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        REQUIRE(SUCCEEDED(apartment_));
        REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf()))));
    }

    ~WicFixture()
    {
        factory.Reset();
        if (SUCCEEDED(apartment_)) { CoUninitialize(); }
    }

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;

private:
    HRESULT apartment_ = E_FAIL;
};

struct OwnedMenu final {
    HMENU value = CreatePopupMenu();
    ~OwnedMenu() { if (value) { DestroyMenu(value); } }
};

DIBSECTION bitmap_section(HBITMAP bitmap, UINT dpi)
{
    REQUIRE(bitmap != nullptr);
    DIBSECTION section{};
    constexpr int section_size = static_cast<int>(sizeof(section));
    REQUIRE(GetObjectW(bitmap, section_size, &section) == section_size);
    REQUIRE(section.dsBm.bmBits != nullptr);
    CHECK(section.dsBm.bmPlanes == 1);
    CHECK(section.dsBm.bmBitsPixel == 32);
    CHECK(section.dsBm.bmWidth == GetSystemMetricsForDpi(SM_CXSMICON, dpi));
    CHECK(section.dsBm.bmHeight == GetSystemMetricsForDpi(SM_CYSMICON, dpi));
    CHECK(std::abs(section.dsBmih.biHeight) == section.dsBm.bmHeight);
    return section;
}

} // namespace

TEST_CASE("Menu icons provide transparent premultiplied native bitmaps", "[menu-icons]")
{
    WicFixture fixture;
    launcher::MenuIcons icons;
    icons.initialize(fixture.factory.Get());
    for (const auto kind : icon_kinds) {
        CAPTURE(static_cast<int>(kind));
        const auto section = bitmap_section(icons.get(kind, 96), 96);
        GdiFlush();
        unsigned transparent = 0, visible = 0, invalid = 0;
        const auto* pixels = static_cast<const BYTE*>(section.dsBm.bmBits);
        for (LONG y = 0; y < section.dsBm.bmHeight; ++y) {
            for (LONG x = 0; x < section.dsBm.bmWidth; ++x) {
                const auto* pixel = pixels + y * section.dsBm.bmWidthBytes + x * 4;
                const auto alpha = pixel[3];
                if (alpha == 0) { ++transparent; }
                else { ++visible; }
                if (pixel[0] > alpha || pixel[1] > alpha || pixel[2] > alpha) { ++invalid; }
            }
        }
        CHECK(transparent > 0);
        CHECK(visible > 0);
        CHECK(invalid == 0);
    }
}

TEST_CASE("Menu bitmap cache survives menu destruction and retains each DPI", "[menu-icons]")
{
    WicFixture fixture;
    launcher::MenuIcons icons;
    icons.initialize(fixture.factory.Get());
    std::array<HBITMAP, icon_kinds.size()> originals{};
    {
        OwnedMenu menu;
        REQUIRE(menu.value != nullptr);
        for (std::size_t index = 0; index < icon_kinds.size(); ++index) {
            originals[index] = icons.get(icon_kinds[index], 96);
            REQUIRE(originals[index] != nullptr);
            CHECK(icons.get(icon_kinds[index], 96) == originals[index]);
            REQUIRE(AppendMenuW(menu.value, MF_STRING, index + 1, L"Owned cached menu icon"));
            MENUITEMINFOW item{sizeof(item)};
            item.fMask = MIIM_BITMAP;
            item.hbmpItem = originals[index];
            REQUIRE(SetMenuItemInfoW(menu.value, static_cast<UINT>(index), TRUE, &item));
            item = MENUITEMINFOW{sizeof(item)};
            item.fMask = MIIM_BITMAP | MIIM_FTYPE;
            REQUIRE(GetMenuItemInfoW(menu.value, static_cast<UINT>(index), TRUE, &item));
            CHECK(item.hbmpItem == originals[index]);
            CHECK((item.fType & MFT_OWNERDRAW) == 0);
        }
    }
    for (std::size_t index = 0; index < icon_kinds.size(); ++index) {
        CAPTURE(index);
        CHECK(icons.get(icon_kinds[index], 96) == originals[index]);
        bitmap_section(originals[index], 96);
        const auto larger = icons.get(icon_kinds[index], 144);
        REQUIRE(larger != nullptr);
        CHECK(larger != originals[index]);
        bitmap_section(larger, 144);
        CHECK(icons.get(icon_kinds[index], 144) == larger);
        CHECK(icons.get(icon_kinds[index], 96) == originals[index]);
        bitmap_section(originals[index], 96);
    }
}
