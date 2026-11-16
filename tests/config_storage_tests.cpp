#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/config_storage.hpp"
#include "launcher/file_lookup.hpp"
#include "launcher/resource_module.hpp"
#include "launcher/settings.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

namespace fs = std::filesystem;

constexpr std::string_view valid_default = R"toml(# Bundled default
schema_version = 1
[[tabs]]
name = "General"
items = [{ label = "Dummy", icon = { stock = "folder" } }]
)toml";

class TemporaryFixture {
public:
    TemporaryFixture() : parent_(fs::weakly_canonical(fs::temp_directory_path())) {
        static std::atomic<unsigned> sequence = 0;
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            root_ = parent_ / ("LauncherConfigStorageTests-" +
                std::to_string(GetCurrentProcessId()) + "-" + std::to_string(stamp) + "-" +
                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
            std::error_code error;
            if (fs::create_directory(root_, error)) {
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::runtime_error("Cannot create isolated configuration-storage fixture: " +
                                         error.message());
            }
        }
        throw std::runtime_error("Cannot allocate an isolated configuration-storage fixture");
    }

    TemporaryFixture(const TemporaryFixture&) = delete;
    TemporaryFixture& operator=(const TemporaryFixture&) = delete;

    ~TemporaryFixture() {
        std::error_code error;
        const auto resolved = fs::weakly_canonical(root_, error);
        // Verify the absolute target is the exact directory this fixture
        // exclusively created, under the chosen temporary parent.
        if (!error && resolved == root_ && resolved.parent_path() == parent_) {
            fs::remove_all(resolved, error);
        }
    }

    fs::path path(std::string_view name) const { return root_ / name; }

private:
    fs::path parent_;
    fs::path root_;
};

void write_bytes(const fs::path& path, std::string_view content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    file.close();
    REQUIRE(file.good());
}

std::string read_bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void require_only_configuration(const fs::path& directory) {
    std::size_t entries = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        REQUIRE(entry.path().filename() == "Launcher.toml");
        ++entries;
    }
    REQUIRE(entries == 1);
}

class ExclusiveFileLock {
public:
    explicit ExclusiveFileLock(const fs::path& path)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ExclusiveFileLock(const ExclusiveFileLock&) = delete;
    ExclusiveFileLock& operator=(const ExclusiveFileLock&) = delete;
    ~ExclusiveFileLock() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

class ReadOnlyFixtureFile {
public:
    explicit ReadOnlyFixtureFile(fs::path path)
        : path_(std::move(path)), original_attributes_(GetFileAttributesW(path_.c_str())) {
        if (original_attributes_ != INVALID_FILE_ATTRIBUTES) {
            applied_ = SetFileAttributesW(path_.c_str(),
                                          original_attributes_ | FILE_ATTRIBUTE_READONLY) != 0;
        }
    }
    ReadOnlyFixtureFile(const ReadOnlyFixtureFile&) = delete;
    ReadOnlyFixtureFile& operator=(const ReadOnlyFixtureFile&) = delete;
    ~ReadOnlyFixtureFile() {
        // Restore only this test's own template, before its fixture is removed.
        if (applied_) {
            SetFileAttributesW(path_.c_str(), original_attributes_);
        }
    }
    bool valid() const { return applied_; }
    DWORD readonly_attributes() const { return original_attributes_ | FILE_ATTRIBUTE_READONLY; }

private:
    fs::path path_;
    DWORD original_attributes_ = INVALID_FILE_ATTRIBUTES;
    bool applied_ = false;
};

fs::path installed_executable() {
    PWSTR program_files = nullptr;
    const auto result = SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DONT_VERIFY, nullptr, &program_files);
    const auto executable = SUCCEEDED(result) ? fs::path(program_files) / L"Długosz/Launcher/Launcher.exe" : fs::path{};
    CoTaskMemFree(program_files);
    REQUIRE(SUCCEEDED(result));
    return executable;
}

} // namespace

