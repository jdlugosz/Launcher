#pragma once

#include "launcher/config.hpp"
#include "launcher/launch.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace launcher {

struct ItemPresentationRequest final {
    Item item;
    // A versioned item supplies its already selected concrete Shell action.
    // Null means unresolved: never inspect item.launch to rediscover a target.
    std::optional<LaunchAction> action;
    IconSource source = StockIcon::application;
    std::filesystem::path configuration_directory;
    std::uint32_t pixel_size = 0;
};

// An owned, apartment-independent result. Pixels are tightly packed 32bpp
// premultiplied BGRA. No WIC, Shell, or Direct2D interface leaves the worker.
struct ItemPresentation final {
    PreparedLaunch launch;
    std::wstring label;
    bool can_admin = false;
    bool can_reveal = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::vector<std::uint8_t> pixels;
    std::vector<std::string> diagnostics;
};

// Blocking worker adapter: create a private STA, prepare the action/label/menu
// facts, decode the icon, copy its pixels, and destroy COM owners before return.
// Missing/failed images use the stock application icon without vetoing launch.
// Sizes outside 1..4096 (64 MiB of output pixels) throw invalid_argument before
// any filesystem or Shell work. Admission, caching, and generations belong to
// the caller; this function owns no service or window and starts no threads.
ItemPresentation prepare_item_presentation(const ItemPresentationRequest& request);

} // namespace launcher
