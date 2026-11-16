#pragma once

#include "launcher/config.hpp"

#include <cstdint>

namespace launcher {

struct PixelRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    bool operator==(const PixelRect&) const = default;
};

struct DockPlacement {
    PixelRect expanded;
    PixelRect collapsed;
    PixelRect collapsed_tabs;
};

// parallel_span follows the chosen edge. panel_depth excludes tab_depth.
// collapsed is the shifted full drawing canvas. The native collapsed HWND
// uses collapsed_tabs as its compact viewport, retaining its monitor's DPI.
// Throws ConfigError for nonpositive sizes, insufficient space, or overflow.
DockPlacement place_dock(Edge edge, PixelRect work_area,
                         int parallel_span, int panel_depth, int tab_depth,
                         double position = 0.5);

// Move the full canvas, clipping each intermediate viewport to this monitor.
PixelRect dock_canvas(const DockPlacement& placement, double expansion) noexcept;
PixelRect dock_viewport(PixelRect canvas, PixelRect work_area, int padding = 0) noexcept;

class DockAnimation {
public:
    void reset(bool expanded) noexcept;
    void start(bool expanded, std::uint32_t duration_ms, std::uint64_t now) noexcept;
    double value(std::uint64_t now) const noexcept;
    bool running(std::uint64_t now) const noexcept;

private:
    double from_ = 0;
    double to_ = 0;
    std::uint64_t started_ = 0;
    double duration_ = 0;
};

struct WindowTransparency {
    bool layered = false;
    bool click_through = false;
    bool attributes_known = false;
    bool color_key = false;
    bool alpha_blend = false;
    std::uint8_t alpha = 255;
};

// Unknown layered attributes are not evidence of transparency. Alpha matters
// only when the native alpha-blending flag was reported.
bool is_transparent_overlay(const WindowTransparency& window) noexcept;

enum class DockState { collapsed, expanded, suppressed };
enum class Pending { none, reveal, hide };

class PeekController {
public:
    explicit PeekController(bool auto_hide = true) noexcept;

    DockState state() const noexcept;
    Pending pending() const noexcept;
    void pointer_changed(bool inside) noexcept;
    void reveal() noexcept;
    void timer_elapsed() noexcept;
    void set_suppressed(bool suppressed) noexcept;
    void set_menu_open(bool open) noexcept;

private:
    void update_pending() noexcept;

    bool auto_hide_ = true;
    bool pointer_inside_ = false;
    bool menu_open_ = false;
    bool suppressed_ = false;
    DockState state_ = DockState::collapsed;
    Pending pending_ = Pending::none;
};

} // namespace launcher