TEST_CASE("First startup seeds and validates the user configuration", "[config][storage]") {
    TemporaryFixture fixture;
    const auto bundled = fixture.path("bundled.toml");
    const auto directory = fixture.path("user") / "nested";
    const auto source = std::string(valid_default) + "\n%STOP ===========examples below\nUnfinished example {\n";
    write_bytes(bundled, source);

    const auto result = launcher::ensure_user_configuration(directory, bundled);
    REQUIRE(result == directory / "Launcher.toml");
    REQUIRE(read_bytes(result) == source);
    REQUIRE(launcher::load_configuration(result).tabs[0].name == "General");
    require_only_configuration(directory);
}

TEST_CASE("An existing customized configuration is never overwritten", "[config][storage]") {
    TemporaryFixture fixture;
    const auto bundled = fixture.path("bundled.toml");
    const auto directory = fixture.path("user");
    fs::create_directory(directory);
    const auto configuration = directory / "Launcher.toml";
    const std::string custom = "# Keep this comment and formatting\n[dock]\nedge='left'\n"
                               "[[tabs]]\nname='My Custom Tab'\n";
    write_bytes(configuration, custom);
    const auto original_time = fs::last_write_time(configuration);
    write_bytes(bundled, valid_default);

    REQUIRE(launcher::ensure_user_configuration(directory, bundled) == configuration);
    REQUIRE(read_bytes(configuration) == custom);
    REQUIRE(fs::last_write_time(configuration) == original_time);
    require_only_configuration(directory);
}

TEST_CASE("An existing malformed configuration is preserved without fallback or validation", "[config][storage]") {
    TemporaryFixture fixture;
    const auto directory = fixture.path("user");
    fs::create_directory(directory);
    const auto configuration = directory / "Launcher.toml";
    const std::string malformed = "[dock\nedge = 'left'\n";
    write_bytes(configuration, malformed);
    const auto original_time = fs::last_write_time(configuration);

    SECTION("Even when the bundled default is missing") {
        REQUIRE(launcher::ensure_user_configuration(directory, fixture.path("missing.toml")) ==
                configuration);
    }
    SECTION("Even when the bundled default is also malformed") {
        const auto bundled = fixture.path("invalid-default.toml");
        write_bytes(bundled, "tabs = 'invalid'\n");
        REQUIRE(launcher::ensure_user_configuration(directory, bundled) == configuration);
    }
    REQUIRE(read_bytes(configuration) == malformed);
    REQUIRE(fs::last_write_time(configuration) == original_time);
    REQUIRE_THROWS_AS(launcher::load_configuration(configuration), launcher::ConfigError);
    require_only_configuration(directory);
}

TEST_CASE("A missing or unreadable bundled default never publishes a user configuration", "[config][storage]") {
    TemporaryFixture fixture;
    const auto bundled = fixture.path("bundled.toml");
    const auto directory = fixture.path("user");
    SECTION("Missing file") {
        REQUIRE_THROWS_AS(launcher::ensure_user_configuration(directory, bundled),
                          launcher::ConfigError);
    }
    SECTION("File held without read sharing") {
        write_bytes(bundled, valid_default);
        ExclusiveFileLock lock(bundled);
        REQUIRE(lock.valid());
        REQUIRE_THROWS_AS(launcher::ensure_user_configuration(directory, bundled),
                          launcher::ConfigError);
    }
    REQUIRE_FALSE(fs::exists(directory / "Launcher.toml"));
    REQUIRE(fs::is_empty(directory));
}

