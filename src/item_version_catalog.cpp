#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/item_version_catalog.hpp"
#include "launcher/adobe_cc.hpp"

#include <algorithm>
#include <climits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace launcher {
namespace {

std::wstring product_name(std::string_view text) {
    if (text.empty()) { return {}; }
    if (text.size() > INT_MAX || text.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("Invalid Adobe product name.");
    }
    const auto length = static_cast<int>(text.size());
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (!count) { throw std::invalid_argument("Adobe product name is not valid UTF-8."); }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, result.data(), count)) {
        throw std::invalid_argument("Cannot convert Adobe product name to Unicode.");
    }
    return result;
}

bool same_executable(const std::filesystem::path& left, const std::filesystem::path& right) noexcept {
    const auto& first = left.native();
    const auto& second = right.native();
    return first.size() == second.size() && first.size() <= INT_MAX &&
        CompareStringOrdinal(first.data(), static_cast<int>(first.size()), second.data(), static_cast<int>(second.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

ItemVersionCatalog::ItemVersionCatalog(const LaunchAction& action, Snapshot snapshot) : snapshot_(std::move(snapshot)), versions_(snapshot_) {
    if (!snapshot_ || action.handler != "adobeCC") { return; }
    const auto product = product_name(action.target);
    include_beta_ = action.include_beta || adobe_cc_is_beta_product(product);
    versions_ = std::make_shared<const std::vector<TargetVersion>>(adobe_cc_versions(*snapshot_, product));
}

const ItemVersionCatalog::Snapshot& ItemVersionCatalog::snapshot() const noexcept { return snapshot_; }

std::span<const TargetVersion> ItemVersionCatalog::versions() const noexcept {
    return versions_ ? std::span<const TargetVersion>(*versions_) : std::span<const TargetVersion>{};
}

std::size_t ItemVersionCatalog::size() const noexcept { return versions().size(); }
bool ItemVersionCatalog::empty() const noexcept { return versions().empty(); }
bool ItemVersionCatalog::multiple() const noexcept { return size() > 1; }

const TargetVersion* ItemVersionCatalog::latest() const noexcept {
    const auto available = versions();
    const auto found = std::ranges::find_if(available, [this](const auto& version) { return include_beta_ || !version.beta; });
    return found == available.end() ? nullptr : &*found;
}

const TargetVersion* ItemVersionCatalog::find(const std::filesystem::path& executable) const noexcept {
    const auto available = versions();
    const auto found = std::ranges::find_if(available, [&](const auto& version) { return same_executable(version.executable, executable); });
    return found == available.end() ? nullptr : &*found;
}

ItemVersionSelection ItemVersionCatalog::select(const std::optional<std::filesystem::path>& pinned) const noexcept {
    ItemVersionSelection result;
    result.automatic = !pinned;
    if (!snapshot_) { return result; }
    result.version = pinned ? find(*pinned) : latest();
    result.state = result.version ? ItemVersionSelectionState::available : pinned
        ? ItemVersionSelectionState::pinned_missing : ItemVersionSelectionState::no_eligible_version;
    return result;
}

} // namespace launcher
