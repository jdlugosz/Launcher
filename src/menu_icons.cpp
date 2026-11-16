#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/menu_icons.hpp"
#include "launcher/icon_images.hpp"
#include "launcher/svg_images.hpp"
#include "launcher/config_error.hpp"

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <type_traits>

namespace launcher {
namespace {

using Microsoft::WRL::ComPtr;

void check(HRESULT result, const char* operation)
{
    if (FAILED(result)) { throw std::system_error(static_cast<int>(result), std::system_category(), operation); }
}

struct BitmapCloser final {
    void operator()(HBITMAP bitmap) const { DeleteObject(bitmap); }
};
using Bitmap = std::unique_ptr<std::remove_pointer_t<HBITMAP>, BitmapCloser>;

std::filesystem::path notepad_path()
{
    std::wstring directory(MAX_PATH, L'\0');
    UINT length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    if (length >= directory.size()) {
        directory.resize(static_cast<std::size_t>(length) + 1);
        length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    }
    if (length == 0 || length >= directory.size()) { return {}; }
    directory.resize(length);
    return std::filesystem::path(directory) / L"notepad.exe";
}

} // namespace

struct MenuIcons::State {
    ComPtr<IWICImagingFactory> factory;
    IconImages images;
    std::map<UINT, std::array<Bitmap, 5>> bitmaps;

    explicit State(IWICImagingFactory* imaging) : factory(imaging)
    {
        check(imaging ? S_OK : E_INVALIDARG, "Menu icon factory");
        images.initialize(imaging);
    }

    ComPtr<IWICBitmapSource> image(MenuIcon icon, UINT size)
    {
        ComPtr<IWICBitmapSource> result;
        switch (icon) {
        case MenuIcon::edit:
            try {
                const auto path = notepad_path();
                if (!path.empty()) { images.load(IconGroup{path, IconIndex{0}}, size, result); }
            } catch (const ConfigError&) {}
            catch (const std::system_error&) {}
            if (!result) { images.load(StockIcon::document, size, result); }
            break;
        case MenuIcon::folder: images.load(StockIcon::folder, size, result); break;
        case MenuIcon::information: images.load(StockIcon::information, size, result); break;
        case MenuIcon::shield: images.load(StockIcon::shield, size, result); break;
        case MenuIcon::quit:
            check(render_svg_data(factory.Get(),
                R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16"><path d="M3 3L13 13M13 3L3 13" fill="none" stroke="#d72626" stroke-width="2.5" stroke-linecap="round"/></svg>)",
                size, result), "Render Quit menu icon");
            break;
        }
        return result;
    }

    Bitmap bitmap(MenuIcon icon, UINT size)
    {
        const auto source = image(icon, size);
        UINT width = 0, height = 0;
        check(source->GetSize(&width, &height), "Menu icon dimensions");
        check(width == size && height == size ? S_OK : E_UNEXPECTED, "Menu icon size");
        BITMAPINFO information{};
        information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        information.bmiHeader.biWidth = static_cast<LONG>(size);
        information.bmiHeader.biHeight = -static_cast<LONG>(size);
        information.bmiHeader.biPlanes = 1;
        information.bmiHeader.biBitCount = 32;
        information.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        Bitmap result(CreateDIBSection(nullptr, &information, DIB_RGB_COLORS, &pixels, nullptr, 0));
        if (!result || !pixels) {
            const DWORD error = GetLastError();
            check(error ? HRESULT_FROM_WIN32(error) : E_OUTOFMEMORY, "Create menu icon bitmap");
        }
        // Both loaders return premultiplied BGRA at the requested physical size.
        check(source->CopyPixels(nullptr, size * 4, size * size * 4, static_cast<BYTE*>(pixels)), "Copy menu icon pixels");
        return result;
    }
};

MenuIcons::MenuIcons() = default;
MenuIcons::~MenuIcons() = default;

void MenuIcons::initialize(IWICImagingFactory* factory)
{
    state_ = std::make_unique<State>(factory);
}

HBITMAP MenuIcons::get(MenuIcon icon, UINT dpi)
{
    const auto index = static_cast<std::size_t>(icon);
    check(state_ && index < 5 ? S_OK : E_INVALIDARG, "Menu icon request");
    dpi = dpi ? dpi : 96;
    int dimension = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    if (dimension <= 0) { dimension = MulDiv(16, static_cast<int>(dpi), 96); }
    check(dimension > 0 && dimension <= 4096 ? S_OK : E_INVALIDARG, "Menu icon dimensions");
    const auto size = static_cast<UINT>(dimension);
    auto& bitmap = state_->bitmaps[size][index];
    if (!bitmap) { bitmap = state_->bitmap(icon, size); }
    return bitmap.get();
}

} // namespace launcher