TEST_CASE("An invalid bundled default is rejected before publication", "[config][storage]") {
    TemporaryFixture fixture;
    const auto bundled = fixture.path("bundled.toml");
    const auto directory = fixture.path("user");
    SECTION("Malformed TOML") {
        write_bytes(bundled, "[dock\nedge = 'left'\n");
    }
    SECTION("Invalid typed configuration") {
        write_bytes(bundled, "[dock]\nhover_delay_ms = -1\n[[tabs]]\nname='General'\n");
    }
    SECTION("Invalid fixed layout") {
        write_bytes(bundled, "[grid]\ncolumns=1\nrows=1\n[[tabs]]\nname='General'\n"
                            "items=[{label='One'}, {label='Two'}]\n");
    }
    REQUIRE_THROWS_AS(launcher::ensure_user_configuration(directory, bundled),
                      launcher::ConfigError);
    REQUIRE_FALSE(fs::exists(directory / "Launcher.toml"));
    REQUIRE(fs::is_empty(directory));
}

TEST_CASE("Storage directory and destination collisions preserve existing content", "[config][storage]") {
    TemporaryFixture fixture;
    const auto bundled = fixture.path("bundled.toml");
    const auto directory = fixture.path("user");
    write_bytes(bundled, valid_default);

    SECTION("The requested user directory is a file") {
        write_bytes(directory, "Leave this file alone");
        REQUIRE_THROWS_AS(launcher::ensure_user_configuration(directory, bundled),
                          launcher::ConfigError);
        REQUIRE(read_bytes(directory) == "Leave this file alone");
    }
    SECTION("Launcher.toml is a directory") {
        fs::create_directories(directory / "Launcher.toml");
        write_bytes(directory / "Launcher.toml" / "sentinel.txt", "Preserve this directory");
        REQUIRE_THROWS_AS(launcher::ensure_user_configuration(directory, bundled),
                          launcher::ConfigError);
        REQUIRE(read_bytes(directory / "Launcher.toml" / "sentinel.txt") ==
                "Preserve this directory");
    }
}

TEST_CASE("Concurrent first startups publish one complete configuration without clobbering", "[config][storage]") {
    TemporaryFixture fixture;
    const auto bundled = fixture.path("bundled.toml");
    const auto directory = fixture.path("user");
    write_bytes(bundled, valid_default);
    std::barrier start(2);
    const auto initialize = [&] {
        start.arrive_and_wait();
        return launcher::ensure_user_configuration(directory, bundled);
    };
    auto first = std::async(std::launch::async, initialize);
    auto second = std::async(std::launch::async, initialize);
    const auto first_path = first.get();
    const auto second_path = second.get();

    REQUIRE(first_path == directory / "Launcher.toml");
    REQUIRE(second_path == first_path);
    REQUIRE(read_bytes(first_path) == valid_default);
    require_only_configuration(directory);
}

TEST_CASE("Configuration arguments use uniform lookup without creating explicit files", "[config][storage][arguments]") {
    TemporaryFixture fixture;
    const auto user = fixture.path("user") / L"D\u0142ugosz" / L"Launcher";
    const auto program = fixture.path("program");
    fs::create_directory(program);
    const auto bundled = program / "Launcher.default.toml";
    write_bytes(bundled, valid_default);
    const launcher::FileLookup files(program / "Launcher.exe", user);

    SECTION("Omitted or empty selects and seeds the normal default") {
        const auto path = launcher::resolve_configuration_path({}, files);
        REQUIRE(path == program / "Launcher.toml");
        REQUIRE(read_bytes(path) == valid_default);
        write_bytes(path, "# My existing settings\n");
        REQUIRE(launcher::resolve_configuration_path({}, files) == path);
        REQUIRE(read_bytes(path) == "# My existing settings\n");
    }
    SECTION("Absolute selects that file without seeding user settings") {
        const auto explicit_path = fixture.path("absolute.toml");
        REQUIRE_THROWS_AS(launcher::resolve_configuration_path(explicit_path, files), launcher::ConfigError);
        write_bytes(explicit_path, "# explicit\n");
        REQUIRE(launcher::resolve_configuration_path(explicit_path, files) == explicit_path);
        REQUIRE_FALSE(fs::exists(user));
    }
    SECTION("Relative preserves its subtree beneath the search roots") {
        fs::create_directories(user / "test configs");
        write_bytes(user / "test configs" / "alternate.toml", "# alternate\n");
        REQUIRE(launcher::resolve_configuration_path("test configs/alternate.toml", files) ==
            user / "test configs" / "alternate.toml");
        REQUIRE_THROWS_AS(launcher::resolve_configuration_path("Launcher.toml", files), launcher::ConfigError);
        REQUIRE_FALSE(fs::exists(program / "Launcher.toml"));
    }
    SECTION("Ambiguous Windows drive-relative paths report an error") {
        REQUIRE_THROWS_AS(launcher::resolve_configuration_path("C:alternate.toml", files), launcher::ConfigError);
        REQUIRE_THROWS_AS(launcher::resolve_configuration_path("\\alternate.toml", files), launcher::ConfigError);
    }
}

