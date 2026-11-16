#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace launcher {

struct NativeDesktopIconSize {
    float size_dip = 32.0F;
    int size_px = 32;
    UINT desktop_dpi = 96;
    bool shell_sample = false;
};

// Read once at startup. Queries the desktop Shell view without changing it.
// The Shell view's nominal size is used as DIPs; size_px applies its HWND's DPI.
// Shell/COM failures fall back to the primary desktop's system icon metric.
// That fallback returns physical pixels, which are normalized to DIPs.
NativeDesktopIconSize sample_desktop_icon_size() noexcept;

} // namespace launcher
