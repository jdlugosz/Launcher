#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/target_version_scan.hpp"
#include "launcher/target_version_cache.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Source = launcher::TargetVersionSource;
using Kind = launcher::TargetFileKind;
using Status = launcher::TargetScanStatus;
using Operation = launcher::TargetScanOperation;
using Entry = launcher::TargetDirectoryEntry;
using ProbeResult = std::expected<Kind, std::error_code>;
using MetadataResult = std::expected<std::optional<launcher::TargetVersion>, std::error_code>;
using Step = std::variant<Entry, std::error_code>;
using namespace std::chrono_literals;

std::error_code windows_error(DWORD code) { return {static_cast<int>(code), std::system_category()}; }

struct CursorCounts final {
    unsigned opened = 0;
    unsigned closed = 0;
    unsigned next = 0;
};

class ScriptedCursor final : public launcher::TargetDirectoryCursor {
public:
    ScriptedCursor(std::vector<Step> steps, std::shared_ptr<CursorCounts> counts)
        : steps_(std::move(steps)), counts_(std::move(counts)) { ++counts_->opened; }
    ~ScriptedCursor() override { ++counts_->closed; }

    std::expected<std::optional<Entry>, std::error_code> next() override {
        ++counts_->next;
        if (position_ == steps_.size()) { return std::nullopt; }
        const auto& step = steps_[position_++];
        if (const auto* error = std::get_if<std::error_code>(&step)) { return std::unexpected(*error); }
        return std::get<Entry>(step);
    }

private:
    std::vector<Step> steps_;
    std::shared_ptr<CursorCounts> counts_;
    std::size_t position_ = 0;
};

// These adapters exercise the production traversal rather than reproducing
// wildcard matching. Native fixtures separately verify FindFirst/Next grammar.
class ScriptedSource final : public Source {
public:
    std::map<fs::path, ProbeResult> probes;
    std::map<fs::path, std::deque<ProbeResult>> probe_steps;
    std::map<fs::path, std::vector<Step>> listings;
    std::map<fs::path, std::error_code> open_errors;
    std::map<fs::path, MetadataResult> metadata;
    std::vector<fs::path> probed;
    std::vector<fs::path> enumerated;
    std::vector<fs::path> metadata_read;
    std::function<void(const fs::path&)> before_metadata;
    std::shared_ptr<CursorCounts> cursors = std::make_shared<CursorCounts>();

    ProbeResult probe(const fs::path& path) override {
        probed.push_back(path);
        if (const auto found = probe_steps.find(path); found != probe_steps.end() && !found->second.empty()) {
            auto reply = std::move(found->second.front());
            found->second.pop_front();
            return reply;
        }
        if (const auto found = probes.find(path); found != probes.end()) { return found->second; }
        return Kind::directory; // Reachable root and literal directory prefixes.
    }

    std::expected<std::unique_ptr<launcher::TargetDirectoryCursor>, std::error_code> enumerate(const fs::path& pattern) override {
        enumerated.push_back(pattern);
        if (const auto found = open_errors.find(pattern); found != open_errors.end()) {
            return std::unexpected(found->second);
        }
        const auto found = listings.find(pattern);
        if (found == listings.end()) { throw std::runtime_error("Unexpected scripted enumeration"); }
        return std::make_unique<ScriptedCursor>(found->second, cursors);
    }

    MetadataResult read_metadata(const fs::path& path) override {
        metadata_read.push_back(path);
        if (before_metadata) { before_metadata(path); }
        if (const auto found = metadata.find(path); found != metadata.end()) { return found->second; }
        return std::nullopt;
    }
};

constexpr std::wstring_view root = L"C:\\Launcher scripted fixtures";

fs::path fixture_path(std::wstring_view suffix) { return fs::path(root) / suffix; }

void add_version(ScriptedSource& source, std::wstring_view directory) {
    source.probes[fixture_path(std::wstring(directory) + L"\\tool.exe")] = Kind::regular_file;
}

ScriptedSource successful_source() {
    ScriptedSource source;
    source.listings[fixture_path(L"Version *")] = {
        Entry{L"Version 5.9", Kind::directory}, Entry{L"Version 5.10", Kind::directory}};
    add_version(source, L"Version 5.9");
    add_version(source, L"Version 5.10");
    return source;
}

void check_limit(const launcher::TargetVersionScanResult& result, launcher::TargetScanLimit limit) {
    CHECK(result.status == Status::incomplete);
    CHECK(result.exhausted_limit == limit);
    if (!result.diagnostics.empty()) {
        CHECK(result.diagnostics.back().operation == Operation::budget);
        CHECK(result.diagnostics.back().limit == limit);
        CHECK(launcher::target_scan_limit(result.diagnostics.back().error) == limit);
        CHECK_FALSE(result.diagnostics.back().error.message().empty());
    }
}

launcher::TargetVersion known_version() {
    launcher::TargetVersion version;
    version.executable = fixture_path(L"Known 4.0\\tool.exe");
    version.product_version = L"4.0";
    version.target_pattern = fixture_path(L"Version *\\tool.exe").wstring();
    return version;
}

// Waiting for the last capture to be destroyed observes the worker's completed
// publication; tests need no timed polling or arbitrary delay to collect it.
std::shared_ptr<int> completion_token(const std::shared_ptr<std::promise<void>>& completion) {
    return std::shared_ptr<int>(new int(0), [completion](int* value) {
        delete value;
        completion->set_value();
    });
}