TEST_CASE("A read-only bundled default seeds editable settings and stays unchanged", "[config][storage]") {
    TemporaryFixture fixture;
    const auto bundled = fixture.path("bundled.toml");
    const auto directory = fixture.path("user");
    write_bytes(bundled, valid_default);
    ReadOnlyFixtureFile readonly_default(bundled);
    REQUIRE(readonly_default.valid());

    const auto configuration = launcher::ensure_user_configuration(directory, bundled);
    const auto user_attributes = GetFileAttributesW(configuration.c_str());
    REQUIRE(user_attributes != INVALID_FILE_ATTRIBUTES);
    REQUIRE((user_attributes & FILE_ATTRIBUTE_READONLY) == 0);
    REQUIRE(GetFileAttributesW(bundled.c_str()) == readonly_default.readonly_attributes());
    REQUIRE(read_bytes(bundled) == valid_default);

    const std::string customized = "# User can customize this file\n" + std::string(valid_default);
    write_bytes(configuration, customized);
    REQUIRE(read_bytes(configuration) == customized);
    REQUIRE(read_bytes(bundled) == valid_default);
    require_only_configuration(directory);
}

TEST_CASE("An invalid read-only default leaves no published configuration or staging files", "[config][storage]") {
    TemporaryFixture fixture;
    const auto bundled = fixture.path("bundled.toml");
    const auto directory = fixture.path("user");
    const std::string malformed = "[dock\nedge='left'\n";
    write_bytes(bundled, malformed);
    ReadOnlyFixtureFile readonly_default(bundled);
    REQUIRE(readonly_default.valid());

    REQUIRE_THROWS_AS(launcher::ensure_user_configuration(directory, bundled),
                      launcher::ConfigError);
    REQUIRE_FALSE(fs::exists(directory / "Launcher.toml"));
    REQUIRE(fs::is_empty(directory));
    REQUIRE(GetFileAttributesW(bundled.c_str()) == readonly_default.readonly_attributes());
    REQUIRE(read_bytes(bundled) == malformed);
}

TEST_CASE("The portable color demo keeps its original configuration and sibling runtime", "[config][storage][color-demo]") {
    TemporaryFixture fixture;
    const auto program = fixture.path("portable");
    const auto user = fixture.path("user");
    fs::create_directories(program / "examples");
    const auto bundled = program / "examples" / "demo_many_colors.toml";
    write_bytes(bundled, valid_default);
    const launcher::FileLookup files(program / "Launcher.exe", user);
    REQUIRE(files.portable());
    const auto runtime = files.runtime_path(bundled);
    const std::string original_runtime = "[dock]\nposition=0.37\n";
    write_bytes(runtime, original_runtime);

    REQUIRE(launcher::resolve_color_demo_configuration(bundled, files) == bundled);
    REQUIRE(read_bytes(bundled) == valid_default);
    REQUIRE(read_bytes(runtime) == original_runtime);
    REQUIRE(launcher::load_runtime_adjustments(runtime, files).edge_position == 0.37);
    REQUIRE_FALSE(fs::exists(user));
    REQUIRE_THROWS_AS(launcher::resolve_color_demo_configuration(program / "missing.toml", files), launcher::ConfigError);
    REQUIRE_FALSE(fs::exists(user));
}

