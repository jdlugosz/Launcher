#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/dock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>

namespace {

void require_inside(launcher::PixelRect rectangle, launcher::PixelRect area) {
    REQUIRE(rectangle.left >= area.left);
    REQUIRE(rectangle.top >= area.top);
    REQUIRE(rectangle.right <= area.right);
    REQUIRE(rectangle.bottom <= area.bottom);
}

} // namespace

TEST_CASE("Dock movement eases without overshoot and reverses without jumping", "[dock][motion]") {
    launcher::DockAnimation movement;
    movement.start(true, 400, 1000);
    REQUIRE(movement.value(1000) == 0);
    REQUIRE(movement.running(1000));
    REQUIRE(movement.value(1100) > 0);
    REQUIRE(movement.value(1100) < 0.25); // Gentle start, rather than constant velocity.
    REQUIRE(movement.value(1200) == 0.5);
    REQUIRE(movement.value(1300) > 0.75);
    REQUIRE(movement.value(1400) == 1);
    REQUIRE_FALSE(movement.running(1400));

    movement.start(false, 800, 1500);
    REQUIRE(movement.value(1900) == 0.5);
    movement.start(true, 200, 1900);
    REQUIRE(movement.value(1900) == 0.5);
    REQUIRE(movement.value(1950) == 0.75);
    REQUIRE(movement.value(2000) == 1);
    REQUIRE_FALSE(movement.running(2000));
    movement.start(false, 0, 2100);
    REQUIRE(movement.value(2100) == 0);
    REQUIRE_FALSE(movement.running(2100));
    movement.reset(true);
    REQUIRE(movement.value(2200) == 1);
}

TEST_CASE("Every sliding viewport stays on its selected monitor at shared edges", "[dock][motion]") {
    const launcher::PixelRect area{-1920, -200, 0, 880};
    for (const auto edge : {launcher::Edge::top, launcher::Edge::bottom, launcher::Edge::left, launcher::Edge::right}) {
        const auto placement = launcher::place_dock(edge, area, 640, 170, 40, 0.25);
        REQUIRE(launcher::dock_canvas(placement, 0) == placement.collapsed);
        REQUIRE(launcher::dock_canvas(placement, 1) == placement.expanded);
        const bool vertical = edge == launcher::Edge::left || edge == launcher::Edge::right;
        int previous_depth = 0;
        for (const auto expansion : {0.0, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0}) {
            const auto canvas = launcher::dock_canvas(placement, expansion);
            const auto viewport = launcher::dock_viewport(canvas, area, 12);
            require_inside(viewport, area);
            REQUIRE(canvas.right - canvas.left == placement.expanded.right - placement.expanded.left);
            REQUIRE(canvas.bottom - canvas.top == placement.expanded.bottom - placement.expanded.top);
            const int depth = vertical ? viewport.right - viewport.left : viewport.bottom - viewport.top;
            REQUIRE(depth >= previous_depth);
            previous_depth = depth;
        }
    }
}

