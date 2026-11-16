#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/item_version_catalog.hpp"
#include "launcher/adobe_cc.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Catalog = launcher::ItemVersionCatalog;
using State = launcher::ItemVersionSelectionState;

launcher::TargetVersion edition(std::wstring product, unsigned major, fs::path executable, bool beta = false) {
    launcher::TargetVersion result;
    result.name = launcher::canonical_adobe_cc_name(product);
    result.product_name = std::move(product);
    result.product_version = std::to_wstring(major);
    result.version = {static_cast<std::uint16_t>(major), 0, 0, 0};
    result.executable = std::move(executable);
    result.beta = beta;
    return result;
}

Catalog::Snapshot snapshot(std::vector<launcher::TargetVersion> versions) {
    return std::make_shared<const std::vector<launcher::TargetVersion>>(std::move(versions));
}

launcher::LaunchAction adobe_action(std::string product, bool include_beta = true) {
    launcher::LaunchAction action;
    action.handler = "adobeCC";
    action.target = std::move(product);
    action.include_beta = include_beta;
    return action;
}

} // namespace

TEST_CASE("Item version catalog distinguishes unchecked and checked empty snapshots", "[item-version-catalog]") {
    const Catalog unchecked;
    CHECK_FALSE(unchecked.snapshot());
    CHECK(unchecked.empty());
    CHECK_FALSE(unchecked.multiple());
    CHECK(unchecked.latest() == nullptr);
    CHECK(unchecked.select().state == State::not_checked);
    CHECK(unchecked.select().automatic);
    const auto pending_pin = unchecked.select(fs::path(L"C:\\Tools\\Editor.exe"));
    CHECK(pending_pin.state == State::not_checked);
    CHECK_FALSE(pending_pin.automatic);

    const auto known_empty = snapshot({});
    const Catalog empty(launcher::LaunchAction{}, known_empty);
    CHECK(empty.snapshot() == known_empty);
    CHECK(empty.empty());
    CHECK_FALSE(empty.multiple());
    CHECK(empty.select().state == State::no_eligible_version);
    CHECK(empty.select(fs::path(L"C:\\Tools\\Editor.exe")).state == State::pinned_missing);
}

TEST_CASE("Generic item catalog retains the adopted search list and version order", "[item-version-catalog]") {
    const auto source = snapshot({
        edition(L"Editor", 5, L"C:\\Tools\\5\\Editor.exe"),
        edition(L"Editor", 4, L"C:\\Tools\\4\\Editor.exe")});
    launcher::LaunchAction action;
    action.target = "C:\\Tools\\*\\Editor.exe";
    action.include_beta = false;
    const Catalog catalog(action, source);
    REQUIRE(catalog.size() == 2);
    CHECK(catalog.multiple());
    CHECK(catalog.snapshot() == source);
    CHECK(catalog.versions().data() == source->data());
    CHECK(catalog.latest() == &source->front());
    CHECK(catalog.select().version == catalog.latest());
    CHECK(catalog.select().state == State::available);

    const Catalog single(action, snapshot({source->front()}));
    CHECK(single.size() == 1);
    CHECK_FALSE(single.multiple());
}