TEST_CASE("Configuration recovery chooses the installed starter only in installed mode", "[config][storage][installed-starter]") {
    TemporaryFixture fixture;
    const auto program = fixture.path("program");
    const auto user = fixture.path("user");
    fs::create_directory(program);
    const auto portable_template = program / "Launcher.default.toml";
    const auto installed_template = program / "Launcher.installed.default.toml";
    const auto portable_text = "# Portable starter\n" + std::string(valid_default);
    const auto installed_text = "# Installed starter\n" + std::string(valid_default) +
        "\n[[tabs]]\nname='Demo'\nitems=[{target='$SELF'}]\n";
    write_bytes(portable_template, portable_text);
    write_bytes(installed_template, installed_text);
    fs::create_directory(program / "examples");
    const auto demo_template = program / "examples" / "demo_many_colors.toml";
    const auto demo_text = "# Personal demo seed\n" + std::string(valid_default);
    write_bytes(demo_template, demo_text);
    const auto demo_configuration = user / "Demos" / "demo_many_colors.toml";
    const launcher::FileLookup installed_files(installed_executable(), user);
    const launcher::FileLookup portable_files(program / "Launcher.exe", user);
    fs::path result;
    SECTION("Installed recovery uses the installed variant") {
        result = launcher::ensure_user_configuration(user, portable_template, installed_files);
        REQUIRE(read_bytes(result) == installed_text);
        REQUIRE(read_bytes(demo_configuration).find("All bundled color names") != std::string::npos);
        REQUIRE(fs::exists(user / "Icons" / "many-colors.svg"));
        REQUIRE_FALSE(fs::exists(installed_files.runtime_path(demo_configuration)));
        const auto loaded = launcher::load_configuration_files(result, installed_files);
        const auto executable = installed_files.executable().u8string();
        REQUIRE(loaded.configuration.tabs.back().items.front().launch->target == std::string(executable.begin(), executable.end()));
    }
    SECTION("Portable first use retains the original portable starter") {
        result = launcher::ensure_user_configuration(user, portable_template, portable_files);
        REQUIRE(read_bytes(result) == portable_text);
        REQUIRE_FALSE(fs::exists(user / "Demos"));
    }
    SECTION("An older installed payload falls back when no installed variant exists") {
        fs::remove(installed_template);
        result = launcher::ensure_user_configuration(user, portable_template, installed_files);
        REQUIRE(read_bytes(result) == portable_text);
        REQUIRE_FALSE(fs::exists(user / "Demos"));
    }
    SECTION("Other template names retain their exact source") {
        const auto other_template = program / "demo_many_colors.toml";
        write_bytes(other_template, portable_text);
        result = launcher::ensure_user_configuration(user, other_template, installed_files);
        REQUIRE(read_bytes(result) == portable_text);
    }
    SECTION("An existing user configuration is preserved without reading templates") {
        fs::create_directory(user);
        const auto existing = user / "Launcher.toml";
        write_bytes(existing, "# Preserve an unfinished personal edit\n[tabs\n");
        fs::remove(installed_template);
        fs::create_directory(installed_template);
        result = launcher::ensure_user_configuration(user, portable_template, installed_files);
        REQUIRE(result == existing);
        REQUIRE(read_bytes(result) == "# Preserve an unfinished personal edit\n[tabs\n");
    }
    SECTION("A present invalid installed starter never selects the portable fallback") {
        write_bytes(installed_template, "[tabs\n");
        REQUIRE_THROWS_AS(launcher::ensure_user_configuration(user, portable_template, installed_files), launcher::ConfigError);
        REQUIRE_FALSE(fs::exists(user / "Launcher.toml"));
    }
    SECTION("Installed recovery preserves an existing edited demo and runtime") {
        fs::create_directories(demo_configuration.parent_path());
        write_bytes(demo_configuration, "# Preserve edited demo\n[tabs\n");
        const auto runtime = installed_files.runtime_path(demo_configuration);
        write_bytes(runtime, "[dock]\nposition=0.27\n");
        result = launcher::ensure_user_configuration(user, portable_template, installed_files);
        REQUIRE(read_bytes(result) == installed_text);
        REQUIRE(read_bytes(demo_configuration) == "# Preserve edited demo\n[tabs\n");
        REQUIRE(read_bytes(runtime) == "[dock]\nposition=0.27\n");
    }
    SECTION("A missing installed demo file uses the embedded default") {
        fs::remove(demo_template);
        result = launcher::ensure_user_configuration(user, portable_template, installed_files);
        REQUIRE(read_bytes(result) == installed_text);
        REQUIRE(fs::exists(demo_configuration));
    }
    REQUIRE(read_bytes(portable_template) == portable_text);
}

