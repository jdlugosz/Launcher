#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/adobe_cc.hpp"
#include "launcher/launch.hpp"
#include "launcher/config.hpp"
#include "launcher/item_version_catalog.hpp"
#include "launcher/version_discovery.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

launcher::AdobeCCInstallation installation(std::wstring product,
        std::array<std::uint16_t, 4> version, std::wstring path, bool beta = false,
        std::array<std::uint16_t, 4> file_version = {}) {
    launcher::AdobeCCInstallation result;
    result.name = launcher::canonical_adobe_cc_name(product);
    result.product_name = std::move(product);
    result.version = version;
    result.file_version_numbers = file_version;
    result.executable = std::move(path);
    result.beta = beta;
    return result;
}

namespace fs = std::filesystem;
using Kind = launcher::TargetFileKind;
using Status = launcher::TargetScanStatus;
using Operation = launcher::TargetScanOperation;
using Entry = launcher::TargetDirectoryEntry;
using Step = std::variant<Entry, std::error_code>;

std::error_code windows_error(DWORD code) { return {static_cast<int>(code), std::system_category()}; }

class AdobeCursor final : public launcher::TargetDirectoryCursor {
public:
    AdobeCursor(std::vector<Step> steps, std::shared_ptr<unsigned> closed) : steps_(std::move(steps)), closed_(std::move(closed)) {}
    ~AdobeCursor() override { ++*closed_; }
    std::expected<std::optional<Entry>, std::error_code> next() override {
        if (position_ == steps_.size()) { return std::nullopt; }
        const auto& step = steps_[position_++];
        if (const auto* error = std::get_if<std::error_code>(&step)) { return std::unexpected(*error); }
        return std::get<Entry>(step);
    }
private:
    std::vector<Step> steps_;
    std::shared_ptr<unsigned> closed_;
    std::size_t position_ = 0;
};

class AdobeSource final : public launcher::TargetVersionSource {
public:
    std::map<fs::path, std::expected<Kind, std::error_code>> probes;
    std::map<fs::path, std::vector<Step>> listings;
    std::map<fs::path, std::error_code> open_errors;
    std::map<fs::path, std::expected<std::optional<launcher::TargetVersion>, std::error_code>> metadata;
    std::set<fs::path> links;
    std::vector<fs::path> enumerated;
    std::vector<fs::path> metadata_read;
    std::shared_ptr<unsigned> closed = std::make_shared<unsigned>(0);
    std::expected<Kind, std::error_code> probe(const fs::path& path) override {
        const auto found = probes.find(path);
        return found == probes.end() ? std::expected<Kind, std::error_code>(std::unexpected(windows_error(ERROR_FILE_NOT_FOUND))) : found->second;
    }
    std::expected<Kind, std::error_code> probe_no_follow(const fs::path& path) override {
        return links.contains(path) ? std::expected<Kind, std::error_code>(Kind::other) : probe(path);
    }
    std::expected<std::unique_ptr<launcher::TargetDirectoryCursor>, std::error_code> enumerate(const fs::path& path) override {
        enumerated.push_back(path);
        if (open_errors.contains(path)) { return std::unexpected(open_errors.at(path)); }
        if (!listings.contains(path)) { return std::unexpected(windows_error(ERROR_FILE_NOT_FOUND)); }
        return std::make_unique<AdobeCursor>(listings.at(path), closed);
    }
    std::expected<std::optional<launcher::TargetVersion>, std::error_code> read_metadata(const fs::path& path) override {
        metadata_read.push_back(path);
        if (const auto found = metadata.find(path); found != metadata.end()) { return found->second; }
        return std::nullopt;
    }
};

const fs::path adobe_root = L"C:\\Launcher Adobe fixture";

AdobeSource adobe_source() {
    AdobeSource source;
    source.probes[adobe_root] = Kind::directory;
    source.probes[adobe_root.root_path()] = Kind::directory;
    source.listings[adobe_root / L"*"] = {};
    return source;
}

void add_program(AdobeSource& source, const fs::path& path, std::wstring product, unsigned major) {
    source.probes[path] = Kind::regular_file;
    source.metadata[path] = installation(std::move(product), {static_cast<std::uint16_t>(major), 0, 0, 0}, path.native());
}

} // namespace

TEST_CASE("Adobe CC discovers registered products on another drive without a product allowlist", "[adobe-cc]") {
    auto source = adobe_source();
    const fs::path directory = L"D:\\Creative Apps\\Adobe Animate 2027";
    const auto executable = directory / L"Animate.exe";
    const auto helper = directory / L"CrashReporter.exe";
    const auto named_helper = directory / L"AdobeCrashReport.exe";
    source.probes[directory] = Kind::directory;
    source.listings[directory / L"*"] = {Entry{L"Animate.exe", Kind::regular_file}, Entry{L"CrashReporter.exe", Kind::regular_file},
        Entry{L"AdobeCrashReport.exe", Kind::regular_file}};
    add_program(source, executable, L"Adobe Animate 2027", 27);
    add_program(source, helper, L"Adobe Animate 2027", 27);
    add_program(source, named_helper, L"Adobe AdobeCrashReport", 27);
    const std::vector<launcher::AdobeCCRegistration> registrations{
        {L"Adobe Animate", directory}, {L"Adobe Animate", directory}, {L"Animate", executable}};
    const auto result = launcher::scan_adobe_cc_detailed(adobe_root, source, {}, registrations);
    REQUIRE(result.status == Status::complete);
    REQUIRE(result.versions.size() == 1);
    CHECK(result.versions.front().executable == executable);
    CHECK(result.versions.front().name == L"Adobe Animate");
    REQUIRE(launcher::adobe_cc_latest(result.versions, L"Animate"));
    CHECK(std::ranges::count(source.metadata_read, executable) == 1);
}

