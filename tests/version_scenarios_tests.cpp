#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/version_scenarios.hpp"
#include "launcher/config_text.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <optional>
#include <thread>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

std::string path_text(const fs::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

struct Fixture final {
    inline static std::atomic<unsigned> next{0};
    std::string id = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++next);
    fs::path root = fs::temp_directory_path() / (L"Launcher-version-scenarios-" + std::wstring(id.begin(), id.end()));
    fs::path scenario = root / L"scenario.toml";
    fs::path config = root / L"fixture.toml";
    fs::path pattern = root / L"Version *" / L"Guide.txt";
    Fixture() {
        REQUIRE(fs::create_directory(root));
        std::ofstream(root / L"LauncherVersionScenario.fixture") << "Launcher version scenario fixture v1\n";
        std::ofstream(config) << "schema_version = 1\n";
        for (const auto directory : {L"Version 1", L"Version 2"}) {
            fs::create_directory(root / directory);
            std::ofstream(root / directory / L"Guide.txt") << "Owned scenario document.";
        }
    }
    ~Fixture() {
        std::error_code error;
        const auto actual = fs::weakly_canonical(root, error);
        if (error) { return; }
        const auto configured_temporary = fs::temp_directory_path(error);
        if (error) { return; }
        const auto temporary = fs::weakly_canonical(configured_temporary, error);
        if (error || actual.parent_path() != temporary || !actual.filename().native().starts_with(L"Launcher-version-scenarios-")) { return; }
        fs::remove_all(actual, error);
    }
    void write(toml::array attempts, const fs::path& search_fixture = {}, std::optional<bool> notifications = {}) const {
        toml::table search{{"id", "documents"}, {"provider", "target"},
            {"path", path_text(pattern)}, {"attempts", std::move(attempts)}};
        if (!search_fixture.empty()) { search.insert("fixture_directory", path_text(search_fixture)); }
        toml::table table{{"schema_version", 1}, {"instance", id}, {"configuration", path_text(config)},
            {"output_directory", path_text(root / L"evidence")}, {"dispatch", "capture-only"},
            {"searches", toml::array{std::move(search)}}};
        if (notifications) { table.insert("notifications", *notifications); }
        std::ofstream(scenario) << table;
    }
    launcher::VersionSearchKey key() const {
        auto identity = launcher::normalize_target_version_pattern(pattern).native();
        CharLowerBuffW(identity.data(), static_cast<DWORD>(identity.size()));
        return {launcher::VersionSearchKind::target_pattern, {std::move(identity)}, 1};
    }
};

class InlineExecutor final : public launcher::VersionDiscoveryExecutor {
public:
    void start(Work work) override { work(); }
};

std::wstring event_name(const fs::path& manifest_path, std::string_view field, std::size_t attempt_index = 0) {
    const auto manifest = launcher::config_text_builder(manifest_path).parse();
    const auto& search = *manifest["searches"].as_array()->get_as<toml::table>(0);
    const auto& attempt = *search["attempts"].as_array()->get_as<toml::table>(attempt_index);
    const auto encoded = *attempt[field].value<std::string>();
    const auto path = fs::path(std::u8string(encoded.begin(), encoded.end()));
    return path.native();
}

} // namespace

TEST_CASE("Version scenarios require an owned absolute isolated capture-only fixture", "[version-scenarios]") {
    const Fixture fixture;
    fixture.write(toml::array{toml::table{}});
    SECTION("implicit configuration is rejected") {
        REQUIRE_THROWS(launcher::VersionScenarios::load(fixture.scenario, {}));
    }
    SECTION("relative scenario paths are rejected") {
        REQUIRE_THROWS(launcher::VersionScenarios::load(L"scenario.toml", fixture.config));
    }
    SECTION("missing ownership marker is rejected") {
        REQUIRE(fs::remove(fixture.root / L"LauncherVersionScenario.fixture"));
        REQUIRE_THROWS(launcher::VersionScenarios::load(fixture.scenario, fixture.config));
    }
    SECTION("another configuration cannot borrow the scenario") {
        const auto other = fixture.root / L"other.toml";
        std::ofstream(other) << "schema_version = 1\n";
        REQUIRE_THROWS(launcher::VersionScenarios::load(fixture.scenario, other));
    }
}

