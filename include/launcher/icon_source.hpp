#pragma once

#include "launcher/resource_identifier.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace launcher {

enum class StockIcon {
    application, folder, document, computer, world, find, shield, recycle,
    settings, information, drive, network, audio, image, video
};

inline constexpr std::array<std::string_view, 15> stock_icon_names{
    "application", "folder", "document", "computer", "world", "find", "shield", "recycle",
    "settings", "information", "drive", "network", "audio", "image", "video"
};

inline std::optional<StockIcon> stock_icon_named(std::string_view name)
{
    for (std::size_t index = 0; index < stock_icon_names.size(); ++index) {
        if (stock_icon_names[index] == name) { return static_cast<StockIcon>(index); }
    }
    return std::nullopt;
}

struct IconIndex final {
    int value = 0; // Zero-based position among logical icon groups.
    bool operator==(const IconIndex&) const = default;
};

struct IconResourceId final {
    std::uint16_t value = 0; // RT_GROUP_ICON identifier, not an individual RT_ICON frame.
    bool operator==(const IconResourceId&) const = default;
};

using IconGroupSelector = std::variant<IconIndex, IconResourceId>;

// Paths contain their final substituted text; image loading never expands it.
struct FileIcon final {
    std::filesystem::path path;
    bool operator==(const FileIcon&) const = default;
};

struct IconGroup final {
    std::filesystem::path path;
    IconGroupSelector selector;
    bool operator==(const IconGroup&) const = default;
};

struct PeResourceIcon final {
    std::filesystem::path path;
    ResourceIdentifier type;
    ResourceIdentifier name;
    std::optional<std::uint16_t> language; // Omitted uses Windows language selection.
    bool operator==(const PeResourceIcon&) const = default;
};

// Prepared launch targets use Shell lookup; explicit files use image decoding first.
struct ShellItemIcon final {
    std::filesystem::path path;
    bool operator==(const ShellItemIcon&) const = default;
};

// Add other resource extractors as distinct alternatives, with their own selectors.
// The same description, with resolved paths, also forms the decoded-image cache key.
using IconSource = std::variant<StockIcon, FileIcon, IconGroup, PeResourceIcon, ShellItemIcon>;

} // namespace launcher