TEST_CASE("Adobe CC registry locations distinguish stale paths from unavailable drives", "[adobe-cc]") {
    auto source = adobe_source();
    const std::vector<launcher::AdobeCCRegistration> registration{{L"Adobe Animate", L"D:\\Removed\\Animate"}};
    SECTION("A missing installation on an accessible drive is absent") {
        source.probes[L"D:\\"] = Kind::directory;
        CHECK(launcher::scan_adobe_cc_detailed(adobe_root, source, {}, registration).status == Status::complete);
    }
    SECTION("An unavailable drive cannot replace a previous catalog with empty") {
        CHECK(launcher::scan_adobe_cc_detailed(adobe_root, source, {}, registration).status == Status::incomplete);
    }
    SECTION("Relative registrations cannot scan the working directory") {
        const std::vector<launcher::AdobeCCRegistration> relative{{L"Adobe Animate", L"relative\\Animate"}};
        CHECK(launcher::scan_adobe_cc_detailed(adobe_root, source, {}, relative).status == Status::incomplete);
    }
}

TEST_CASE("Adobe root paths identify independent normalized catalog searches", "[adobe-cc][adobe-roots]") {
    launcher::LaunchAction action{"adobeCC", "Photoshop"};
    action.root_paths = {L"D:\\Apps\\Adobe\\", L"C:\\Apps\\Adobe", L"d:\\apps\\adobe", L"portable"};
    const auto key = launcher::adobe_cc_search_key(action, L"C:\\Config");
    REQUIRE(key.locations.size() == 3);
    CHECK(key.locations[0] == L"c:\\apps\\adobe");
    CHECK(key.locations[1] == L"c:\\config\\portable");
    CHECK(key.locations[2] == L"d:\\apps\\adobe");
    auto alias = action;
    alias.target = "Adobe Photoshop (Beta)";
    alias.root_paths = {L"portable", L"d:\\apps\\Adobe", L"c:\\apps\\adobe"};
    CHECK(launcher::adobe_cc_search_key(alias, L"C:\\Config") == key);
    alias.root_paths = {L"E:\\Other"};
    CHECK(launcher::adobe_cc_search_key(alias, L"C:\\Config") != key);
    alias = action;
    alias.target = "Adobe Animate";
    CHECK(launcher::adobe_cc_search_key(alias, L"C:\\Config") != key);
}

TEST_CASE("Adobe roots scan directory trees across drives and establish versions from executables", "[adobe-cc][adobe-roots]") {
    auto source = adobe_source();
    const auto stable = adobe_root / L"Photoshop.exe";
    source.listings[adobe_root / L"*"] = {Entry{L"Photoshop.exe", Kind::regular_file}};
    add_program(source, stable, L"Adobe Photoshop 2026", 27);
    const fs::path root = L"D:\\Creative Apps";
    const auto group = root / L"Adobe tools";
    const auto install = group / L"Photoshop Beta";
    const auto beta = install / L"Photoshop.exe";
    for (const auto& path : {root, group, install}) { source.probes[path] = Kind::directory; }
    source.listings[root / L"*"] = {Entry{L"Adobe tools", Kind::directory}};
    source.listings[group / L"*"] = {Entry{L"Photoshop Beta", Kind::directory}};
    source.listings[install / L"*"] = {Entry{L"Photoshop.exe", Kind::regular_file}, Entry{L"Plugins", Kind::directory}, Entry{L"Linked", Kind::directory, true}};
    source.probes[install / L"Plugins"] = Kind::directory;
    add_program(source, beta, L"Adobe Photoshop (Beta)", 28);
    const std::vector<launcher::AdobeCCRegistration> roots{{L"Photoshop", root, true}, {L"Photoshop", root, true}};
    const auto scan = launcher::scan_adobe_cc_detailed(adobe_root, source, {}, roots);
    REQUIRE(scan.status == Status::complete);
    REQUIRE(scan.versions.size() == 2);
    CHECK(launcher::adobe_cc_latest(scan.versions, L"Photoshop")->executable == beta);
    CHECK(launcher::adobe_cc_latest(scan.versions, L"Photoshop", false)->executable == stable);
    CHECK(std::ranges::find(source.enumerated, install / L"Plugins" / L"*") == source.enumerated.end());
    const std::vector<std::wstring> configured{root.native()};
    const auto report = launcher::parse_configuration(launcher::adobe_cc_tab_report(scan.versions, {}, configured));
    REQUIRE(report.tabs[0].items.size() == 2);
    for (const auto& item : report.tabs[0].items) { CHECK(item.launch->root_paths == std::vector<fs::path>{root}); }
}

TEST_CASE("Adobe configured roots reach native discovery and launch preparation", "[adobe-cc][adobe-roots]") {
    struct TemporaryRoot {
        fs::path path = fs::temp_directory_path() / (L"LauncherAdobeRoots-" + std::to_wstring(GetCurrentProcessId()));
        ~TemporaryRoot() { std::error_code ignored; fs::remove_all(path, ignored); }
    } temporary;
    std::array<wchar_t, 32768> module{};
    const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    REQUIRE(length > 0);
    REQUIRE(length < module.size());
    const auto probe = fs::path(std::wstring(module.data(), length)).parent_path() / L"LauncherLaunchConsoleProbe.exe";
    const auto metadata = launcher::read_target_version(probe);
    REQUIRE(metadata);
    const auto directory = temporary.path / L"arbitrary hierarchy" / L"application";
    fs::create_directories(directory);
    const auto executable = directory / (metadata->product_name + L".exe");
    fs::copy_file(probe, executable, fs::copy_options::overwrite_existing);
    const auto product = metadata->product_name;
    const auto bytes = WideCharToMultiByte(CP_UTF8, 0, product.data(), static_cast<int>(product.size()), nullptr, 0, nullptr, nullptr);
    std::string target(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, product.data(), static_cast<int>(product.size()), target.data(), bytes, nullptr, nullptr);
    launcher::LaunchAction action{"adobeCC", target};
    action.root_paths = {temporary.path};
    const auto catalog = launcher::scan_adobe_cc_for_action(action, temporary.path.parent_path());
    const auto selected = launcher::adobe_cc_latest(catalog, product);
    REQUIRE(selected);
    CHECK(fs::equivalent(selected->executable, executable));
    const auto prepared = launcher::prepare_launch_action(action, temporary.path.parent_path());
    REQUIRE(prepared.action);
    CHECK(fs::equivalent(prepared.action->target, executable));
    const auto custom_executable = directory / L"SpecialTool.exe";
    fs::copy_file(probe, custom_executable, fs::copy_options::overwrite_existing);
    action.target = "Special*.exe";
    const auto explicit_prepared = launcher::prepare_launch_action(action, temporary.path.parent_path());
    REQUIRE(explicit_prepared.action);
    CHECK(fs::equivalent(explicit_prepared.action->target, custom_executable));
}

