#pragma once

#include "launcher/windows_pch.hpp"

#include <string>
#include <span>
#include <string_view>

namespace launcher {

// Rasterize directly at the final physical size. The result joins the ordinary
// WIC image cache; vector details stay out of window placement and drawing.
HRESULT render_svg_image(IWICImagingFactory* factory, const std::wstring& path, UINT size, Microsoft::WRL::ComPtr<IWICBitmapSource>& output);
bool is_svg_image(std::span<const BYTE> bytes);
HRESULT render_svg_data(IWICImagingFactory* factory, std::string_view xml, UINT size, Microsoft::WRL::ComPtr<IWICBitmapSource>& output);

} // namespace launcher