TEST_CASE("Dock geometry keeps visible tabs inside every edge of a negative-coordinate monitor", "[dock]") {
    using launcher::PixelRect;
    const PixelRect area{-1920, -200, 0, 880};
    launcher::DockPlacement placement;

    SECTION("Top has the panel above the tabs") {
        placement = launcher::place_dock(launcher::Edge::top, area, 640, 170, 40);
        REQUIRE(placement.expanded == PixelRect{-1280, -200, -640, 10});
        REQUIRE(placement.collapsed == PixelRect{-1280, -370, -640, -160});
        REQUIRE(placement.collapsed_tabs == PixelRect{-1280, -200, -640, -160});
    }
    SECTION("Bottom has the panel below the tabs") {
        placement = launcher::place_dock(launcher::Edge::bottom, area, 640, 170, 40);
        REQUIRE(placement.expanded == PixelRect{-1280, 670, -640, 880});
        REQUIRE(placement.collapsed == PixelRect{-1280, 840, -640, 1050});
        REQUIRE(placement.collapsed_tabs == PixelRect{-1280, 840, -640, 880});
    }
    SECTION("Left has the panel left of the tabs") {
        placement = launcher::place_dock(launcher::Edge::left, area, 640, 170, 40);
        REQUIRE(placement.expanded == PixelRect{-1920, 20, -1710, 660});
        REQUIRE(placement.collapsed == PixelRect{-2090, 20, -1880, 660});
        REQUIRE(placement.collapsed_tabs == PixelRect{-1920, 20, -1880, 660});
    }
    SECTION("Right has the panel right of the tabs, including a shared monitor seam") {
        placement = launcher::place_dock(launcher::Edge::right, area, 640, 170, 40);
        REQUIRE(placement.expanded == PixelRect{-210, 20, 0, 660});
        REQUIRE(placement.collapsed == PixelRect{-40, 20, 170, 660});
        REQUIRE(placement.collapsed_tabs == PixelRect{-40, 20, 0, 660});
    }
    require_inside(placement.expanded, area);
    require_inside(placement.collapsed_tabs, area);
    REQUIRE(placement.expanded.right - placement.expanded.left ==
            placement.collapsed.right - placement.collapsed.left);
    REQUIRE(placement.expanded.bottom - placement.expanded.top ==
            placement.collapsed.bottom - placement.collapsed.top);
}

TEST_CASE("Dock centering uses the selected work area rather than the primary origin", "[dock]") {
    const auto dock = launcher::place_dock(launcher::Edge::right,
        {3840, 500, 5760, 1580}, 640, 170, 40);
    REQUIRE(dock.expanded == launcher::PixelRect{5550, 720, 5760, 1360});
    REQUIRE(dock.collapsed_tabs == launcher::PixelRect{5720, 720, 5760, 1360});
}

TEST_CASE("Odd unused pixels consistently favor the beginning of an edge", "[dock]") {
    const auto top = launcher::place_dock(launcher::Edge::top,
        {-101, -301, 900, 700}, 200, 50, 10);
    REQUIRE(top.expanded == launcher::PixelRect{299, -301, 499, -241});
    const auto right = launcher::place_dock(launcher::Edge::right,
        {-101, -301, 900, 700}, 200, 50, 10);
    REQUIRE(right.expanded == launcher::PixelRect{840, 99, 900, 299});
}

TEST_CASE("An exactly fitting dock remains valid", "[dock]") {
    const launcher::PixelRect area{-1920, -200, 0, 880};
    const auto dock = launcher::place_dock(launcher::Edge::bottom, area, 1920, 1000, 80);
    REQUIRE(dock.expanded == area);
    REQUIRE(dock.collapsed_tabs == launcher::PixelRect{-1920, 800, 0, 880});
}

TEST_CASE("Invalid dock dimensions and insufficient space are rejected", "[dock]") {
    const launcher::PixelRect area{0, 0, 1000, 800};
    SECTION("Empty or inverted display area") {
        REQUIRE_THROWS_AS(launcher::place_dock(launcher::Edge::top,
            {0, 0, 0, 800}, 100, 50, 10), launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::place_dock(launcher::Edge::left,
            {0, 800, 1000, 0}, 100, 50, 10), launcher::ConfigError);
    }
    SECTION("Nonpositive dock dimensions") {
        REQUIRE_THROWS_AS(launcher::place_dock(launcher::Edge::top, area, 0, 50, 10),
            launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::place_dock(launcher::Edge::left, area, 100, -1, 10),
            launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::place_dock(launcher::Edge::right, area, 100, 50, 0),
            launcher::ConfigError);
    }
    SECTION("Parallel span exceeds the selected edge") {
        REQUIRE_THROWS_AS(launcher::place_dock(launcher::Edge::top, area, 1001, 50, 10),
            launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::place_dock(launcher::Edge::right, area, 801, 50, 10),
            launcher::ConfigError);
    }
    SECTION("Combined depths exceed the perpendicular dimension") {
        REQUIRE_THROWS_AS(launcher::place_dock(launcher::Edge::bottom, area, 100, 790, 11),
            launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::place_dock(launcher::Edge::left, area, 100, 990, 11),
            launcher::ConfigError);
    }
    SECTION("Unknown edge") {
        REQUIRE_THROWS_AS(launcher::place_dock(static_cast<launcher::Edge>(42),
            area, 100, 50, 10), launcher::ConfigError);
    }
}