TEST_CASE("Adobe executable patterns discover versions by filename across roots", "[adobe-cc][adobe-executable]") {
    auto source = adobe_source();
    const auto stable_dir = adobe_root / L"Custom 2025";
    const auto beta_dir = fs::path(L"D:\\Custom (Beta)");
    const auto legacy_dir = fs::path(L"E:\\Custom 2023");
    const auto stable = stable_dir / L"CustomHost.exe";
    const auto beta = beta_dir / L"CustomHostBeta.exe";
    const auto legacy = legacy_dir / L"CustomHost.exe";
    source.probes[stable_dir] = Kind::directory;
    source.listings[adobe_root / L"*"] = {Entry{L"Custom 2025", Kind::directory}};
    source.listings[stable_dir / L"*"] = {Entry{L"CustomHost.exe", Kind::regular_file}, Entry{L"Helper.exe", Kind::regular_file}};
    source.probes[beta_dir] = Kind::directory;
    source.listings[beta_dir / L"*"] = {Entry{L"CustomHostBeta.exe", Kind::regular_file}};
    source.probes[legacy_dir] = Kind::directory;
    source.listings[legacy_dir / L"*"] = {Entry{L"CustomHost.exe", Kind::regular_file}};
    add_program(source, stable, L"Different Product Name 2025", 27);
    add_program(source, beta, L"Another Product Name 2026", 28);
    add_program(source, stable_dir / L"Helper.exe", L"Different Product Name 2025", 27);
    source.metadata.at(stable).value()->has_version_resource = true;
    source.metadata.at(beta).value()->has_version_resource = true;
    source.probes[legacy] = Kind::regular_file; // This executable has no version resource.
    const std::vector<launcher::AdobeCCRegistration> roots{{L"", beta_dir, true}, {L"", legacy_dir, true}};
    const auto result = launcher::scan_adobe_cc_detailed(adobe_root, source, {}, roots, L"customhost*.EXE");
    REQUIRE(result.status == Status::complete);
    REQUIRE(result.versions.size() == 3);
    CHECK(launcher::adobe_cc_versions(result.versions, L"CUSTOMHOST?.exe").empty());
    CHECK(launcher::adobe_cc_versions(result.versions, L"CustomHost.exe").size() == 2);
    REQUIRE(launcher::adobe_cc_latest(result.versions, L"CustomHost*.exe"));
    CHECK(launcher::adobe_cc_latest(result.versions, L"CustomHost*.exe")->executable == beta);
    CHECK(launcher::adobe_cc_latest(result.versions, L"CustomHost*.exe", false)->executable == stable);
    auto snapshot = std::make_shared<const std::vector<launcher::TargetVersion>>(result.versions);
    launcher::LaunchAction catalog_action{"adobeCC", "CustomHost*.exe"};
    catalog_action.include_beta = false;
    const launcher::ItemVersionCatalog catalog(catalog_action, snapshot);
    REQUIRE(catalog.size() == 3);
    REQUIRE(catalog.latest());
    CHECK(catalog.latest()->executable == stable);
    CHECK(launcher::adobe_cc_versions(result.versions, L"Different Product Name").empty());
    CHECK(std::ranges::find(source.metadata_read, stable_dir / L"Helper.exe") == source.metadata_read.end());
    launcher::LaunchAction action{"adobeCC", "CustomHost*.exe"};
    auto key = launcher::adobe_cc_search_key(action, L"C:\\Config");
    CHECK(key.product == L"customhost*.exe");
    action.target = "CustomHost.exe";
    CHECK(key != launcher::adobe_cc_search_key(action, L"C:\\Config"));
}

TEST_CASE("Adobe exact executable target excludes helpers sharing its product name", "[adobe-cc][adobe-executable]") {
    auto source = adobe_source();
    source.listings[adobe_root / L"*"] = {Entry{L"AfterFX.exe", Kind::regular_file}, Entry{L"aerender.exe", Kind::regular_file}};
    add_program(source, adobe_root / L"AfterFX.exe", L"Adobe After Effects 2026", 26);
    add_program(source, adobe_root / L"aerender.exe", L"Adobe After Effects 2026", 26);
    const auto result = launcher::scan_adobe_cc_detailed(adobe_root, source, {}, {}, L"AfterFX.exe");
    REQUIRE(result.status == Status::complete);
    REQUIRE(result.versions.size() == 1);
    CHECK(result.versions.front().executable == adobe_root / L"AfterFX.exe");
}

