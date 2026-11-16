#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/dock.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>

namespace launcher {
namespace {

PixelRect checked_rect(std::int64_t left, std::int64_t top,
                       std::int64_t right, std::int64_t bottom) {
    constexpr auto minimum = std::numeric_limits<int>::min();
    constexpr auto maximum = std::numeric_limits<int>::max();
    const auto fits = [](std::int64_t coordinate) {
        return coordinate >= minimum && coordinate <= maximum;
    };
    if (!fits(left) || !fits(top) || !fits(right) || !fits(bottom)) {
        throw ConfigError("dock: placement exceeds native coordinate limits");
    }
    return {static_cast<int>(left), static_cast<int>(top),
            static_cast<int>(right), static_cast<int>(bottom)};
}

} // namespace

DockPlacement place_dock(Edge edge, PixelRect work_area,
                         int parallel_span, int panel_depth, int tab_depth,
                         double position) {
    if (!std::isfinite(position) || position < 0 || position > 1) {
        throw ConfigError("dock: position must be finite and from 0 through 1");
    }
    const bool horizontal = edge == Edge::top || edge == Edge::bottom;
    if (!horizontal && edge != Edge::left && edge != Edge::right) {
        throw ConfigError("dock: invalid display edge");
    }
    const auto left = static_cast<std::int64_t>(work_area.left);
    const auto top = static_cast<std::int64_t>(work_area.top);
    const auto right = static_cast<std::int64_t>(work_area.right);
    const auto bottom = static_cast<std::int64_t>(work_area.bottom);
    const auto width = right - left;
    const auto height = bottom - top;
    if (width <= 0 || height <= 0) {
        throw ConfigError("dock: display work area must have positive dimensions");
    }
    if (parallel_span <= 0 || panel_depth <= 0 || tab_depth <= 0) {
        throw ConfigError("dock: parallel span, panel depth, and tab depth must be positive");
    }
    const auto depth = static_cast<std::int64_t>(panel_depth) + tab_depth;
    if (depth > std::numeric_limits<int>::max()) {
        throw ConfigError("dock: total depth exceeds native window-size limits");
    }
    const auto available_parallel = horizontal ? width : height;
    const auto available_depth = horizontal ? height : width;
    if (parallel_span > available_parallel || depth > available_depth) {
        throw ConfigError("dock: configured grid and tabs do not fit the display work area");
    }

    const auto start = (horizontal ? left : top) + static_cast<std::int64_t>(
        std::floor(static_cast<double>(available_parallel - parallel_span) * position));
    const auto end = start + parallel_span;
    switch (edge) {
    case Edge::top:
        return {checked_rect(start, top, end, top + depth),
                checked_rect(start, top - panel_depth, end, top + tab_depth),
                checked_rect(start, top, end, top + tab_depth)};
    case Edge::bottom:
        return {checked_rect(start, bottom - depth, end, bottom),
                checked_rect(start, bottom - tab_depth, end, bottom + panel_depth),
                checked_rect(start, bottom - tab_depth, end, bottom)};
    case Edge::left:
        return {checked_rect(left, start, left + depth, end),
                checked_rect(left - panel_depth, start, left + tab_depth, end),
                checked_rect(left, start, left + tab_depth, end)};
    case Edge::right:
        return {checked_rect(right - depth, start, right, end),
                checked_rect(right - tab_depth, start, right + panel_depth, end),
                checked_rect(right - tab_depth, start, right, end)};
    }
    throw ConfigError("dock: invalid display edge");
}

PixelRect dock_canvas(const DockPlacement& placement, double expansion) noexcept {
    const auto coordinate = [expansion](int collapsed, int expanded) {
        return static_cast<int>(std::lround(std::lerp(static_cast<double>(collapsed),
            static_cast<double>(expanded), std::clamp(expansion, 0.0, 1.0))));
    };
    const int left = coordinate(placement.collapsed.left, placement.expanded.left);
    const int top = coordinate(placement.collapsed.top, placement.expanded.top);
    // Round the origin once. Independently rounding opposite edges can add a
    // pixel when a negative-coordinate monitor straddles a half-pixel step.
    return {left, top,
        static_cast<int>(static_cast<std::int64_t>(left) + placement.expanded.right - placement.expanded.left),
        static_cast<int>(static_cast<std::int64_t>(top) + placement.expanded.bottom - placement.expanded.top)};
}

PixelRect dock_viewport(PixelRect canvas, PixelRect work_area, int padding) noexcept {
    return {static_cast<int>(std::max<std::int64_t>(work_area.left, static_cast<std::int64_t>(canvas.left) - padding)),
        static_cast<int>(std::max<std::int64_t>(work_area.top, static_cast<std::int64_t>(canvas.top) - padding)),
        static_cast<int>(std::min<std::int64_t>(work_area.right, static_cast<std::int64_t>(canvas.right) + padding)),
        static_cast<int>(std::min<std::int64_t>(work_area.bottom, static_cast<std::int64_t>(canvas.bottom) + padding))};
}

void DockAnimation::reset(bool expanded) noexcept {
    from_ = to_ = expanded ? 1.0 : 0.0;
    duration_ = 0;
}

void DockAnimation::start(bool expanded, std::uint32_t duration_ms, std::uint64_t now) noexcept {
    from_ = value(now);
    to_ = expanded ? 1.0 : 0.0;
    // A reversal starts exactly where the previous motion is now. The setting
    // describes a full trip; a partial return takes proportionately less time.
    duration_ = duration_ms * std::abs(to_ - from_);
    started_ = now;
}

double DockAnimation::value(std::uint64_t now) const noexcept {
    if (!running(now)) { return to_; }
    const double time = static_cast<double>(now - started_) / duration_;
    return std::lerp(from_, to_, time * time * (3.0 - 2.0 * time));
}

bool DockAnimation::running(std::uint64_t now) const noexcept {
    return from_ != to_ && now >= started_ && static_cast<double>(now - started_) < duration_;
}

bool is_transparent_overlay(const WindowTransparency& window) noexcept {
    return window.layered &&
        (window.click_through ||
         (window.attributes_known &&
          (window.color_key || (window.alpha_blend && window.alpha < 255))));
}

PeekController::PeekController(bool auto_hide) noexcept
    : auto_hide_(auto_hide),
      state_(auto_hide ? DockState::collapsed : DockState::expanded) {}

DockState PeekController::state() const noexcept { return state_; }
Pending PeekController::pending() const noexcept { return pending_; }

void PeekController::update_pending() noexcept {
    if (suppressed_ || menu_open_ || !auto_hide_) {
        pending_ = Pending::none;
    } else if (state_ == DockState::collapsed && pointer_inside_) {
        pending_ = Pending::reveal;
    } else if (state_ == DockState::expanded && !pointer_inside_) {
        pending_ = Pending::hide;
    } else {
        pending_ = Pending::none;
    }
}

void PeekController::pointer_changed(bool inside) noexcept {
    pointer_inside_ = inside;
    update_pending();
}

void PeekController::reveal() noexcept {
    if (suppressed_) { return; }
    state_ = DockState::expanded;
    update_pending();
}

void PeekController::timer_elapsed() noexcept {
    const auto action = pending_;
    pending_ = Pending::none;
    if (suppressed_ || menu_open_ || !auto_hide_) {
        return;
    }
    if (action == Pending::reveal && pointer_inside_ && state_ == DockState::collapsed) {
        state_ = DockState::expanded;
    } else if (action == Pending::hide && !pointer_inside_ && state_ == DockState::expanded) {
        state_ = DockState::collapsed;
    }
}

void PeekController::set_suppressed(bool suppressed) noexcept {
    if (suppressed_ == suppressed) {
        return;
    }
    suppressed_ = suppressed;
    if (suppressed) {
        state_ = DockState::suppressed;
        pending_ = Pending::none;
    } else {
        state_ = menu_open_ || !auto_hide_ ? DockState::expanded : DockState::collapsed;
        update_pending();
    }
}

void PeekController::set_menu_open(bool open) noexcept {
    if (menu_open_ == open) {
        return;
    }
    menu_open_ = open;
    if (open && !suppressed_) {
        state_ = DockState::expanded;
    }
    update_pending();
}

} // namespace launcher