TEST_CASE("Collapsed positions and combined native dimensions cannot overflow", "[dock]") {
    constexpr auto minimum = std::numeric_limits<int>::min();
    constexpr auto maximum = std::numeric_limits<int>::max();
    SECTION("Top would move past the smallest coordinate") {
        REQUIRE_THROWS_AS(launcher::place_dock(launcher::Edge::top,
            {0, minimum, 1000, minimum + 800}, 100, 50, 10), launcher::ConfigError);
    }
    SECTION("Left would move past the smallest coordinate") {
        REQUIRE_THROWS_AS(launcher::place_dock(launcher::Edge::left,
            {minimum, 0, minimum + 1000, 800}, 100, 50, 10), launcher::ConfigError);
    }
    SECTION("Bottom would move past the largest coordinate") {
        REQUIRE_THROWS_AS(launcher::place_dock(launcher::Edge::bottom,
            {0, maximum - 800, 1000, maximum}, 100, 50, 10), launcher::ConfigError);
    }
    SECTION("Right would move past the largest coordinate") {
        REQUIRE_THROWS_AS(launcher::place_dock(launcher::Edge::right,
            {maximum - 1000, 0, maximum, 800}, 100, 50, 10), launcher::ConfigError);
    }
    SECTION("Depth addition is checked before conversion") {
        REQUIRE_THROWS_AS(launcher::place_dock(launcher::Edge::top,
            {minimum, minimum, maximum, maximum}, 100, maximum, 1), launcher::ConfigError);
    }
    SECTION("A wide area centers without overflowing intermediate coordinates") {
        const auto dock = launcher::place_dock(launcher::Edge::top,
            {minimum, 0, maximum, 800}, 100, 50, 10);
        REQUIRE(dock.expanded == launcher::PixelRect{-51, 0, 49, 60});
    }
}

TEST_CASE("Hover dwell reveals and departure dwell collapses", "[peek]") {
    launcher::PeekController peek;
    REQUIRE(peek.state() == launcher::DockState::collapsed);
    REQUIRE(peek.pending() == launcher::Pending::none);
    peek.pointer_changed(true);
    REQUIRE(peek.state() == launcher::DockState::collapsed);
    REQUIRE(peek.pending() == launcher::Pending::reveal);
    peek.timer_elapsed();
    REQUIRE(peek.state() == launcher::DockState::expanded);
    REQUIRE(peek.pending() == launcher::Pending::none);
    peek.pointer_changed(false);
    REQUIRE(peek.state() == launcher::DockState::expanded);
    REQUIRE(peek.pending() == launcher::Pending::hide);
    peek.timer_elapsed();
    REQUIRE(peek.state() == launcher::DockState::collapsed);
    REQUIRE(peek.pending() == launcher::Pending::none);
}

TEST_CASE("Leaving before reveal and returning before hide cancel the pending transition", "[peek]") {
    launcher::PeekController peek;
    peek.pointer_changed(true);
    peek.pointer_changed(false);
    REQUIRE(peek.pending() == launcher::Pending::none);
    peek.timer_elapsed();
    REQUIRE(peek.state() == launcher::DockState::collapsed);

    peek.pointer_changed(true);
    peek.timer_elapsed();
    peek.pointer_changed(false);
    peek.pointer_changed(true);
    REQUIRE(peek.pending() == launcher::Pending::none);
    peek.timer_elapsed();
    REQUIRE(peek.state() == launcher::DockState::expanded);
}

TEST_CASE("Refreshing pointer state before a timer prevents a stale transition", "[peek]") {
    launcher::PeekController peek;
    peek.pointer_changed(true);
    peek.pointer_changed(true);
    peek.timer_elapsed();
    REQUIRE(peek.state() == launcher::DockState::expanded);
    peek.pointer_changed(false);
    peek.pointer_changed(true);
    peek.timer_elapsed();
    REQUIRE(peek.state() == launcher::DockState::expanded);
}