TEST_CASE("Adobe recursive roots inspect main layouts before descending into large asset trees", "[adobe-cc][adobe-roots]") {
    auto source = adobe_source();
    const fs::path root = L"D:\\Creative Apps\\Illustrator";
    const auto support = root / L"Support Files";
    const auto windows = support / L"Contents" / L"Windows";
    const auto assets = support / L"Required";
    for (const auto& path : {root, support, support / L"Contents", windows, assets}) { source.probes[path] = Kind::directory; }
    source.listings[root / L"*"] = {Entry{L"Support Files", Kind::directory}};
    source.listings[support / L"*"] = {Entry{L"Required", Kind::directory}, Entry{L"Contents", Kind::directory}};
    source.listings[windows / L"*"] = {Entry{L"Illustrator.exe", Kind::regular_file}};
    add_program(source, windows / L"Illustrator.exe", L"Adobe Illustrator 2026", 30);
    const std::vector<launcher::AdobeCCRegistration> roots{{L"Illustrator", root, true}};
    const auto result = launcher::scan_adobe_cc_detailed(adobe_root, source, {}, roots);
    REQUIRE(result.status == Status::complete);
    REQUIRE(result.versions.size() == 1);
    CHECK(std::ranges::find(source.enumerated, assets / L"*") == source.enumerated.end());
}

TEST_CASE("Adobe CC report is pasteable TOML with exact discovered family and Beta names", "[adobe-cc]") {
    const std::vector catalog{
        installation(L"Adobe Animate 2027", {27, 0, 0, 0}, L"D:\\Apps\\Animate.exe"),
        installation(L"Adobe Photoshop 2026", {27, 0, 0, 0}, L"D:\\Apps\\Photoshop.exe"),
        installation(L"Adobe Photoshop 2025", {26, 0, 0, 0}, L"D:\\Older\\Photoshop.exe"),
        installation(L"Adobe Photoshop (Beta)", {28, 0, 0, 0}, L"D:\\Beta\\Photoshop.exe", true)};
    const std::vector<std::wstring> missing{L"PhotoShopz\"\n[[tabs]]"};
    const auto report = launcher::adobe_cc_tab_report(catalog, missing);
    CHECK(report.find("# No discovered AdobeCC application matches target = \"PhotoShopz\\\"\\u000A[[tabs]]\".") != std::string::npos);
    const auto config = launcher::parse_configuration(report);
    REQUIRE(config.tabs.size() == 1);
    REQUIRE(config.tabs.front().items.size() == 3);
    for (const auto& item : config.tabs.front().items) {
        REQUIRE(item.launch);
        CHECK(item.launch->handler == "adobeCC");
        CHECK((item.launch->target == "Adobe Animate" || item.launch->target == "Adobe Photoshop" || item.launch->target == "Adobe Photoshop (Beta)"));
    }
    const std::vector unusual{installation(L"Adobe \u30c6\u30b9\u30c8 \"Special\"\\Edition", {1, 0, 0, 0}, L"D:\\Other.exe")};
    const auto unicode = launcher::parse_configuration(launcher::adobe_cc_tab_report(unusual));
    CHECK(unicode.tabs.front().items.front().launch->target == "Adobe \xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88 \"Special\"\\Edition");
    CHECK(launcher::adobe_cc_tab_report({}).find("No installed Adobe applications were discovered") != std::string::npos);
}

TEST_CASE("Adobe CC matches ProductName families and executable aliases", "[adobe-cc]") {
    REQUIRE(launcher::canonical_adobe_cc_name(L"  Adobe After Effects 2026  ") == L"Adobe After Effects");
    REQUIRE(launcher::canonical_adobe_cc_name(L"aFtEr EfFeCtS (bEtA)") == L"Adobe After Effects");
    REQUIRE(launcher::canonical_adobe_cc_name(L"Adobe Illustrator 2027 (Beta)") == L"Adobe Illustrator");
    REQUIRE(launcher::canonical_adobe_cc_name(L"Adobe Premiere 2026") == L"Adobe Premiere");
    REQUIRE(launcher::canonical_adobe_cc_name(L"Premiere Pro") == L"Adobe Premiere");
    REQUIRE(launcher::canonical_adobe_cc_name(L"Adobe Creative Cloud") == L"Creative Cloud Desktop");
    REQUIRE(launcher::canonical_adobe_cc_name(L"Creative Cloud Desktop") == L"Creative Cloud Desktop");
    REQUIRE(launcher::canonical_adobe_cc_name(L"Adobe Lightroom Classic") == L"Adobe Lightroom Classic");
    REQUIRE(launcher::canonical_adobe_cc_name(L"Adobe Custom Tool") == L"Adobe Custom Tool");
    REQUIRE(launcher::canonical_adobe_cc_name(L" \t ").empty());
}

TEST_CASE("Adobe CC orders numeric product and file versions and de-duplicates paths", "[adobe-cc]") {
    const std::vector installed{
        installation(L"Adobe Audition 2025", {25, 6, 6, 0}, L"C:\\Adobe\\old.exe"),
        installation(L"Adobe Audition 2026", {26, 9, 0, 0}, L"C:\\Adobe\\minor-nine.exe"),
        installation(L"Adobe Audition 2026", {26, 10, 0, 0}, L"C:\\Adobe\\new.exe", false, {26, 10, 0, 3}),
        installation(L"Adobe Audition 2026", {26, 10, 0, 0}, L"C:\\Adobe\\rebuilt.exe", false, {26, 10, 0, 4}),
        installation(L"Adobe Audition 2026", {26, 10, 0, 0}, L"c:\\ADOBE\\rebuilt.exe", false, {26, 10, 0, 4}),
        installation(L"Adobe Photoshop 2026", {27, 11, 0, 26}, L"C:\\Adobe\\Photoshop.exe")
    };
    const auto versions = launcher::adobe_cc_versions(installed, L"audition");
    REQUIRE(versions.size() == 4);
    REQUIRE(versions[0].executable.filename() == L"rebuilt.exe");
    REQUIRE(versions[1].executable.filename() == L"new.exe");
    REQUIRE(versions[2].executable.filename() == L"minor-nine.exe");
    REQUIRE(versions[3].executable.filename() == L"old.exe");
    const auto latest = launcher::adobe_cc_latest(installed, L"Adobe Audition");
    REQUIRE(latest.has_value());
    REQUIRE(latest->executable.filename() == L"rebuilt.exe");
    REQUIRE_FALSE(launcher::adobe_cc_latest(installed, L"Bridge").has_value());
    REQUIRE(launcher::adobe_cc_versions(installed, L"").empty());
}

