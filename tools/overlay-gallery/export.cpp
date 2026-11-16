#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/icon_images.hpp"
#include "launcher/pending_overlay.hpp"
#include "launcher/png.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

void check(HRESULT result, const char* operation)
{
    if (FAILED(result)) { throw std::system_error(static_cast<int>(result), std::system_category(), operation); }
}

ComPtr<IWICBitmap> composite(IWICImagingFactory* wic, ID2D1Factory* d2d, IWICBitmapSource* base, IWICBitmapSource* overlay, UINT size, float dpi)
{
    ComPtr<IWICBitmap> image;
    check(wic->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, image.GetAddressOf()), "Create gallery image");
    ComPtr<ID2D1RenderTarget> target;
    const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    check(d2d->CreateWicBitmapRenderTarget(image.Get(), properties, target.GetAddressOf()), "Create gallery drawing target");
    ComPtr<ID2D1Bitmap> base_bitmap;
    ComPtr<ID2D1Bitmap> overlay_bitmap;
    check(target->CreateBitmapFromWicBitmap(base, nullptr, base_bitmap.GetAddressOf()), "Create gallery icon bitmap");
    if (overlay) { check(target->CreateBitmapFromWicBitmap(overlay, nullptr, overlay_bitmap.GetAddressOf()), "Create gallery overlay bitmap"); }
    const float logical = static_cast<float>(size) * 96.0F / dpi;
    const auto bounds = D2D1::RectF(0, 0, logical, logical);
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F));
    target->DrawBitmap(base_bitmap.Get(), bounds, 1.0F, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    if (overlay_bitmap) { target->DrawBitmap(overlay_bitmap.Get(), bounds, 1.0F, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR); }
    check(target->EndDraw(), "Draw gallery image");
    return image;
}

void export_gallery(const fs::path& directory)
{
    if (!fs::create_directory(directory)) { throw std::runtime_error("Output directory already exists; choose a new directory."); }
    constexpr std::array sizes{16, 24, 32, 33, 40, 48, 64, 65, 80, 96, 128, 256};
    constexpr std::array scales{100, 200};
    struct Source { const char* name; launcher::StockIcon icon; };
    constexpr std::array sources{Source{"document", launcher::StockIcon::document},
        Source{"folder", launcher::StockIcon::folder}, Source{"world", launcher::StockIcon::world}};
    ComPtr<IWICImagingFactory> wic;
    check(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.GetAddressOf())), "Initialize WIC");
    ComPtr<ID2D1Factory> d2d;
    check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()), "Initialize Direct2D");
    launcher::IconImages images;
    images.initialize(wic.Get());
    std::ofstream manifest(directory / L"manifest.json", std::ios::binary);
    if (!manifest) { throw std::runtime_error("Cannot create gallery manifest."); }
    manifest << "{\n  \"schema_version\": 1,\n"
        "  \"renderer\": \"LauncherImages / IconImages::load / render_pending_overlay / Direct2D DrawBitmap / write_png\",\n"
        "  \"icon_sources\": [\"document\", \"folder\", \"world\"],\n"
        "  \"sizes_dip\": [16,24,32,33,40,48,64,65,80,96,128,256],\n"
        "  \"scales_percent\": [100,200],\n  \"samples\": [\n";
    bool first = true;
    unsigned written = 0;
    for (const auto size : sizes) {
        for (const auto scale : scales) {
            const UINT pixels = static_cast<UINT>(size * scale / 100);
            const float dpi = 96.0F * static_cast<float>(scale) / 100.0F;
            const std::string suffix = std::to_string(size) + "-" + std::to_string(scale);
            ComPtr<IWICBitmapSource> overlay;
            check(launcher::render_pending_overlay(wic.Get(), static_cast<float>(size), pixels, overlay), "Rasterize production blinds");
            const std::string overlay_name = "overlay-" + suffix + ".png";
            check(launcher::write_png(wic.Get(), overlay.Get(), directory / overlay_name, dpi), "Write overlay PNG");
            ++written;
            for (const auto& source : sources) {
                ComPtr<IWICBitmapSource> icon;
                images.load(source.icon, pixels, icon);
                for (const bool pending : {false, true}) {
                    const std::string name = std::string(source.name) + "-" + suffix + (pending ? "-pending.png" : "-ready.png");
                    const auto image = composite(wic.Get(), d2d.Get(), icon.Get(), pending ? overlay.Get() : nullptr, pixels, dpi);
                    check(launcher::write_png(wic.Get(), image.Get(), directory / name, dpi), "Write gallery PNG");
                    ++written;
                }
            }
            if (!first) { manifest << ",\n"; }
            first = false;
            manifest << "    {\"size_dip\":" << size << ",\"scale_percent\":" << scale << ",\"pixels\":" << pixels
                << ",\"slats\":" << launcher::pending_overlay_slats(static_cast<float>(size)) << ",\"overlay\":\"" << overlay_name << "\"}";
        }
    }
    manifest << "\n  ]\n}\n";
    manifest.close();
    if (!manifest) { throw std::runtime_error("Cannot finish gallery manifest."); }
    std::cout << "Exported " << written << " native PNGs and manifest.json.\n";
}
} // namespace

int wmain(int count, wchar_t** arguments)
{
    if (count != 2) { std::cerr << "Usage: LauncherOverlayGallery new-output-directory\n"; return 2; }
    const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(apartment)) { std::cerr << "Cannot initialize the gallery COM apartment.\n"; return 1; }
    int result = 0;
    try { export_gallery(fs::absolute(arguments[1])); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    CoUninitialize();
    return result;
}