TEST_CASE("Adobe item catalog filters products once and separates menu availability from Beta policy", "[item-version-catalog]") {
    const auto source = snapshot({
        edition(L"Adobe Photoshop 2024", 24, L"C:\\Adobe\\Photoshop 2024\\Photoshop.exe"),
        edition(L"Adobe InDesign 2030", 30, L"C:\\Adobe\\InDesign 2030\\InDesign.exe"),
        edition(L"Adobe Photoshop 2026 (Beta)", 26, L"C:\\Adobe\\Photoshop Beta\\Photoshop.exe", true),
        edition(L"Adobe Photoshop 2025", 25, L"C:\\Adobe\\Photoshop 2025\\Photoshop.exe"),
        edition(L"Adobe Photoshop 2025", 25, L"c:\\adobe\\photoshop 2025\\PHOTOSHOP.EXE")});
    const Catalog catalog(adobe_action("Photoshop", false), source);
    REQUIRE(catalog.size() == 3);
    CHECK(catalog.multiple());
    CHECK(catalog.snapshot() == source);
    CHECK(catalog.versions().data() != source->data());
    CHECK(catalog.versions().front().beta);
    REQUIRE(catalog.latest());
    CHECK(catalog.latest()->version[0] == 25);
    CHECK_FALSE(catalog.latest()->beta);
    CHECK(catalog.versions().back().version[0] == 24);

    // The menu offers Beta even when an automatic launch prefers stable.
    const auto beta = catalog.select(fs::path(L"c:\\adobe\\photoshop beta\\PHOTOSHOP.EXE"));
    REQUIRE(beta.version);
    CHECK(beta.state == State::available);
    CHECK_FALSE(beta.automatic);
    CHECK(beta.version == &catalog.versions().front());
    CHECK(beta.version->beta);

    const Catalog with_beta(adobe_action("Photoshop"), source);
    CHECK(with_beta.latest() == &with_beta.versions().front());
    const Catalog copied = catalog;
    CHECK(copied.snapshot() == source);
    CHECK(copied.versions().data() == catalog.versions().data());
    CHECK(copied.latest() == catalog.latest());
}

TEST_CASE("Explicit Adobe Beta product remains eligible when include_beta is false", "[item-version-catalog]") {
    const auto source = snapshot({
        edition(L"Adobe Premiere Pro 2025", 25, L"C:\\Adobe\\Premiere 2025\\Premiere.exe"),
        edition(L"Adobe Premiere Pro 2026 (Beta)", 26, L"C:\\Adobe\\Premiere Beta\\Premiere.exe", true)});
    const Catalog catalog(adobe_action("Premiere (Beta)", false), source);
    REQUIRE(catalog.size() == 1);
    REQUIRE(catalog.latest());
    CHECK(catalog.latest()->beta);
    CHECK(catalog.select().state == State::available);
    CHECK(catalog.find(source->front().executable) == nullptr);

    const Catalog stable_only(adobe_action("Premiere", false), snapshot({source->back()}));
    CHECK(stable_only.size() == 1);
    CHECK(stable_only.latest() == nullptr);
    CHECK(stable_only.select().state == State::no_eligible_version);
    CHECK(stable_only.select(source->back().executable).state == State::available);
}

TEST_CASE("Pinned item selection compares exact paths without silently following a newer version", "[item-version-catalog]") {
    const auto source = snapshot({
        edition(L"Editor", 5, L"D:\\Программа\\5\\Editor.exe"),
        edition(L"Editor", 4, L"D:\\Программа\\4\\Editor.exe")});
    const auto action = launcher::LaunchAction{};
    const Catalog catalog(action, source);
    const fs::path pin = L"d:\\ПРОГРАММА\\4\\EDITOR.EXE";
    const auto selected = catalog.select(pin);
    REQUIRE(selected.version);
    CHECK(selected.version == &source->back());
    CHECK(selected.state == State::available);
    CHECK_FALSE(selected.automatic);
    CHECK(catalog.find(L"D:\\Программа\\14\\Editor.exe") == nullptr);
    CHECK(catalog.find(L"D:\\Программа\\4\\Editor.exe.bak") == nullptr);

    const Catalog removed(action, snapshot({source->front()}));
    const auto unavailable = removed.select(pin);
    CHECK(unavailable.version == nullptr);
    CHECK(unavailable.state == State::pinned_missing);
    CHECK_FALSE(unavailable.automatic);
    CHECK(removed.select().version == removed.latest());
    // Restoring the same executable makes the retained preference available.
    CHECK(Catalog(action, source).select(pin).state == State::available);
}

TEST_CASE("Adobe item catalog rejects malformed product text", "[item-version-catalog]") {
    const auto source = snapshot({});
    CHECK_THROWS_AS(Catalog(adobe_action(std::string("Photoshop\0suffix", 16)), source), std::invalid_argument);
    CHECK_THROWS_AS(Catalog(adobe_action(std::string("\xff")), source), std::invalid_argument);
}