launcher::TargetVersionCache::RefreshResult refresh_and_collect(launcher::TargetVersionCache& cache,
        launcher::TargetVersionCache::Scanner scanner) {
    const auto completion = std::make_shared<std::promise<void>>();
    auto finished = completion->get_future();
    auto token = completion_token(completion);
    REQUIRE(cache.refresh([scanner = std::move(scanner), token] { return scanner(); }));
    token.reset();
    REQUIRE(finished.wait_for(2s) == std::future_status::ready);
    auto result = cache.poll();
    REQUIRE(result.has_value());
    return std::move(*result);
}

struct Gate final {
    std::promise<void> entered;
    std::promise<void> permission;
    std::shared_future<void> allowed = permission.get_future().share();
    bool released = false;
    void open() {
        if (!released) { released = true; permission.set_value(); }
    }
};

struct ReleaseGuard final {
    std::shared_ptr<Gate> gate;
    ~ReleaseGuard() { gate->open(); }
};

class NativeFixture final {
public:
    NativeFixture() : parent_(fs::weakly_canonical(fs::temp_directory_path())) {
        static std::atomic<unsigned> sequence = 0;
        root_ = parent_ / (L"LauncherStructuredScan-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence++));
        if (!fs::create_directory(root_)) { throw std::runtime_error("Cannot create native scan fixture"); }
    }
    ~NativeFixture() {
        std::error_code error;
        const auto resolved = fs::weakly_canonical(root_, error);
        if (!error && resolved == root_ && resolved.parent_path() == parent_) { fs::remove_all(root_, error); }
    }
    fs::path path(std::wstring_view name) const { return root_ / name; }
    fs::path file(std::wstring_view name, std::string_view content) const {
        const auto destination = path(name);
        fs::create_directories(destination.parent_path());
        std::ofstream output(destination, std::ios::binary);
        output << content;
        output.close();
        if (!output) { throw std::runtime_error("Cannot write native scan fixture"); }
        return destination;
    }

private:
    fs::path parent_;
    fs::path root_;
};

fs::path versioned_probe() {
    std::wstring module(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (count == 0 || count >= module.size()) { throw std::runtime_error("Cannot locate test executable"); }
    module.resize(count);
    return fs::path(module).parent_path() / L"LauncherLaunchGuiProbe.exe";
}

void create_test_link(const fs::path& link, const fs::path& target, bool directory) {
    const DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE | (directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);
    if (CreateSymbolicLinkW(link.c_str(), target.c_str(), flags)) { return; }
    const DWORD error = GetLastError();
    if (error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_PARAMETER) {
        SKIP("Unprivileged symbolic links are unavailable on this machine");
    }
    throw fs::filesystem_error("Cannot create scan fixture symbolic link", link, target, windows_error(error));
}

} // namespace

TEST_CASE("Target pattern normalization simplifies literals without removing wildcard parent traversal", "[target-scan]") {
    const auto normalized = launcher::normalize_target_version_pattern(L"C:/Launcher absent fixture/Discard/.././Version */../Version ?/./tool.exe");
    CHECK(normalized.native() == L"C:\\Launcher absent fixture\\Version *\\..\\Version ?\\tool.exe");
    CHECK(launcher::normalize_target_version_pattern(normalized) == normalized);
    CHECK(launcher::normalize_target_version_pattern(L"C:\\..\\..\\Version *\\tool.exe").native() == L"C:\\Version *\\tool.exe");
}

TEST_CASE("Target pattern normalization preserves drive UNC and extended roots", "[target-scan]") {
    const std::vector<std::pair<std::wstring, std::wstring>> cases{
        {LR"(C:\Tools\Unused\..\Version *\tool.exe)", LR"(C:\Tools\Version *\tool.exe)"},
        {LR"(\\unavailable-server\share\Unused\..\Version *\tool.exe)", LR"(\\unavailable-server\share\Version *\tool.exe)"},
        {LR"(\\?\C:\Tools\Unused\..\Version *\tool.exe)", LR"(\\?\C:\Tools\Version *\tool.exe)"},
        {LR"(\\?\UNC\unavailable-server\share\Unused\..\Version *\tool.exe)", LR"(\\?\UNC\unavailable-server\share\Version *\tool.exe)"}
    };
    for (const auto& [input, expected] : cases) {
        const auto normalized = launcher::normalize_target_version_pattern(input);
        CHECK(normalized.native() == expected);
        CHECK(launcher::normalize_target_version_pattern(normalized) == normalized);
    }
}

TEST_CASE("Target pattern normalization anchors relative patterns while preserving wildcard semantics", "[target-scan]") {
    auto current = fs::current_path();
    current.make_preferred();
    const auto expected = current / LR"(Version *\..\tool?.exe)";
    CHECK(launcher::normalize_target_version_pattern(LR"(.\Unused\..\Version *\..\tool?.exe)") == expected);
    const auto drive_relative = current.root_name().native() + LR"(Version *\..\tool?.exe)";
    CHECK(launcher::normalize_target_version_pattern(drive_relative) == expected);
    CHECK(fs::current_path() == current);
}

TEST_CASE("Target pattern normalization rejects an empty identity or embedded NUL", "[target-scan]") {
    fs::path pattern;
    SECTION("An empty pattern is invalid") {}
    SECTION("An embedded NUL cannot become a shorter search") {
        auto text = fixture_path(L"Version *\\tool.exe").native();
        text.insert(text.begin() + 4, L'\0');
        pattern = std::move(text);
    }
    try {
        (void)launcher::normalize_target_version_pattern(pattern);
        FAIL("Invalid search identity was accepted");
    } catch (const fs::filesystem_error& error) {
        CHECK(error.path1() == pattern);
        CHECK(error.code() == std::make_error_code(std::errc::invalid_argument));
    }
}

TEST_CASE("Structured target scans retain fallback ordering and own every directory cursor", "[target-scan]") {
    auto source = successful_source();
    const auto pattern = fixture_path(L"Version *\\tool.exe");
    const auto result = launcher::scan_target_versions_detailed(pattern, source);
    REQUIRE(result.status == Status::complete);
    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.versions.size() == 2);
    CHECK(result.versions[0].executable == fixture_path(L"Version 5.10\\tool.exe"));
    CHECK(result.versions[1].executable == fixture_path(L"Version 5.9\\tool.exe"));
    CHECK(result.versions[0].target_pattern == pattern.wstring());
    CHECK(result.versions[0].path_label == L"Version 5.10");
    CHECK_FALSE(result.versions[0].has_version_resource);
    REQUIRE(source.cursors->opened == 1);
    CHECK(source.cursors->closed == source.cursors->opened);
    CHECK(source.metadata_read.size() == 2);
}

