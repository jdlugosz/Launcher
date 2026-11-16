#pragma once

#include "launcher/windows_pch.hpp"

#include <memory>

namespace launcher {

// Keep this scope inside the borrowed menu's lifetime and on its owner's thread.
// Shell extensions retain their native drawing and message handling.
class PopupMenuDrawing final {
public:
    PopupMenuDrawing(HWND owner, HMENU menu, UINT dpi, ULONG_PTR heading_tag, HMENU native_submenu = nullptr);
    ~PopupMenuDrawing();

    PopupMenuDrawing(const PopupMenuDrawing&) = delete;
    PopupMenuDrawing& operator=(const PopupMenuDrawing&) = delete;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace launcher
