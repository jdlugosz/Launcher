#pragma once

#include <cstdint>

namespace launcher {

constexpr float icon_hover_padding_px(std::uint32_t gap) noexcept {
    if (gap < 4) { return static_cast<float>(gap); }
    return gap < 8 ? 4.0F : static_cast<float>(gap) * 0.5F;
}

} // namespace launcher