TEST_CASE("Structured target scans distinguish expected absence from unreachable storage", "[target-scan]") {
    ScriptedSource source;
    const auto wildcard = fixture_path(L"Version *");
    const auto pattern = wildcard / L"tool.exe";

    SECTION("An empty enumeration is a complete empty observation") {
        source.listings[wildcard] = {};
    }
    SECTION("No wildcard matches in a reachable directory is a complete empty observation") {
        source.open_errors[wildcard] = windows_error(ERROR_FILE_NOT_FOUND);
    }
    SECTION("A missing literal target is a complete empty observation") {
        source.listings[wildcard] = {Entry{L"Version 5.10", Kind::directory}};
        source.probes[fixture_path(L"Version 5.10\\tool.exe")] = std::unexpected(windows_error(ERROR_FILE_NOT_FOUND));
    }
    const auto result = launcher::scan_target_versions_detailed(pattern, source);
    CHECK(result.status == Status::complete);
    CHECK(result.versions.empty());
    CHECK(result.diagnostics.empty());
    CHECK(source.cursors->opened == source.cursors->closed);
}

TEST_CASE("Structured target scans report root failure without pretending there are no versions", "[target-scan]") {
    ScriptedSource source;
    const auto error = windows_error(ERROR_BAD_NETPATH);
    source.probes[fs::path(root).root_path()] = std::unexpected(error);
    const auto result = launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source);
    CHECK(result.status == Status::failed);
    CHECK(result.versions.empty());
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].operation == Operation::probe);
    CHECK(result.diagnostics[0].path == fs::path(root).root_path());
    CHECK(result.diagnostics[0].error == error);
    CHECK(source.enumerated.empty());
}

TEST_CASE("Structured target scans reject invalid patterns before filesystem operations", "[target-scan]") {
    ScriptedSource source;
    fs::path pattern;
    SECTION("An empty pattern has no discovery identity") {}
    SECTION("An embedded NUL must not silently truncate the requested pattern") {
        auto text = fixture_path(L"Version *\\tool.exe").wstring();
        text.insert(text.begin() + 4, L'\0');
        pattern = std::move(text);
    }
    const auto result = launcher::scan_target_versions_detailed(pattern, source);
    CHECK(result.status == Status::failed);
    CHECK(result.versions.empty());
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].operation == Operation::normalize);
    CHECK(result.diagnostics[0].path == pattern);
    CHECK(result.diagnostics[0].error == windows_error(ERROR_INVALID_NAME));
    CHECK(source.probed.empty());
    CHECK(source.enumerated.empty());
    CHECK(source.metadata_read.empty());
}

TEST_CASE("Structured target scans keep observations when a cursor fails after an entry", "[target-scan]") {
    ScriptedSource source;
    const auto wildcard = fixture_path(L"Version *");
    const auto error = windows_error(ERROR_UNEXP_NET_ERR);
    source.listings[wildcard] = {Entry{L"Version 5.10", Kind::directory}, error};
    add_version(source, L"Version 5.10");
    const auto result = launcher::scan_target_versions_detailed(wildcard / L"tool.exe", source);
    CHECK(result.status == Status::incomplete);
    REQUIRE(result.versions.size() == 1);
    CHECK(result.versions[0].executable == fixture_path(L"Version 5.10\\tool.exe"));
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].operation == Operation::enumerate);
    CHECK(result.diagnostics[0].path == wildcard);
    CHECK(result.diagnostics[0].error == error);
    CHECK(source.cursors->opened == 1);
    CHECK(source.cursors->closed == 1);
}

TEST_CASE("Structured target scans preserve good sibling branches when another branch fails", "[target-scan]") {
    auto source = successful_source();
    const auto denied = fixture_path(L"Version 5.9\\tool.exe");
    const auto error = windows_error(ERROR_ACCESS_DENIED);
    source.probes[denied] = std::unexpected(error);
    const auto result = launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source);
    CHECK(result.status == Status::incomplete);
    REQUIRE(result.versions.size() == 1);
    CHECK(result.versions[0].executable == fixture_path(L"Version 5.10\\tool.exe"));
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].operation == Operation::probe);
    CHECK(result.diagnostics[0].path == denied);
    CHECK(result.diagnostics[0].error == error);
    CHECK(source.cursors->opened == source.cursors->closed);
}