TEST_CASE("Version scenarios enable native notifications only with an explicit boolean", "[version-scenarios]") {
    const Fixture fixture;
    bool enabled = false;
    SECTION("notifications are disabled by default") { fixture.write(toml::array{toml::table{}}); }
    SECTION("notifications can be enabled for an interactive scenario") {
        fixture.write(toml::array{toml::table{}}, {}, true);
        enabled = true;
    }
    SECTION("an explicit false remains disabled") { fixture.write(toml::array{toml::table{}}, {}, false); }
    SECTION("a string cannot enable notifications") {
        fixture.write(toml::array{toml::table{}});
        auto table = launcher::config_text_builder(fixture.scenario).parse();
        table.insert("notifications", "true");
        { std::ofstream output(fixture.scenario); output << table; }
        REQUIRE_THROWS(launcher::VersionScenarios::load(fixture.scenario, fixture.config));
        return;
    }
    const auto scenario = launcher::VersionScenarios::load(fixture.scenario, fixture.config);
    CHECK(scenario->notifications_enabled() == enabled);
    const auto manifest = launcher::config_text_builder(scenario->output_directory() / L"manifest.toml").parse();
    CHECK(manifest["notifications"].value<bool>() == enabled);
}

TEST_CASE("Version scenarios reject unowned or escaping explicit search fixtures", "[version-scenarios]") {
    Fixture fixture;
    const Fixture external;
    fixture.pattern = external.pattern;
    auto search_fixture = external.root;
    SECTION("the selected root needs its own marker") { REQUIRE(fs::remove(external.root / L"LauncherVersionScenario.fixture")); }
    SECTION("a drive root is not a fixture") { search_fixture = external.root.root_path(); }
    SECTION("a share root is not a fixture") { search_fixture = L"\\\\fixture.invalid\\share"; }
    SECTION("a relative root is not a fixture") { search_fixture = L"relative-fixture"; }
    SECTION("a nonexistent root is not a fixture") { search_fixture /= L"missing"; }
    SECTION("root traversal cannot borrow a marker") { search_fixture = external.root / L"Version 1" / L".."; }
    SECTION("search traversal is rejected even when it returns inside") {
        fixture.pattern = external.root / L"Version 1" / L".." / L"Version *" / L"Guide.txt";
    }
    SECTION("a search cannot leave its selected root") { fixture.pattern = fixture.root / L"Version *" / L"Guide.txt"; }
    SECTION("a root directory link is rejected") {
        search_fixture = fixture.root / L"linked-external";
        if (!CreateSymbolicLinkW(search_fixture.c_str(), external.root.c_str(),
                SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            SKIP("Creating a directory link requires Windows Developer Mode or the symlink privilege.");
        }
    }
    fixture.write(toml::array{toml::table{}}, search_fixture);
    REQUIRE_THROWS(launcher::VersionScenarios::load(fixture.scenario, fixture.config));
}

TEST_CASE("Version scenarios keep evidence internal while external native metadata fails after a gate", "[version-scenarios]") {
    Fixture fixture;
    const Fixture external;
    fixture.pattern = external.pattern;
    fixture.write(toml::array{toml::table{}, toml::table{{"gate", true}, {"gate_stage", "metadata"}}}, external.root);
    const auto scenario = launcher::VersionScenarios::load(fixture.scenario, fixture.config);
    CHECK(scenario->configuration_path() == fixture.config);
    CHECK(scenario->output_directory().parent_path() == fixture.root / L"evidence");
    const auto first = scenario->scanner()(fixture.key());
    REQUIRE(first.status == launcher::TargetScanStatus::complete);
    REQUIRE(first.versions.size() == 2);
    for (const auto& version : first.versions) { CHECK(version.executable.parent_path().parent_path() == external.root); }
    const auto manifest = scenario->output_directory() / L"manifest.toml";
    struct Handle final {
        HANDLE value;
        ~Handle() { if (value && value != INVALID_HANDLE_VALUE) { CloseHandle(value); } }
    } entered{OpenEventW(SYNCHRONIZE, FALSE, event_name(manifest, "entered", 1).c_str())},
        release{OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name(manifest, "release", 1).c_str())};
    REQUIRE(entered.value);
    REQUIRE(release.value);
    std::promise<launcher::TargetVersionScanResult> promise;
    auto finished = promise.get_future();
    std::jthread worker([scanner = scenario->scanner(), key = fixture.key(), &promise] {
        try { promise.set_value(scanner(key)); }
        catch (...) { promise.set_exception(std::current_exception()); }
    });
    struct ReleaseOnExit final { HANDLE value; ~ReleaseOnExit() { SetEvent(value); } } release_on_exit{release.value};
    REQUIRE(WaitForSingleObject(entered.value, 2'000) == WAIT_OBJECT_0);
    CHECK(finished.wait_for(0ms) == std::future_status::timeout);
    // The metadata gate precedes native file opens. Real exclusive handles can
    // therefore make the resumed native adapter return sharing violations.
    const Handle locked_first{CreateFileW((external.root / L"Version 1" / L"Guide.txt").c_str(), GENERIC_READ,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    const Handle locked_second{CreateFileW((external.root / L"Version 2" / L"Guide.txt").c_str(), GENERIC_READ,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    REQUIRE(locked_first.value != INVALID_HANDLE_VALUE);
    REQUIRE(locked_second.value != INVALID_HANDLE_VALUE);
    REQUIRE(SetEvent(release.value));
    REQUIRE(finished.wait_for(2s) == std::future_status::ready);
    const auto result = finished.get();
    worker.join();
    CHECK(result.status == launcher::TargetScanStatus::incomplete);
    REQUIRE(result.diagnostics.size() == 2);
    for (const auto& diagnostic : result.diagnostics) {
        CHECK(diagnostic.operation == launcher::TargetScanOperation::metadata);
        CHECK(diagnostic.error == std::error_code(ERROR_SHARING_VIOLATION, std::system_category()));
        CHECK(diagnostic.path.parent_path().parent_path() == external.root);
    }
    launcher::PreparedLaunch prepared;
    prepared.action = launcher::NativeLaunchAction{first.versions.front().executable.native(), {}, {}, launcher::LaunchShow::normal};
    CHECK_NOTHROW(scenario->capture_action(prepared, "open", false));
    prepared.action->target = (fixture.root / L"Version 1" / L"Guide.txt").native();
    CHECK_THROWS(scenario->capture_action(prepared, "open", false));
}

TEST_CASE("Version scenario plans exercise native partial failed exception and retained catalogs", "[version-scenarios]") {
    const Fixture fixture;
    fixture.write(toml::array{toml::table{}, toml::table{{"fail_after_entries", 1}, {"error", 53}},
        toml::table{{"outcome", "failed"}}, toml::table{{"outcome", "throw"}}, toml::table{}});
    const auto scenario = launcher::VersionScenarios::load(fixture.scenario, fixture.config);
    InlineExecutor executor;
    launcher::VersionDiscoveryService service(scenario->scanner(), executor);
    const std::array keys{fixture.key()};
    service.set_searches(keys);
    service.begin_activation(keys);
    REQUIRE(service.collect_ready() == 1);
    const auto original = service.state(keys[0])->snapshot;
    REQUIRE(original);
    REQUIRE(original->size() == 2);
    service.begin_activation(keys);
    REQUIRE(service.collect_ready() == 1);
    auto state = *service.state(keys[0]);
    CHECK(state.last_attempt->observation.status == launcher::TargetScanStatus::incomplete);
    CHECK(state.last_attempt->observation.versions.size() == 1);
    CHECK(state.snapshot == original);
    service.begin_activation(keys);
    REQUIRE(service.collect_ready() == 1);
    state = *service.state(keys[0]);
    CHECK(state.last_attempt->observation.status == launcher::TargetScanStatus::failed);
    CHECK(state.snapshot == original);
    service.begin_activation(keys);
    REQUIRE(service.collect_ready() == 1);
    state = *service.state(keys[0]);
    CHECK(state.last_attempt->failure == launcher::VersionDiscoveryFailure::scanner);
    CHECK(state.snapshot == original);
    REQUIRE(fs::remove(fixture.root / L"Version 1" / L"Guide.txt"));
    REQUIRE(fs::remove(fixture.root / L"Version 2" / L"Guide.txt"));
    service.begin_activation(keys);
    REQUIRE(service.collect_ready() == 1);
    state = *service.state(keys[0]);
    REQUIRE(state.snapshot);
    CHECK(state.snapshot->empty());
    CHECK(state.snapshot != original);
}

TEST_CASE("Version scenario fixture-root probes use Windows case-insensitive identity", "[version-scenarios]") {
    Fixture fixture;
    SECTION("the literal fixture root has alternate case") {}
    SECTION("a native wildcard-parent step returns to that root") {
        fixture.pattern = fixture.root / L"Version *" / L".." / L"Version *" / L"Guide.txt";
    }
    auto alternate = fixture.pattern.native();
    CharUpperBuffW(alternate.data(), static_cast<DWORD>(alternate.size()));
    fixture.pattern = std::move(alternate);
    fixture.write(toml::array{toml::table{}});
    const auto scenario = launcher::VersionScenarios::load(fixture.scenario, fixture.config);
    const auto result = scenario->scanner()(fixture.key());
    REQUIRE(result.status == launcher::TargetScanStatus::complete);
    REQUIRE(result.versions.size() == 2);
    CHECK(result.diagnostics.empty());
}

TEST_CASE("Version scenario gate owns its controller until native work is released", "[version-scenarios]") {
    const Fixture fixture;
    std::string stage;
    SECTION("before discovery") { stage = "before"; }
    SECTION("inside an owned native directory cursor") { stage = "cursor"; }
    SECTION("inside the metadata adapter") { stage = "metadata"; }
    fixture.write(toml::array{toml::table{{"gate", true}, {"gate_stage", stage}}});
    auto scenario = launcher::VersionScenarios::load(fixture.scenario, fixture.config);
    const auto manifest = scenario->output_directory() / L"manifest.toml";
    struct Event final {
        HANDLE value;
        ~Event() { if (value) { CloseHandle(value); } }
    } entered{OpenEventW(SYNCHRONIZE, FALSE, event_name(manifest, "entered").c_str())},
        release{OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name(manifest, "release").c_str())};
    REQUIRE(entered.value);
    REQUIRE(release.value);
    const std::weak_ptr<launcher::VersionScenarios> lifetime = scenario;
    std::promise<launcher::TargetVersionScanResult> promise;
    auto finished = promise.get_future();
    std::jthread worker([scanner = scenario->scanner(), key = fixture.key(), &promise]() mutable {
        try { promise.set_value(scanner(key)); }
        catch (...) { promise.set_exception(std::current_exception()); }
        scanner = {};
    });
    struct ReleaseOnExit final { HANDLE value; ~ReleaseOnExit() { SetEvent(value); } } release_on_exit{release.value};
    scenario.reset();
    REQUIRE(WaitForSingleObject(entered.value, 2'000) == WAIT_OBJECT_0);
    CHECK_FALSE(lifetime.expired());
    CHECK(finished.wait_for(0ms) == std::future_status::timeout);
    REQUIRE(SetEvent(release.value));
    REQUIRE(finished.wait_for(2s) == std::future_status::ready);
    CHECK(finished.get().versions.size() == 2);
    worker.join();
    CHECK(lifetime.expired());
}

TEST_CASE("Version scenario dispatch receipts preserve options and controls reject another PID", "[version-scenarios]") {
    const Fixture fixture;
    fixture.write(toml::array{toml::table{}});
    const auto scenario = launcher::VersionScenarios::load(fixture.scenario, fixture.config);
    const auto manifest = launcher::config_text_builder(scenario->output_directory() / L"manifest.toml").parse();
    const auto message = static_cast<std::uint32_t>(*manifest["control_message"].value<std::int64_t>());
    CHECK_FALSE(scenario->command(message, 1, static_cast<std::intptr_t>(GetCurrentProcessId()) + 1));
    CHECK_FALSE(scenario->command(message, 99, GetCurrentProcessId()));
    const auto command = scenario->command(message, (std::uintptr_t{3} << 16) | 4, GetCurrentProcessId());
    REQUIRE(command);
    CHECK(command->kind == launcher::VersionScenarioCommandKind::select_page);
    CHECK(command->page == 3);
    CHECK(scenario->command(message, 7, GetCurrentProcessId())->kind == launcher::VersionScenarioCommandKind::open_icon_menu);
    launcher::PreparedLaunch prepared;
    prepared.action = launcher::NativeLaunchAction{(fixture.root / L"Version 1" / L"Guide.txt").native(),
        L"quoted \"argument\"\nnext", fixture.root.native(), launcher::LaunchShow::minimized};
    scenario->capture_action(prepared, "runas", true);
    scenario->checkpoint("explicit-cleanup");
    std::ifstream trace(scenario->output_directory() / L"trace.jsonl");
    const std::string contents{std::istreambuf_iterator<char>(trace), {}};
    CHECK(contents.find("\"stage\":\"action-captured\"") != contents.npos);
    CHECK(contents.find("\"verb\":\"runas\"") != contents.npos);
    CHECK(contents.find("\\u000a") != contents.npos);
    CHECK(contents.find("explicit-cleanup") != contents.npos);
}