TEST_CASE("Adobe CC selects the newest edition including Beta unless stable is requested", "[adobe-cc]") {
    const std::vector installed{
        installation(L"Adobe Premiere 2026", {26, 5, 2, 5}, L"C:\\Adobe\\stable.exe"),
        installation(L"Adobe Premiere (Beta)", {27, 1, 0, 13}, L"C:\\Adobe\\beta.exe", true),
        installation(L"Adobe Premiere (Beta)", {27, 0, 0, 2}, L"C:\\Adobe\\older-beta.exe", true)
    };
    const auto versions = launcher::adobe_cc_versions(installed, L"Premiere Pro");
    REQUIRE(versions.size() == 3);
    REQUIRE(versions.front().beta);
    REQUIRE(launcher::adobe_cc_latest(installed, L"Premiere Pro")->beta);
    const auto stable = launcher::adobe_cc_latest(installed, L"Premiere Pro", false);
    REQUIRE(stable.has_value());
    REQUIRE(stable->executable.filename() == L"stable.exe");
    const auto beta = launcher::adobe_cc_latest(installed, L"Adobe Premiere Pro (Beta)");
    REQUIRE(beta.has_value());
    REQUIRE(beta->executable.filename() == L"beta.exe");
    REQUIRE(launcher::adobe_cc_versions(installed, L"Premiere (Beta)").size() == 2);
    REQUIRE(launcher::adobe_cc_latest(installed, L"Premiere", true)->beta);
    const std::vector beta_only{installed[1]};
    REQUIRE_FALSE(launcher::adobe_cc_latest(beta_only, L"Premiere", false).has_value());
    REQUIRE(launcher::adobe_cc_latest(beta_only, L"Premiere")->beta);
    REQUIRE(launcher::adobe_cc_latest(beta_only, L"Premiere (Beta)").has_value());
}