TEST_CASE("Structured target scans do not call a failed empty enumeration complete", "[target-scan]") {
    ScriptedSource source;
    const auto wildcard = fixture_path(L"Version *");
    const auto error = windows_error(ERROR_ACCESS_DENIED);
    SECTION("Opening the cursor fails") { source.open_errors[wildcard] = error; }
    SECTION("The first cursor step fails") { source.listings[wildcard] = {error}; }
    const auto result = launcher::scan_target_versions_detailed(wildcard / L"tool.exe", source);
    CHECK(result.status == Status::incomplete);
    CHECK(result.versions.empty());
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].operation == Operation::enumerate);
    CHECK(result.diagnostics[0].error == error);
    CHECK(source.cursors->opened == source.cursors->closed);
}

TEST_CASE("Structured target scan failures retain full Windows paths beyond 1024 units", "[target-scan]") {
    ScriptedSource source;
    auto directory = fs::path(root);
    // Individual components remain below Windows' usual 255-unit limit.
    for (unsigned index = 0; index < 6; ++index) { directory /= std::wstring(200, static_cast<wchar_t>(L'a' + index)); }
    const auto wildcard = directory / L"Version *";
    const auto executable = directory / L"Version 5.10" / L"tool.exe";
    REQUIRE(wildcard.native().size() > 1'024);
    REQUIRE(executable.native().size() > 1'024);
    const auto error = windows_error(ERROR_CRC);
    source.listings[wildcard] = {Entry{L"Version 5.10", Kind::directory}};
    source.probes[executable] = Kind::regular_file;
    auto expected_path = wildcard;
    auto operation = Operation::enumerate;
    SECTION("opening a directory listing") { source.open_errors[wildcard] = error; }
    SECTION("advancing a directory listing") { source.listings[wildcard] = {error}; }
    SECTION("probing a concrete file") {
        source.probes[executable] = std::unexpected(error);
        expected_path = executable;
        operation = Operation::probe;
    }
    SECTION("reading concrete file metadata") {
        source.metadata[executable] = std::unexpected(error);
        expected_path = executable;
        operation = Operation::metadata;
    }
    const auto result = launcher::scan_target_versions_detailed(wildcard / L"tool.exe", source);
    CHECK(result.status == Status::incomplete);
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().path == expected_path);
    CHECK(result.diagnostics.front().operation == operation);
    CHECK(result.diagnostics.front().error == error);
    CHECK(result.omitted_diagnostics == 0);
    CHECK(source.cursors->opened == source.cursors->closed);
}

TEST_CASE("Structured target scans distinguish raw Win32 provider failures from missing paths", "[target-scan]") {
    ScriptedSource source;
    DWORD code = ERROR_BAD_NETPATH;
    SECTION("A bad network path is not an ordinary absent filename") {}
    SECTION("A bad network name is not an ordinary absent filename") { code = ERROR_BAD_NET_NAME; }
    SECTION("An invalid provider filename is not an ordinary absent filename") { code = ERROR_INVALID_NAME; }
    const auto wildcard = fixture_path(L"Version *");
    const auto error = windows_error(code);
    source.open_errors[wildcard] = error;
    const auto result = launcher::scan_target_versions_detailed(wildcard / L"tool.exe", source);
    CHECK(result.status == Status::incomplete);
    CHECK(result.versions.empty());
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].operation == Operation::enumerate);
    CHECK(result.diagnostics[0].path == wildcard);
    CHECK(result.diagnostics[0].error == error);
}

TEST_CASE("Structured target scans do not hide an unavailable parent behind file not found", "[target-scan]") {
    ScriptedSource source;
    const auto wildcard = fixture_path(L"Version *");
    source.open_errors[wildcard] = windows_error(ERROR_PATH_NOT_FOUND);
    const auto error = windows_error(ERROR_BAD_NETPATH);
    source.probe_steps[fs::path(root)] = {Kind::directory, std::unexpected(error)};
    const auto result = launcher::scan_target_versions_detailed(wildcard / L"tool.exe", source);
    CHECK(result.status == Status::incomplete);
    CHECK(result.versions.empty());
    REQUIRE_FALSE(result.diagnostics.empty());
    CHECK(result.diagnostics.back().error == error);
    REQUIRE(source.enumerated.size() == 1);
    CHECK(source.enumerated[0] == wildcard);
}

TEST_CASE("Structured target scans bound repeated diagnostics without declaring success", "[target-scan]") {
    ScriptedSource source;
    auto& entries = source.listings[fixture_path(L"Version *")];
    for (unsigned index = 0; index < 70; ++index) {
        const auto directory = L"Version " + std::to_wstring(index);
        entries.push_back(Entry{directory, Kind::directory});
        source.probes[fixture_path(directory + L"\\tool.exe")] = std::unexpected(windows_error(ERROR_ACCESS_DENIED));
    }
    const auto result = launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source);
    CHECK(result.status == Status::incomplete);
    CHECK(result.versions.empty());
    CHECK(result.diagnostics.size() == 64);
    CHECK(result.omitted_diagnostics == 6);
    CHECK(source.cursors->opened == source.cursors->closed);
}

TEST_CASE("Structured target scans distinguish missing metadata from metadata read failure", "[target-scan]") {
    auto source = successful_source();
    const auto unavailable = fixture_path(L"Version 5.10\\tool.exe");
    const auto error = windows_error(ERROR_SHARING_VIOLATION);
    source.metadata[unavailable] = std::unexpected(error);
    const auto result = launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source);
    CHECK(result.status == Status::incomplete);
    REQUIRE(result.versions.size() == 2);
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].operation == Operation::metadata);
    CHECK(result.diagnostics[0].path == unavailable);
    CHECK(result.diagnostics[0].error == error);
    CHECK(source.cursors->opened == source.cursors->closed);
}

