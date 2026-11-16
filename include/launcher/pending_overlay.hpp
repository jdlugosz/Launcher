#pragma once

#include "launcher/windows_pch.hpp"

namespace launcher {

// Density follows the drawn DIP extent, independently of monitor DPI. These
// static SVG resources are provisional artwork and never require a frame timer.
unsigned pending_overlay_slats(float logical_icon_extent) noexcept;
HRESULT render_pending_overlay(IWICImagingFactory* factory, float logical_icon_extent, UINT physical_size, Microsoft::WRL::ComPtr<IWICBitmapSource>& output);

} // namespace launcher