TEST_CASE("The installed color demo seeds an editable isolated profile and saves its own position", "[config][storage][color-demo]") {
    TemporaryFixture fixture;
    const auto bundled = fixture.path("bundled-demo.toml");
    const auto user = fixture.path("user");
    fs::create_directory(user);
    const std::string personal = "# Preserve the normal personal configuration\n";
    const std::string personal_runtime = "# Preserve normal saved sizes and position\n";
    write_bytes(user / "Launcher.toml", personal);
    write_bytes(user / "Launcher.runtime.toml", personal_runtime);
    const auto source = std::string(valid_default) + "\n# Preserve bundled formatting and comments\n";
    write_bytes(bundled, source);
    ReadOnlyFixtureFile readonly_demo(bundled);
    REQUIRE(readonly_demo.valid());
    const launcher::FileLookup files(installed_executable(), user);
    REQUIRE_FALSE(files.portable());

    const auto configuration = launcher::resolve_color_demo_configuration(bundled, files);
    REQUIRE(configuration == user / "Demos" / "demo_many_colors.toml");
    launcher::ResourceModule module(nullptr);
    const auto embedded = module.FetchText(std::uint16_t{10}, std::string("DEMO_CONFIGURATION"));
    REQUIRE(read_bytes(configuration) == embedded);
    REQUIRE(std::distance(fs::directory_iterator(configuration.parent_path()), fs::directory_iterator{}) == 1);
    const auto attributes = GetFileAttributesW(configuration.c_str());
    REQUIRE(attributes != INVALID_FILE_ATTRIBUTES);
    REQUIRE((attributes & FILE_ATTRIBUTE_READONLY) == 0);
    const auto runtime = files.runtime_path(configuration);
    REQUIRE(runtime == configuration.parent_path() / "demo_many_colors.runtime.toml");
    launcher::RuntimeAdjustments moved;
    moved.edge_position = 0.23;
    moved.icon_spacing_px = 8U;
    launcher::save_runtime_adjustments(runtime, moved, files);
    REQUIRE(launcher::load_runtime_adjustments(runtime, files).edge_position == moved.edge_position);
    REQUIRE(read_bytes(configuration) == embedded);
    REQUIRE(read_bytes(user / "Launcher.toml") == personal);
    REQUIRE(read_bytes(user / "Launcher.runtime.toml") == personal_runtime);
    REQUIRE(GetFileAttributesW(bundled.c_str()) == readonly_demo.readonly_attributes());
    REQUIRE(read_bytes(bundled) == source);
    REQUIRE_FALSE(fs::exists(files.runtime_path(bundled)));
}