TEST_CASE("Target scan limits reject oversized paths and component depth before probing", "[target-scan][target-scan-budget]") {
    ScriptedSource source;
    launcher::TargetScanLimits limits;
    auto limit = launcher::TargetScanLimit::components;
    SECTION("Every normalized supplied component contributes to the depth bound") { limits.max_components = 1; }
    SECTION("Actual target paths are rejected rather than shortened") {
        limits.max_path_units = 8;
        limit = launcher::TargetScanLimit::path_units;
    }
    const auto result = launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source, limits);
    check_limit(result, limit);
    CHECK(result.versions.empty());
    CHECK(source.probed.empty());
    CHECK(source.enumerated.empty());
}

TEST_CASE("Target scan work limits close an owned cursor on exhaustion", "[target-scan][target-scan-budget]") {
    auto source = successful_source();
    launcher::TargetScanLimits limits;
    auto limit = launcher::TargetScanLimit::operations;
    SECTION("Provider operations count the root prefix enumeration and cursor calls") { limits.max_operations = 4; }
    SECTION("Visited entries include entries that do not ultimately become files") {
        limits.max_entries = 1;
        limit = launcher::TargetScanLimit::entries;
    }
    SECTION("Accepted literal and wildcard branches share one finite allowance") {
        limits.max_branches = 2;
        limit = launcher::TargetScanLimit::branches;
    }
    const auto result = launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source, limits);
    check_limit(result, limit);
    CHECK(result.versions.empty());
    CHECK(source.metadata_read.empty());
    CHECK(source.cursors->opened == 1);
    CHECK(source.cursors->closed == 1);
    if (limit == launcher::TargetScanLimit::operations) {
        CHECK(source.probed.size() + source.enumerated.size() + source.metadata_read.size() + source.cursors->next == limits.max_operations);
    }
}

TEST_CASE("Target scans stop admitting provider work after an adapter reports a named limit", "[target-scan][target-scan-budget]") {
    auto source = successful_source();
    const auto error = launcher::make_target_scan_limit_error(launcher::TargetScanLimit::storage_bytes);
    SECTION("A cursor limit closes the cursor and leaves observed branches untrusted") {
        source.listings[fixture_path(L"Version *")] = {Entry{L"Version 5.9", Kind::directory}, error,
            Entry{L"Version 5.10", Kind::directory}};
        const auto result = launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source);
        check_limit(result, launcher::TargetScanLimit::storage_bytes);
        CHECK(result.versions.empty());
        CHECK(source.metadata_read.empty());
        CHECK(source.cursors->next == 2);
        CHECK(source.cursors->closed == 1);
    }
    SECTION("A root limit remains incomplete rather than a provider failure") {
        source.probes[fs::path(root).root_path()] = std::unexpected(error);
        const auto result = launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source);
        check_limit(result, launcher::TargetScanLimit::storage_bytes);
        CHECK(source.enumerated.empty());
        CHECK(source.metadata_read.empty());
    }
}

TEST_CASE("Target scan target limits count distinct final identities without dropping duplicates as success", "[target-scan][target-scan-budget]") {
    auto source = successful_source();
    launcher::TargetScanLimits limits;
    limits.max_targets = 1;
    SECTION("A second distinct final file makes the observation incomplete") {
        const auto result = launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source, limits);
        check_limit(result, launcher::TargetScanLimit::targets);
        CHECK(source.metadata_read.empty());
        CHECK(result.versions.empty());
    }
    SECTION("Duplicate native matches do not spend the distinct target allowance twice") {
        source.listings[fixture_path(L"Version *")] = {Entry{L"Version 5.9", Kind::directory}, Entry{L"Version 5.9", Kind::directory}};
        const auto result = launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source, limits);
        CHECK(result.status == Status::complete);
        CHECK(result.exhausted_limit == launcher::TargetScanLimit::none);
        REQUIRE(result.versions.size() == 1);
        CHECK(result.versions[0].executable == fixture_path(L"Version 5.9\\tool.exe"));
    }
    CHECK(source.cursors->opened == source.cursors->closed);
}

TEST_CASE("Target scan storage limits include frontier capacity retained labels and result text", "[target-scan][target-scan-budget]") {
    ScriptedSource source;
    launcher::TargetScanLimits limits;
    SECTION("No storage allowance performs no filesystem operations") {
        limits.max_storage_bytes = 0;
        const auto result = launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source, limits);
        check_limit(result, launcher::TargetScanLimit::storage_bytes);
        CHECK(source.probed.empty());
        CHECK(result.diagnostics.empty());
        CHECK(result.omitted_diagnostics == 1);
    }
    SECTION("A growing frontier cannot escape the memory allowance") {
        limits.max_storage_bytes = 4'096;
        for (unsigned index = 0; index < 100; ++index) {
            source.listings[fixture_path(L"Version *")].push_back(Entry{L"Version with retained descriptive text " + std::to_wstring(index), Kind::directory});
        }
        const auto result = launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source, limits);
        check_limit(result, launcher::TargetScanLimit::storage_bytes);
        CHECK(source.cursors->opened == 1);
        CHECK(source.cursors->closed == 1);
        CHECK(source.metadata_read.empty());
    }
    SECTION("Individually valid metadata also consumes the aggregate result allowance") {
        source = successful_source();
        launcher::TargetVersion metadata;
        metadata.has_version_resource = true;
        metadata.product_name.assign(4'000, L'N');
        metadata.product_version.assign(4'000, L'V');
        metadata.file_version.assign(4'000, L'F');
        source.metadata[fixture_path(L"Version 5.9\\tool.exe")] = metadata;
        source.metadata[fixture_path(L"Version 5.10\\tool.exe")] = metadata;
        limits.max_storage_bytes = 64 * 1024;
        const auto result = launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source, limits);
        check_limit(result, launcher::TargetScanLimit::storage_bytes);
        REQUIRE(result.versions.size() == 1);
        CHECK(source.metadata_read.size() == 2);
        CHECK(source.cursors->opened == source.cursors->closed);
    }
}

TEST_CASE("Target scan metadata budgets reject oversized fields without a fallback success", "[target-scan][target-scan-budget]") {
    auto source = successful_source();
    launcher::TargetScanLimits limits;
    limits.max_metadata_units = 4;
    launcher::TargetVersion metadata;
    metadata.product_name = L"A long product name";
    source.metadata[fixture_path(L"Version 5.9\\tool.exe")] = metadata;
    const auto result = launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source, limits);
    check_limit(result, launcher::TargetScanLimit::metadata_units);
    CHECK(result.versions.empty());
    REQUIRE(source.metadata_read.size() == 1);
    CHECK(source.cursors->opened == source.cursors->closed);
}

