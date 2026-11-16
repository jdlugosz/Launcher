#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "launcher/settings.hpp"

#include <functional>
#include <string>

namespace launcher {

struct AdjustmentCallbacks {
    std::function<bool(const RuntimeAdjustments&, std::wstring&)> preview;
    std::function<bool(const RuntimeAdjustments&, std::wstring&)> commit;
    std::function<std::wstring()> describe;
    std::function<std::uint32_t()> default_icon_spacing_px;
    std::function<void(HWND)> dialog_window;
    std::function<double()> default_thickness_dip;
    std::function<std::vector<AdjustmentPreset>()> load_presets;
    std::function<bool(std::string_view, const RuntimeAdjustments&, std::wstring&)> store_preset;
};

// Preview continuously; Apply/OK commit, and Cancel restores the last Apply.
INT_PTR show_adjustments_dialog(HINSTANCE instance, HWND owner,
    const RuntimeAdjustments& initial, const AdjustmentCallbacks& callbacks);

} // namespace launcher
