#pragma once

#include "launcher/windows_pch.hpp"

#include <memory>

namespace launcher {

enum class MenuIcon { edit, folder, information, quit, shield };

// Borrowed bitmaps are cached at their physical menu size and survive popup
// destruction. Keep this object alive while any menu uses one of its bitmaps.
class MenuIcons final {
public:
    MenuIcons();
    ~MenuIcons();

    MenuIcons(const MenuIcons&) = delete;
    MenuIcons& operator=(const MenuIcons&) = delete;

    void initialize(IWICImagingFactory* factory);
    HBITMAP get(MenuIcon icon, UINT dpi);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace launcher