TEST_CASE("A popup expands and holds the dock until the menu closes", "[peek]") {
    launcher::PeekController peek;
    peek.pointer_changed(true);
    peek.set_menu_open(true);
    REQUIRE(peek.state() == launcher::DockState::expanded);
    REQUIRE(peek.pending() == launcher::Pending::none);
    peek.pointer_changed(false);
    peek.timer_elapsed();
    REQUIRE(peek.state() == launcher::DockState::expanded);
    REQUIRE(peek.pending() == launcher::Pending::none);
    peek.set_menu_open(false);
    REQUIRE(peek.pending() == launcher::Pending::hide);
    peek.timer_elapsed();
    REQUIRE(peek.state() == launcher::DockState::collapsed);
}

TEST_CASE("Closing a menu while inside does not arm a hide", "[peek]") {
    launcher::PeekController peek;
    peek.set_menu_open(true);
    peek.pointer_changed(true);
    peek.set_menu_open(false);
    REQUIRE(peek.state() == launcher::DockState::expanded);
    REQUIRE(peek.pending() == launcher::Pending::none);
}

TEST_CASE("Full-screen suppression cancels timers and restores a fresh collapsed state", "[peek]") {
    launcher::PeekController peek;
    peek.pointer_changed(true);
    peek.set_suppressed(true);
    REQUIRE(peek.state() == launcher::DockState::suppressed);
    REQUIRE(peek.pending() == launcher::Pending::none);
    peek.timer_elapsed();
    peek.pointer_changed(false);
    peek.timer_elapsed();
    REQUIRE(peek.state() == launcher::DockState::suppressed);
    peek.set_suppressed(false);
    REQUIRE(peek.state() == launcher::DockState::collapsed);
    REQUIRE(peek.pending() == launcher::Pending::none);

    peek.pointer_changed(true);
    peek.timer_elapsed();
    peek.pointer_changed(false);
    REQUIRE(peek.pending() == launcher::Pending::hide);
    peek.set_suppressed(true);
    peek.timer_elapsed();
    peek.set_suppressed(false);
    REQUIRE(peek.state() == launcher::DockState::collapsed);
}

TEST_CASE("Ending suppression under the pointer requires a new reveal dwell", "[peek]") {
    launcher::PeekController peek;
    peek.set_suppressed(true);
    peek.pointer_changed(true);
    peek.set_suppressed(false);
    REQUIRE(peek.state() == launcher::DockState::collapsed);
    REQUIRE(peek.pending() == launcher::Pending::reveal);
    peek.timer_elapsed();
    REQUIRE(peek.state() == launcher::DockState::expanded);
}

TEST_CASE("Suppression takes precedence over a popup", "[peek]") {
    launcher::PeekController peek;
    peek.set_menu_open(true);
    peek.set_suppressed(true);
    peek.pointer_changed(false);
    peek.timer_elapsed();
    REQUIRE(peek.state() == launcher::DockState::suppressed);
    REQUIRE(peek.pending() == launcher::Pending::none);
    peek.set_suppressed(false);
    REQUIRE(peek.state() == launcher::DockState::expanded);
    REQUIRE(peek.pending() == launcher::Pending::none);
    peek.set_menu_open(false);
    REQUIRE(peek.pending() == launcher::Pending::hide);
}

TEST_CASE("Disabling auto-hide stays expanded except during full-screen suppression", "[peek]") {
    launcher::PeekController peek(false);
    REQUIRE(peek.state() == launcher::DockState::expanded);
    peek.pointer_changed(false);
    peek.timer_elapsed();
    peek.pointer_changed(true);
    peek.pointer_changed(false);
    REQUIRE(peek.state() == launcher::DockState::expanded);
    REQUIRE(peek.pending() == launcher::Pending::none);
    peek.set_suppressed(true);
    peek.timer_elapsed();
    REQUIRE(peek.state() == launcher::DockState::suppressed);
    peek.set_suppressed(false);
    REQUIRE(peek.state() == launcher::DockState::expanded);
    REQUIRE(peek.pending() == launcher::Pending::none);
}