TEST_CASE("Adobe CC scanner tolerates absent roots and malformed main executable resources", "[adobe-cc]") {
    struct Fixture {
        std::filesystem::path root = std::filesystem::temp_directory_path() /
            (L"Launcher-adobe-scan-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()));
        ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    } fixture;
    REQUIRE(launcher::scan_adobe_cc(fixture.root).empty());
    REQUIRE(std::filesystem::create_directories(fixture.root / L"Adobe Photoshop 2026"));
    {
        std::ofstream invalid(fixture.root / L"Adobe Photoshop 2026" / L"Photoshop.exe", std::ios::binary);
        REQUIRE(invalid.good());
        invalid << "This fixture is not a PE executable.";
    }
    REQUIRE(launcher::scan_adobe_cc(fixture.root).empty());
    REQUIRE(launcher::scan_adobe_cc(fixture.root / L"Adobe Photoshop 2026" / L"Photoshop.exe").empty());
    const auto program = fixture.root / L"Adobe Photoshop 2026" / L"Photoshop.exe";
    struct ReadLock {
        HANDLE value;
        ~ReadLock() { if (value != INVALID_HANDLE_VALUE) { CloseHandle(value); } }
    } lock{CreateFileW(program.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    REQUIRE(lock.value != INVALID_HANDLE_VALUE);
    const auto unreadable = launcher::scan_adobe_cc_detailed(fixture.root);
    REQUIRE(unreadable.status == Status::incomplete);
    REQUIRE(unreadable.diagnostics.size() == 1);
    CHECK(unreadable.diagnostics.front().error.value() == ERROR_SHARING_VIOLATION);
    REQUIRE_THROWS_AS(launcher::scan_adobe_cc(fixture.root), fs::filesystem_error);
}

TEST_CASE("Adobe structured discovery preserves fixed layouts metadata selection and link exclusions", "[adobe-cc][target-scan]") {
    auto source = adobe_source();
    source.listings[adobe_root / L"*"] = {
        Entry{L".", Kind::directory}, Entry{L"..", Kind::directory},
        Entry{L"Photoshop.exe", Kind::regular_file}, Entry{L"After Effects (Beta)", Kind::directory},
        Entry{L"Desktop", Kind::directory}, Entry{L"Reader", Kind::directory},
        Entry{L"Linked Photoshop", Kind::directory, true}, Entry{L"Bridge.exe", Kind::regular_file}};
    add_program(source, adobe_root / L"Photoshop.exe", L"Adobe Photoshop 2026", 27);
    // The non-main filename must not be opened even if metadata describes Adobe.
    add_program(source, adobe_root / L"Bridge.exe", L"Adobe Bridge", 20);
    source.probes[adobe_root / L"Linked Photoshop"] = Kind::directory;
    source.links.insert(adobe_root / L"Linked Photoshop");
    for (const auto child : {L"After Effects (Beta)", L"Desktop", L"Reader"}) {
        source.probes[adobe_root / child] = Kind::directory;
        source.listings[adobe_root / child / L"*"] = {};
    }
    const auto effects = adobe_root / L"After Effects (Beta)" / L"Support Files";
    source.probes[effects] = Kind::directory;
    source.probes[effects / L"Contents"] = Kind::directory;
    source.probes[effects / L"Contents" / L"Windows"] = Kind::directory;
    source.listings[effects / L"*"] = {};
    source.listings[effects / L"Contents" / L"Windows" / L"*"] = {
        Entry{L"AfterFX.exe", Kind::regular_file}, Entry{L"Photoshop.exe", Kind::regular_file, true}};
    add_program(source, effects / L"Contents" / L"Windows" / L"AfterFX.exe", L"Adobe After Effects 2027", 28);
    const auto cloud = adobe_root / L"Desktop" / L"ACC";
    source.probes[cloud] = Kind::directory;
    source.listings[cloud / L"*"] = {Entry{L"Creative Cloud.exe", Kind::regular_file}};
    add_program(source, cloud / L"Creative Cloud.exe", L"Adobe Creative Cloud", 7);
    const auto acrobat = adobe_root / L"Reader" / L"Acrobat";
    source.probes[acrobat] = Kind::directory;
    source.listings[acrobat / L"*"] = {Entry{L"Acrobat.exe", Kind::regular_file}};
    add_program(source, acrobat / L"Acrobat.exe", L"Adobe Acrobat", 25);
    const auto result = launcher::scan_adobe_cc_detailed(adobe_root, source);
    REQUIRE(result.status == Status::complete);
    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.versions.size() == 4);
    CHECK(launcher::adobe_cc_latest(result.versions, L"After Effects")->beta);
    CHECK(launcher::adobe_cc_latest(result.versions, L"After Effects")->executable == effects / L"Contents" / L"Windows" / L"AfterFX.exe");
    CHECK(launcher::adobe_cc_latest(result.versions, L"Creative Cloud Desktop")->name == L"Creative Cloud Desktop");
    CHECK(launcher::adobe_cc_latest(result.versions, L"Acrobat")->executable == acrobat / L"Acrobat.exe");
    CHECK(source.metadata_read.size() == 4);
    CHECK(*source.closed == source.enumerated.size());
}

TEST_CASE("Adobe structured root failures distinguish inaccessible hierarchy from absence", "[adobe-cc][target-scan]") {
    auto source = adobe_source();
    SECTION("missing beneath a reachable parent is complete empty") {
        source.probes.erase(adobe_root);
        const auto result = launcher::scan_adobe_cc_detailed(adobe_root, source);
        CHECK(result.status == Status::complete);
        CHECK(result.versions.empty());
        CHECK(result.diagnostics.empty());
    }
    SECTION("unreadable root is failed") {
        source.probes[adobe_root] = std::unexpected(windows_error(ERROR_ACCESS_DENIED));
        const auto result = launcher::scan_adobe_cc_detailed(adobe_root, source);
        REQUIRE(result.status == Status::failed);
        REQUIRE(result.diagnostics.size() == 1);
        CHECK(result.diagnostics.front().operation == Operation::probe);
        CHECK(result.diagnostics.front().error.value() == ERROR_ACCESS_DENIED);
    }
    SECTION("missing descendants cannot hide an unavailable drive") {
        source.probes.erase(adobe_root);
        source.probes[adobe_root.root_path()] = std::unexpected(windows_error(ERROR_NOT_READY));
        const auto result = launcher::scan_adobe_cc_detailed(adobe_root, source);
        REQUIRE(result.status == Status::failed);
        REQUIRE(result.diagnostics.size() == 1);
        CHECK(result.diagnostics.front().error.value() == ERROR_NOT_READY);
    }
    SECTION("a missing network share is not a complete empty directory") {
        const fs::path remote = L"\\\\fixture.invalid\\missing\\Adobe";
        const auto result = launcher::scan_adobe_cc_detailed(remote, source);
        CHECK(result.status == Status::failed);
        CHECK_FALSE(result.diagnostics.empty());
    }
    SECTION("root links and files are excluded without enumeration") {
        source.links.insert(adobe_root);
        CHECK(launcher::scan_adobe_cc_detailed(adobe_root, source).status == Status::complete);
        CHECK(source.enumerated.empty());
    }
    SECTION("wildcards are not an Adobe root") {
        const auto result = launcher::scan_adobe_cc_detailed(adobe_root / L"*", source);
        CHECK(result.status == Status::failed);
        REQUIRE(result.diagnostics.size() == 1);
        CHECK(result.diagnostics.front().operation == Operation::normalize);
        CHECK(source.enumerated.empty());
    }
}

TEST_CASE("Adobe partial coverage preserves observed evidence without authoritative success", "[adobe-cc][target-scan]") {
    auto source = adobe_source();
    source.listings[adobe_root / L"*"] = {Entry{L"Photoshop.exe", Kind::regular_file}};
    add_program(source, adobe_root / L"Photoshop.exe", L"Adobe Photoshop", 27);
    SECTION("cursor fails after an observed installation") {
        source.listings[adobe_root / L"*"].push_back(windows_error(ERROR_NETNAME_DELETED));
    }
    SECTION("a sibling installation cannot be enumerated") {
        const auto child = adobe_root / L"Unreadable";
        source.listings[adobe_root / L"*"].push_back(Entry{L"Unreadable", Kind::directory});
        source.probes[child] = Kind::directory;
        source.open_errors[child / L"*"] = windows_error(ERROR_ACCESS_DENIED);
    }
    const auto result = launcher::scan_adobe_cc_detailed(adobe_root, source);
    REQUIRE(result.status == Status::incomplete);
    REQUIRE(result.versions.size() == 1);
    CHECK(result.versions.front().executable == adobe_root / L"Photoshop.exe");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().operation == Operation::enumerate);
    CHECK(*source.closed == source.enumerated.size() - source.open_errors.size());
}

TEST_CASE("Adobe discovery failures retain full Windows paths beyond 1024 units", "[adobe-cc][target-scan]") {
    AdobeSource source;
    auto directory = adobe_root;
    // A long installation root can have ordinary-sized directory components.
    for (unsigned index = 0; index < 6; ++index) { directory /= std::wstring(200, static_cast<wchar_t>(L'a' + index)); }
    const auto pattern = directory / L"*";
    const auto executable = directory / L"Photoshop.exe";
    REQUIRE(directory.native().size() > 1'024);
    const auto error = windows_error(ERROR_CRC);
    source.probes[directory] = Kind::directory;
    source.probes[directory.root_path()] = Kind::directory;
    source.listings[pattern] = {Entry{L"Photoshop.exe", Kind::regular_file}};
    add_program(source, executable, L"Adobe Photoshop", 27);
    auto expected_path = pattern;
    auto operation = Operation::enumerate;
    auto status = Status::incomplete;
    SECTION("opening a directory listing") { source.open_errors[pattern] = error; }
    SECTION("advancing a directory listing") { source.listings[pattern] = {error}; }
    SECTION("probing an installation root") {
        source.probes[directory] = std::unexpected(error);
        expected_path = directory;
        operation = Operation::probe;
        status = Status::failed;
    }
    SECTION("reading concrete file metadata") {
        source.metadata[executable] = std::unexpected(error);
        expected_path = executable;
        operation = Operation::metadata;
    }
    const auto result = launcher::scan_adobe_cc_detailed(directory, source);
    CHECK(result.status == status);
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().path == expected_path);
    CHECK(result.diagnostics.front().operation == operation);
    CHECK(result.diagnostics.front().error == error);
    CHECK(result.omitted_diagnostics == 0);
    CHECK(*source.closed == source.enumerated.size() - source.open_errors.size());
}

TEST_CASE("Adobe metadata absence is optional but unreadable ranking is incomplete", "[adobe-cc][target-scan]") {
    auto source = adobe_source();
    source.listings[adobe_root / L"*"] = {Entry{L"Photoshop.exe", Kind::regular_file}, Entry{L"Illustrator.exe", Kind::regular_file}};
    add_program(source, adobe_root / L"Photoshop.exe", L"Adobe Photoshop", 27);
    const auto illustrator = adobe_root / L"Illustrator.exe";
    source.probes[illustrator] = Kind::regular_file;
    SECTION("absent or malformed resource is skipped") {
        const auto result = launcher::scan_adobe_cc_detailed(adobe_root, source);
        CHECK(result.status == Status::complete);
        CHECK(result.versions.size() == 1);
    }
    SECTION("sharing violation is an incomplete observation") {
        source.metadata[illustrator] = std::unexpected(windows_error(ERROR_SHARING_VIOLATION));
        const auto result = launcher::scan_adobe_cc_detailed(adobe_root, source);
        CHECK(result.status == Status::incomplete);
        CHECK(result.versions.size() == 1);
        REQUIRE(result.diagnostics.size() == 1);
        CHECK(result.diagnostics.front().operation == Operation::metadata);
        CHECK(result.diagnostics.front().error.value() == ERROR_SHARING_VIOLATION);
    }
    SECTION("a missing resource path with a still present file is unreliable") {
        source.metadata[illustrator] = std::unexpected(windows_error(ERROR_FILE_NOT_FOUND));
        const auto result = launcher::scan_adobe_cc_detailed(adobe_root, source);
        CHECK(result.status == Status::incomplete);
        REQUIRE(result.diagnostics.size() == 1);
        CHECK(result.diagnostics.front().operation == Operation::metadata);
    }
}

TEST_CASE("Adobe structured scans apply finite work target and diagnostic budgets", "[adobe-cc][target-scan]") {
    auto source = adobe_source();
    source.listings[adobe_root / L"*"] = {Entry{L"Photoshop.exe", Kind::regular_file}, Entry{L"Illustrator.exe", Kind::regular_file}};
    add_program(source, adobe_root / L"Photoshop.exe", L"Adobe Photoshop", 27);
    add_program(source, adobe_root / L"Illustrator.exe", L"Adobe Illustrator", 31);
    launcher::TargetScanLimits limits;
    auto expected = launcher::TargetScanLimit::targets;
    SECTION("targets") { limits.max_targets = 1; }
    SECTION("operations") { limits.max_operations = 0; expected = launcher::TargetScanLimit::operations; }
    SECTION("entries") { limits.max_entries = 0; expected = launcher::TargetScanLimit::entries; }
    SECTION("branches") { limits.max_branches = 0; expected = launcher::TargetScanLimit::branches; }
    SECTION("storage") { limits.max_storage_bytes = 0; expected = launcher::TargetScanLimit::storage_bytes; }
    SECTION("path") { limits.max_path_units = 0; expected = launcher::TargetScanLimit::path_units; }
    SECTION("metadata") { limits.max_metadata_units = 1; expected = launcher::TargetScanLimit::metadata_units; }
    SECTION("components") { limits.max_components = 0; expected = launcher::TargetScanLimit::components; }
    SECTION("no diagnostic storage still fails honestly") { limits.max_targets = 0; limits.max_diagnostics = 0; }
    const auto result = launcher::scan_adobe_cc_detailed(adobe_root, source, limits);
    CHECK(result.status == Status::incomplete);
    if (limits.max_diagnostics && limits.max_storage_bytes) {
        REQUIRE(result.diagnostics.size() == 1);
        CHECK(result.diagnostics.front().operation == Operation::budget);
        CHECK(result.diagnostics.front().limit == expected);
    } else {
        CHECK(result.diagnostics.empty());
        CHECK(result.omitted_diagnostics == 1);
    }
    CHECK(result.exhausted_limit == expected);
}

TEST_CASE("Adobe distinct target limits admit repeated fixed layouts and case-insensitive paths once", "[adobe-cc][target-scan]") {
    auto source = adobe_source();
    const auto support = adobe_root / L"Support Files";
    source.probes[support] = Kind::directory;
    source.listings[adobe_root / L"*"] = {Entry{L"Support Files", Kind::directory}};
    source.listings[support / L"*"] = {Entry{L"Photoshop.exe", Kind::regular_file}, Entry{L"PHOTOSHOP.EXE", Kind::regular_file}};
    add_program(source, support / L"Photoshop.exe", L"Adobe Photoshop", 27);
    launcher::TargetScanLimits limits;
    limits.max_targets = 1;
    const auto result = launcher::scan_adobe_cc_detailed(adobe_root, source, limits);
    CHECK(result.status == Status::complete);
    CHECK(result.diagnostics.empty());
    REQUIRE(result.versions.size() == 1);
    CHECK(result.versions.front().executable == support / L"Photoshop.exe");
    REQUIRE(source.metadata_read.size() == 1);
    CHECK(source.metadata_read.front() == support / L"Photoshop.exe");
    CHECK(*source.closed == source.enumerated.size());
}

TEST_CASE("Adobe provider budget failures stop work and remain incomplete at the root", "[adobe-cc][target-scan]") {
    auto source = adobe_source();
    const auto exhaustion = launcher::make_target_scan_limit_error(launcher::TargetScanLimit::resource_bytes);
    SECTION("probe") { source.probes[adobe_root] = std::unexpected(exhaustion); }
    SECTION("cursor") { source.listings[adobe_root / L"*"] = {exhaustion}; }
    SECTION("metadata") {
        source.listings[adobe_root / L"*"] = {Entry{L"Photoshop.exe", Kind::regular_file}, Entry{L"Illustrator.exe", Kind::regular_file}};
        add_program(source, adobe_root / L"Photoshop.exe", L"Adobe Photoshop", 27);
        add_program(source, adobe_root / L"Illustrator.exe", L"Adobe Illustrator", 31);
        source.metadata[adobe_root / L"Photoshop.exe"] = std::unexpected(exhaustion);
    }
    const auto result = launcher::scan_adobe_cc_detailed(adobe_root, source);
    CHECK(result.status == Status::incomplete);
    CHECK(result.exhausted_limit == launcher::TargetScanLimit::resource_bytes);
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().operation == Operation::budget);
    CHECK(source.metadata_read.size() <= 1);
    CHECK(*source.closed == source.enumerated.size());
}