TEST_CASE("Target scan diagnostic caps preserve failure and sampled paths do not alter targets", "[target-scan][target-scan-budget]") {
    auto source = successful_source();
    launcher::TargetScanLimits limits;
    SECTION("A zero record cap still exposes the exhausted budget") {
        limits.max_diagnostics = 0;
        limits.max_entries = 0;
        const auto result = launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source, limits);
        check_limit(result, launcher::TargetScanLimit::entries);
        CHECK(result.diagnostics.empty());
        CHECK(result.omitted_diagnostics == 1);
    }
    SECTION("Only diagnostic path samples are shortened") {
        limits.max_diagnostic_path_units = 8;
        source.probes[fixture_path(L"Version 5.9\\tool.exe")] = std::unexpected(windows_error(ERROR_ACCESS_DENIED));
        const auto result = launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source, limits);
        CHECK(result.status == Status::incomplete);
        REQUIRE(result.diagnostics.size() == 1);
        CHECK(result.diagnostics[0].path.native().size() == 8);
        REQUIRE(result.versions.size() == 1);
        CHECK(result.versions[0].executable == fixture_path(L"Version 5.10\\tool.exe"));
        CHECK(result.versions[0].target_pattern == fixture_path(L"Version *\\tool.exe").native());
    }
    CHECK(source.cursors->opened == source.cursors->closed);
}

TEST_CASE("Native target metadata checks resource storage text and translation budgets", "[target-scan][target-scan-budget][native]") {
    launcher::TargetScanLimits limits;
    auto limit = launcher::TargetScanLimit::resource_bytes;
    SECTION("The native resource buffer is bounded before allocation") { limits.max_resource_bytes = 0; }
    SECTION("The resource buffer and produced fields share the storage allowance") {
        limits.max_storage_bytes = 1;
        limit = launcher::TargetScanLimit::storage_bytes;
    }
    SECTION("Native field lengths are bounded before allocating text") {
        limits.max_metadata_units = 0;
        limit = launcher::TargetScanLimit::metadata_units;
    }
    SECTION("Malformed repeated translation records cannot cause unbounded metadata work") {
        limits.max_translations = 0;
        limit = launcher::TargetScanLimit::translations;
    }
    const auto source = launcher::make_native_target_version_source(limits);
    const auto metadata = source->read_metadata(versioned_probe());
    REQUIRE_FALSE(metadata.has_value());
    CHECK(launcher::target_scan_limit(metadata.error()) == limit);
}

TEST_CASE("Native metadata resource exhaustion keeps the structured scan incomplete", "[target-scan][target-scan-budget][native]") {
    const NativeFixture fixture;
    const auto path = fixture.path(L"Version 5.10\\tool.exe");
    fs::create_directories(path.parent_path());
    REQUIRE(fs::copy_file(versioned_probe(), path));
    launcher::TargetScanLimits limits;
    limits.max_resource_bytes = 0;
    const auto result = launcher::scan_target_versions_detailed(fixture.path(L"Version *\\tool.exe"), limits);
    check_limit(result, launcher::TargetScanLimit::resource_bytes);
    CHECK(result.versions.empty());
}

TEST_CASE("Legacy target scanning rejects default budget exhaustion", "[target-scan][target-scan-budget]") {
    ScriptedSource source;
    const fs::path oversized(L"C:\\" + std::wstring(32'768, L'x'));
    try {
        static_cast<void>(launcher::scan_target_versions(oversized, source));
        FAIL("A resource-limited observation must not replace a complete cache");
    } catch (const fs::filesystem_error& failure) {
        CHECK(launcher::target_scan_limit(failure.code()) == launcher::TargetScanLimit::path_units);
    }
    CHECK(source.probed.empty());
}

TEST_CASE("Structured target scans recheck a file that disappears during metadata reading", "[target-scan]") {
    auto source = successful_source();
    const auto newest = fixture_path(L"Version 5.10\\tool.exe");
    source.metadata[newest] = std::unexpected(windows_error(ERROR_FILE_NOT_FOUND));
    SECTION("The file has disappeared from an otherwise reachable directory") {
        source.probe_steps[newest] = {Kind::regular_file, std::unexpected(windows_error(ERROR_FILE_NOT_FOUND))};
        const auto result = launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source);
        CHECK(result.status == Status::complete);
        REQUIRE(result.versions.size() == 1);
        CHECK(result.versions[0].executable == fixture_path(L"Version 5.9\\tool.exe"));
        CHECK(result.diagnostics.empty());
    }
    SECTION("The file still exists so the metadata failure is not a missing resource") {
        const auto result = launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source);
        CHECK(result.status == Status::incomplete);
        REQUIRE(result.versions.size() == 2);
        REQUIRE(result.diagnostics.size() == 1);
        CHECK(result.diagnostics[0].operation == Operation::metadata);
        CHECK(result.diagnostics[0].path == newest);
        CHECK(result.diagnostics[0].error == windows_error(ERROR_FILE_NOT_FOUND));
    }
    CHECK(source.cursors->opened == source.cursors->closed);
}

