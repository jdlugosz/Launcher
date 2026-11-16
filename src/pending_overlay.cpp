#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/pending_overlay.hpp"
#include "launcher/svg_images.hpp"
#include "../resources/pending_overlay_resource.h"

#include <cmath>
#include <string_view>

namespace launcher {

unsigned pending_overlay_slats(float logical_icon_extent) noexcept
{
    if (logical_icon_extent <= 32.0F) { return 4; }
    if (logical_icon_extent <= 64.0F) { return 5; }
    return 6;
}

HRESULT render_pending_overlay(IWICImagingFactory* factory, float logical_icon_extent, UINT physical_size, Microsoft::WRL::ComPtr<IWICBitmapSource>& output)
{
    output.Reset();
    if (!factory || !std::isfinite(logical_icon_extent) || logical_icon_extent <= 0.0F || physical_size == 0 || physical_size > 4096) { return E_INVALIDARG; }
    const auto id = static_cast<WORD>(IDR_PENDING_BLINDS_4 + pending_overlay_slats(logical_icon_extent) - 4);
    // Borrow only this process's embedded artwork: no path, Shell provider or
    // external resource module is consulted during drawing/cache preparation.
    const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(id), L"LAUNCHER_PENDING_SVG");
    if (!resource) { return HRESULT_FROM_WIN32(GetLastError()); }
    const auto size = SizeofResource(nullptr, resource);
    const auto data = LoadResource(nullptr, resource);
    const auto bytes = data ? LockResource(data) : nullptr;
    if (!bytes || size == 0) { return E_FAIL; }
    return render_svg_data(factory, std::string_view(static_cast<const char*>(bytes), size), physical_size, output);
}

} // namespace launcher