TEST_CASE("Adobe CC selection excludes generic targets in the shared catalog", "[adobe-cc]") {
    auto stable = installation(L"Adobe Photoshop 2026", {27, 0, 0, 0}, L"C:\\Adobe\\stable.exe");
    auto beta = installation(L"Adobe Photoshop (Beta)", {28, 0, 0, 0}, L"C:\\Adobe\\beta.exe", true);
    auto generic_stable = installation(L"Adobe Photoshop 2026", {29, 0, 0, 0}, L"C:\\Adobe\\generic-stable.exe");
    generic_stable.target_pattern = L"C:\\Adobe\\generic-*.exe";
    auto generic_beta = installation(L"Adobe Photoshop (Beta)", {30, 0, 0, 0}, L"C:\\Adobe\\generic-beta.exe");
    generic_beta.target_pattern = generic_stable.target_pattern;
    // Generic discovery does not assign Adobe's Beta flag. A generic record
    // with the same product family must not bypass the stable-only selection.
    const std::vector catalog{stable, beta, generic_stable, generic_beta};
    const auto versions = launcher::adobe_cc_versions(catalog, L"Photoshop");
    REQUIRE(versions.size() == 2);
    CHECK(versions[0].executable == beta.executable);
    CHECK(versions[1].executable == stable.executable);
    REQUIRE(launcher::adobe_cc_latest(catalog, L"Photoshop", false).has_value());
    CHECK(launcher::adobe_cc_latest(catalog, L"Photoshop", false)->executable == stable.executable);
    REQUIRE(launcher::adobe_cc_latest(catalog, L"Photoshop").has_value());
    CHECK(launcher::adobe_cc_latest(catalog, L"Photoshop")->executable == beta.executable);
    REQUIRE(launcher::adobe_cc_versions(catalog, L"Photoshop (Beta)").size() == 1);
    CHECK(launcher::adobe_cc_versions(catalog, L"Photoshop (Beta)").front().executable == beta.executable);
}