TEST_CASE("Structured target scans use supplied numeric metadata without replacing path identity", "[target-scan]") {
    auto source = successful_source();
    launcher::TargetVersion metadata;
    metadata.executable = L"C:\\Wrong path must not survive.exe";
    metadata.has_version_resource = true;
    metadata.version = {9, 0, 0, 0};
    metadata.product_version = L"9.0";
    metadata.product_name = L"Actual product";
    const auto upgraded = fixture_path(L"Version 5.9\\tool.exe");
    source.metadata[upgraded] = metadata;
    const auto result = launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source);
    REQUIRE(result.status == Status::complete);
    REQUIRE(result.versions.size() == 2);
    CHECK(result.versions[0].executable == upgraded);
    CHECK(result.versions[0].product_name == L"Actual product");
    CHECK(result.versions[0].product_version == L"9.0");
    CHECK(result.versions[0].path_label == L"Version 5.9");
    CHECK(result.versions[0].has_version_resource);
}

TEST_CASE("Structured target scans verify final entries and skip nonfiles", "[target-scan]") {
    ScriptedSource source;
    const auto pattern = fixture_path(L"tool*");
    source.listings[pattern] = {Entry{L"tool directory", Kind::directory},
        Entry{L"tool vanished.exe", Kind::regular_file}, Entry{L"tool ordinary.exe", Kind::regular_file}};
    source.probes[fixture_path(L"tool directory")] = Kind::directory;
    source.probes[fixture_path(L"tool vanished.exe")] = std::unexpected(windows_error(ERROR_FILE_NOT_FOUND));
    source.probes[fixture_path(L"tool ordinary.exe")] = Kind::regular_file;
    const auto result = launcher::scan_target_versions_detailed(pattern, source);
    REQUIRE(result.status == Status::complete);
    REQUIRE(result.versions.size() == 1);
    CHECK(result.versions[0].executable == fixture_path(L"tool ordinary.exe"));
    REQUIRE(source.metadata_read.size() == 1);
    CHECK(source.metadata_read[0] == result.versions[0].executable);
    CHECK(source.cursors->opened == source.cursors->closed);
}

TEST_CASE("Legacy target scans reject partial observations with their original error", "[target-scan]") {
    auto source = successful_source();
    const auto denied = fixture_path(L"Version 5.9\\tool.exe");
    const auto error = windows_error(ERROR_ACCESS_DENIED);
    source.probes[denied] = std::unexpected(error);
    try {
        static_cast<void>(launcher::scan_target_versions(fixture_path(L"Version *\\tool.exe"), source));
        FAIL("A partial version catalog must not replace a complete cache");
    } catch (const fs::filesystem_error& failure) {
        CHECK(failure.code() == error);
        CHECK(failure.path1() == denied);
    }
    CHECK(source.cursors->opened == source.cursors->closed);
}

TEST_CASE("Target scan adapter exceptions propagate and close every owned cursor", "[target-scan]") {
    auto source = successful_source();
    source.before_metadata = [](const fs::path&) { throw std::runtime_error("Injected metadata allocation failure"); };
    CHECK_THROWS_WITH(launcher::scan_target_versions_detailed(fixture_path(L"Version *\\tool.exe"), source),
        "Injected metadata allocation failure");
    CHECK(source.cursors->opened == 1);
    CHECK(source.cursors->closed == 1);
}

TEST_CASE("Blocked target scanning keeps a usable cache and rejects a later partial refresh", "[target-scan][adobe-cache]") {
    launcher::TargetVersionCache cache;
    const std::vector previous{known_version()};
    const auto seeded = refresh_and_collect(cache, [previous] { return previous; });
    REQUIRE(seeded.changed);
    REQUIRE(cache.snapshot() == previous);

    const auto source = std::make_shared<ScriptedSource>(successful_source());
    const auto gate = std::make_shared<Gate>();
    const ReleaseGuard release{gate};
    auto entered = gate->entered.get_future();
    source->before_metadata = [gate](const fs::path&) {
        gate->entered.set_value();
        gate->allowed.wait();
    };
    // Only one candidate reaches metadata, keeping the gate a single operation.
    source->probes[fixture_path(L"Version 5.9\\tool.exe")] = std::unexpected(windows_error(ERROR_ACCESS_DENIED));
    const auto completion = std::make_shared<std::promise<void>>();
    auto finished = completion->get_future();
    auto token = completion_token(completion);
    REQUIRE(cache.refresh([source, token] {
        return launcher::scan_target_versions(fixture_path(L"Version *\\tool.exe"), *source);
    }));
    token.reset();
    REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    CHECK(cache.refreshing());
    CHECK(cache.snapshot() == previous);
    CHECK_FALSE(cache.poll().has_value());
    gate->open();
    REQUIRE(finished.wait_for(2s) == std::future_status::ready);
    const auto result = cache.poll();
    REQUIRE(result.has_value());
    CHECK_FALSE(result->changed);
    CHECK_FALSE(result->error.empty());
    CHECK(cache.initialized());
    CHECK_FALSE(cache.refreshing());
    CHECK(cache.snapshot() == previous);
    CHECK(source->cursors->opened == source->cursors->closed);
}