TEST_CASE("The captured color-keyed ruler window is a transparent overlay", "[fullscreen][transparency]") {
    const launcher::WindowTransparency ruler{
        .layered = true,
        .click_through = false,
        .attributes_known = true,
        .color_key = true,
        .alpha_blend = false,
        .alpha = 0,
    };
    REQUIRE(launcher::is_transparent_overlay(ruler));
}

TEST_CASE("An opaque layered window remains eligible for fullscreen classification", "[fullscreen][transparency]") {
    const launcher::WindowTransparency opaque{
        .layered = true,
        .attributes_known = true,
        .alpha_blend = true,
        .alpha = 255,
    };
    REQUIRE_FALSE(launcher::is_transparent_overlay(opaque));
}

TEST_CASE("Known partial alpha blending marks a layered window as transparent", "[fullscreen][transparency]") {
    launcher::WindowTransparency window{
        .layered = true,
        .attributes_known = true,
        .alpha_blend = true,
    };
    for (const auto alpha : std::array<std::uint8_t, 3>{0, 127, 254}) {
        CAPTURE(static_cast<unsigned>(alpha));
        window.alpha = alpha;
        REQUIRE(launcher::is_transparent_overlay(window));
    }
}

TEST_CASE("Unknown attributes do not exclude a layered window unless it is click-through", "[fullscreen][transparency]") {
    launcher::WindowTransparency unknown{
        .layered = true,
        .attributes_known = false,
        .color_key = true,
        .alpha_blend = true,
        .alpha = 0,
    };
    REQUIRE_FALSE(launcher::is_transparent_overlay(unknown));
    unknown.click_through = true;
    REQUIRE(launcher::is_transparent_overlay(unknown));
}

TEST_CASE("A nonlayered window is not excluded by a transparent painting flag", "[fullscreen][transparency]") {
    const launcher::WindowTransparency ordinary{
        .layered = false,
        .click_through = true,
        .attributes_known = true,
        .color_key = true,
        .alpha_blend = true,
        .alpha = 0,
    };
    REQUIRE_FALSE(launcher::is_transparent_overlay(ordinary));
}

TEST_CASE("An alpha value alone is ignored when native alpha blending is absent", "[fullscreen][transparency]") {
    const launcher::WindowTransparency window{
        .layered = true,
        .attributes_known = true,
        .color_key = false,
        .alpha_blend = false,
        .alpha = 0,
    };
    REQUIRE_FALSE(launcher::is_transparent_overlay(window));
}

TEST_CASE("Normalized positions use the available interval on all four negative-coordinate edges", "[dock][position]") {
    const launcher::PixelRect area{-1920, -200, 0, 880};
    for (const auto edge : {launcher::Edge::top, launcher::Edge::bottom,
                            launcher::Edge::left, launcher::Edge::right}) {
        const bool horizontal = edge == launcher::Edge::top || edge == launcher::Edge::bottom;
        const auto beginning = launcher::place_dock(edge, area, 640, 170, 40, 0);
        const auto ending = launcher::place_dock(edge, area, 640, 170, 40, 1);
        const auto quarter = launcher::place_dock(edge, area, 640, 170, 40, 0.25);
        REQUIRE((horizontal ? beginning.expanded.left : beginning.expanded.top) ==
                (horizontal ? area.left : area.top));
        REQUIRE((horizontal ? ending.expanded.right : ending.expanded.bottom) ==
                (horizontal ? area.right : area.bottom));
        REQUIRE((horizontal ? quarter.expanded.left : quarter.expanded.top) ==
                (horizontal ? -1600 : -90));
        require_inside(beginning.expanded, area);
        require_inside(ending.expanded, area);
    }
}

TEST_CASE("Normalized positions reject nonfinite and out-of-range values", "[dock][position]") {
    for (const auto position : {-0.01, 1.01, std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::infinity()}) {
        REQUIRE_THROWS_AS(launcher::place_dock(launcher::Edge::right,
            {0, 0, 1000, 800}, 100, 50, 10, position), launcher::ConfigError);
    }
}