TEST_CASE("Repeated installed color demos preserve edits and runtime even without their template", "[config][storage][color-demo]") {
    TemporaryFixture fixture;
    const auto user = fixture.path("user");
    const auto directory = user / "Demos";
    fs::create_directories(directory);
    const auto configuration = directory / "demo_many_colors.toml";
    const auto runtime = directory / "demo_many_colors.runtime.toml";
    const std::string customized = "# Preserve even an unfinished user edit\n[tabs\n";
    const std::string saved = "[dock]\nposition=0.19\n";
    const std::string personal = "# Preserve my main configuration\n";
    const std::string personal_runtime = "# Preserve my main runtime settings\n";
    write_bytes(configuration, customized);
    write_bytes(runtime, saved);
    write_bytes(user / "Launcher.toml", personal);
    write_bytes(user / "Launcher.runtime.toml", personal_runtime);
    const auto original_time = fs::last_write_time(configuration);
    const launcher::FileLookup files(installed_executable(), user);

    REQUIRE(launcher::resolve_color_demo_configuration(fixture.path("missing-demo.toml"), files) == configuration);
    REQUIRE(read_bytes(configuration) == customized);
    REQUIRE(read_bytes(runtime) == saved);
    REQUIRE(fs::last_write_time(configuration) == original_time);
    REQUIRE(read_bytes(user / "Launcher.toml") == personal);
    REQUIRE(read_bytes(user / "Launcher.runtime.toml") == personal_runtime);
}

TEST_CASE("Legacy installed demo migration keeps edits and saved position", "[config][storage][color-demo]") {
    TemporaryFixture fixture;
    const auto user = fixture.path("user");
    const auto legacy = user / "Demos" / "ManyColors" / "Launcher.toml";
    fs::create_directories(legacy.parent_path());
    const std::string edited = "# Preserve my unfinished demo edit\n[tabs\n";
    const std::string runtime = "[dock]\nposition=0.37\n";
    write_bytes(legacy, edited);
    write_bytes(legacy.parent_path() / "Launcher.runtime.toml", runtime);
    const launcher::FileLookup files(installed_executable(), user);
    const auto migrated = launcher::resolve_color_demo_configuration(fixture.path("absent-source.toml"), files);
    REQUIRE(migrated == user / "Demos" / "demo_many_colors.toml");
    REQUIRE(read_bytes(migrated) == edited);
    REQUIRE(read_bytes(files.runtime_path(migrated)) == runtime);
    REQUIRE(read_bytes(legacy) == edited);
}

TEST_CASE("Installed demo creation does not read program directory examples", "[config][storage][color-demo]") {
    TemporaryFixture fixture;
    const auto bundled = fixture.path("bundled-demo.toml");
    const auto user = fixture.path("user");
    const auto directory = user / "Demos";
    const launcher::FileLookup files(installed_executable(), user);

    SECTION("Missing template") {
        REQUIRE(fs::exists(launcher::resolve_color_demo_configuration(bundled, files)));
    }
    SECTION("Invalid template") {
        write_bytes(bundled, "[dock\nedge='left'\n");
        REQUIRE(fs::exists(launcher::resolve_color_demo_configuration(bundled, files)));
    }
    SECTION("Unreadable template") {
        write_bytes(bundled, valid_default);
        ExclusiveFileLock lock(bundled);
        REQUIRE(lock.valid());
        REQUIRE(fs::exists(launcher::resolve_color_demo_configuration(bundled, files)));
    }
    REQUIRE(fs::exists(directory / "demo_many_colors.toml"));
    REQUIRE(fs::exists(user / "Icons" / "many-colors.svg"));
    REQUIRE_FALSE(fs::exists(directory / "demo_many_colors.runtime.toml"));
    REQUIRE_FALSE(fs::exists(user / "Launcher.toml"));
    REQUIRE_FALSE(fs::exists(user / "Launcher.runtime.toml"));
}

TEST_CASE("The color demo requires a real user directory in installed mode", "[config][storage][color-demo]") {
    TemporaryFixture fixture;
    const auto bundled = fixture.path("bundled-demo.toml");
    write_bytes(bundled, valid_default);
    const launcher::FileLookup files(installed_executable(), {});
    REQUIRE_FALSE(files.portable());
    REQUIRE_THROWS_AS(launcher::resolve_color_demo_configuration(bundled, files), launcher::ConfigError);
    REQUIRE(read_bytes(bundled) == valid_default);
}