TEST_CASE("Native target scan metadata treats ordinary and malformed files as optional absence", "[target-scan][native]") {
    const NativeFixture fixture;
    const auto ordinary = fixture.file(L"Version 5.9\\tool.exe", "An ordinary document without version resources.");
    const auto malformed = fixture.file(L"Version 5.10\\tool.exe", "MZ an intentionally truncated executable header.");
    const auto source = launcher::make_native_target_version_source();
    REQUIRE(source);
    for (const auto& path : {ordinary, malformed}) {
        const auto probe = source->probe(path);
        REQUIRE(probe.has_value());
        CHECK(*probe == Kind::regular_file);
        const auto metadata = source->read_metadata(path);
        REQUIRE(metadata.has_value());
        CHECK_FALSE(metadata->has_value());
    }
    const auto result = launcher::scan_target_versions_detailed(fixture.path(L"Version *\\tool.exe"));
    REQUIRE(result.status == Status::complete);
    CHECK(result.diagnostics.empty());
    REQUIRE(result.versions.size() == 2);
    CHECK(result.versions[0].executable == malformed);
    CHECK(result.versions[1].executable == ordinary);
}

TEST_CASE("Native target scan metadata reports a missing file as unreadable", "[target-scan][native]") {
    const NativeFixture fixture;
    const auto source = launcher::make_native_target_version_source();
    const auto metadata = source->read_metadata(fixture.path(L"Missing.exe"));
    REQUIRE_FALSE(metadata.has_value());
    CHECK(metadata.error() == windows_error(ERROR_FILE_NOT_FOUND));
}

TEST_CASE("Native target scan metadata retains numeric and textual version resources", "[target-scan][native]") {
    const auto source = launcher::make_native_target_version_source();
    const auto probe = versioned_probe();
    REQUIRE(fs::is_regular_file(probe));
    const auto metadata = source->read_metadata(probe);
    REQUIRE(metadata.has_value());
    REQUIRE(metadata->has_value());
    const auto& version = **metadata;
    CHECK(version.has_version_resource);
    CHECK(version.executable == probe);
    CHECK_FALSE(version.product_name.empty());
    CHECK_FALSE(version.product_version.empty());
    CHECK_FALSE(version.file_version.empty());
    CHECK(version.version[0] != 0);
    CHECK(launcher::read_target_version(probe) == *metadata);
}

TEST_CASE("Native target scan metadata distinguishes a sharing failure from absent resources", "[target-scan][native]") {
    const NativeFixture fixture;
    const auto copy = fixture.path(L"Locked probe.exe");
    REQUIRE(fs::copy_file(versioned_probe(), copy));
    struct Handle final {
        HANDLE value;
        ~Handle() { if (value != INVALID_HANDLE_VALUE) { CloseHandle(value); } }
    } lock{CreateFileW(copy.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    REQUIRE(lock.value != INVALID_HANDLE_VALUE);
    const auto source = launcher::make_native_target_version_source();
    const auto metadata = source->read_metadata(copy);
    REQUIRE_FALSE(metadata.has_value());
    CHECK(metadata.error() == windows_error(ERROR_SHARING_VIOLATION));
}

TEST_CASE("Native target scans follow configured links while keeping only final files", "[target-scan][native]") {
    const NativeFixture fixture;
    const auto source = launcher::make_native_target_version_source();
    SECTION("A file link remains a usable target at the configured link path") {
        const auto target = fixture.file(L"Actual\\tool.exe", "A linked ordinary file without version resources.");
        const auto link = fixture.path(L"Version 5.10.exe");
        create_test_link(link, target, false);
        const auto kind = source->probe(link);
        REQUIRE(kind.has_value());
        CHECK(*kind == Kind::regular_file);
        const auto result = launcher::scan_target_versions_detailed(fixture.path(L"Version *.exe"));
        REQUIRE(result.status == Status::complete);
        REQUIRE(result.versions.size() == 1);
        CHECK(result.versions[0].executable == link);
    }
    SECTION("A directory link may be traversed but cannot itself become a final file") {
        const auto target = fixture.file(L"Actual\\tool.exe", "A file beneath a linked directory.");
        const auto link = fixture.path(L"Version 5.10");
        create_test_link(link, target.parent_path(), true);
        const auto kind = source->probe(link);
        REQUIRE(kind.has_value());
        CHECK(*kind == Kind::directory);
        const auto nested = launcher::scan_target_versions_detailed(fixture.path(L"Version *\\tool.exe"));
        REQUIRE(nested.status == Status::complete);
        REQUIRE(nested.versions.size() == 1);
        CHECK(nested.versions[0].executable == link / L"tool.exe");
        const auto terminal = launcher::scan_target_versions_detailed(fixture.path(L"Version *"));
        CHECK(terminal.status == Status::complete);
        CHECK(terminal.versions.empty());
    }
    SECTION("A dangling file link is an absent final target") {
        create_test_link(fixture.path(L"Version 5.10.exe"), fixture.path(L"Missing.exe"), false);
        const auto result = launcher::scan_target_versions_detailed(fixture.path(L"Version *.exe"));
        CHECK(result.status == Status::complete);
        CHECK(result.versions.empty());
        CHECK(result.diagnostics.empty());
    }
}