TEST_CASE("Adobe CC prepares the installed stable application with configured launch options", "[adobe-cc]") {
    const auto catalog = launcher::scan_adobe_cc();
    const auto latest = launcher::adobe_cc_latest(catalog, L"Adobe After Effects", false);
    if (!latest) { SKIP("After Effects is not installed on this test machine."); }
    const launcher::LaunchAction action{"adobeCC", "Adobe After Effects", "\"project with spaces.aep\"",
        "Projects", launcher::LaunchShow::minimized, false};
    const auto base = std::filesystem::temp_directory_path();
    const auto prepared = launcher::prepare_launch_action(action, base);
    REQUIRE(prepared.result.success);
    REQUIRE(prepared.action.has_value());
    REQUIRE(prepared.action->target == latest->executable.wstring());
    REQUIRE(prepared.action->arguments == L"\"project with spaces.aep\"");
    REQUIRE(prepared.action->working_directory == (base / L"Projects").lexically_normal().wstring());
    REQUIRE(prepared.action->show == launcher::LaunchShow::minimized);
    REQUIRE_FALSE(latest->beta);
    REQUIRE(std::filesystem::is_regular_file(latest->executable));
    auto beta_action = action;
    beta_action.include_beta = true;
    const auto including_beta = launcher::prepare_launch_action(beta_action, base);
    REQUIRE(including_beta.action.has_value());
    REQUIRE(including_beta.action->target ==
        launcher::adobe_cc_latest(catalog, L"Adobe After Effects", true)->executable.wstring());
    const auto beta = launcher::adobe_cc_latest(catalog, L"Adobe After Effects (Beta)");
    if (beta) {
        beta_action.include_beta = false;
        beta_action.target = "Adobe After Effects (Beta)";
        const auto explicit_beta = launcher::prepare_launch_action(beta_action, base);
        REQUIRE(explicit_beta.action.has_value());
        REQUIRE(explicit_beta.action->target == beta->executable.wstring());
    }
    const auto missing = launcher::prepare_launch_action(
        launcher::LaunchAction{"adobeCC", "Adobe nonexistent fixture product"}, base);
    REQUIRE_FALSE(missing.action.has_value());
    REQUIRE(missing.result.error_code == ERROR_FILE_NOT_FOUND);
    if (std::filesystem::is_regular_file(latest->executable.parent_path() / L"aerender.exe")) {
        auto exact = action;
        exact.target = "aerender.exe";
        const auto renderer = launcher::prepare_launch_action(exact, base);
        REQUIRE(renderer.action);
        CHECK(std::filesystem::path(renderer.action->target).filename() == L"aerender.exe");
    }
}
