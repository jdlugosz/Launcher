#include <windows.h>
#include <shlobj.h>

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/file_lookup.hpp"
#include "launcher/config_storage.hpp"
#include "launcher/config_error.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
namespace fs = std::filesystem;

struct Fixture {
    fs::path root, program, user, custom;
    Fixture() {
        static std::atomic<unsigned> sequence = 0;
        root = fs::temp_directory_path() / ("LauncherLookup-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetTickCount64()) + "-" + std::to_string(sequence++));
        REQUIRE(fs::create_directory(root));
        program = root / "portable";
        user = root / "user";
        custom = root / "custom";
        fs::create_directories(program);
        fs::create_directories(custom);
    }
    ~Fixture() {
        std::error_code ignored;
        const auto resolved = fs::weakly_canonical(root, ignored);
        if (!ignored && resolved.parent_path() == fs::weakly_canonical(fs::temp_directory_path()) &&
            resolved.filename().wstring().starts_with(L"LauncherLookup-")) {
            fs::remove_all(resolved, ignored);
        }
    }
    static void write(const fs::path& path, const std::string& content = "fixture") {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file << content;
        file.close();
        REQUIRE(file.good());
    }
    launcher::FileLookup files(const fs::path& configuration = {}, bool explicitly_named = false) const {
        return {program / "Launcher.exe", user, configuration, explicitly_named};
    }
};
}

TEST_CASE("File lookup retains the last directory identity and independent mode flags", "[file-lookup]") {
    Fixture fixture;
    fs::create_directory(fixture.user);
    const auto files = fixture.files(fixture.user / ".", true);
    REQUIRE(files.portable());
    REQUIRE(files.installation_scope() == launcher::InstallationScope::portable);
    REQUIRE(files.explicitly_named());
    REQUIRE(files.directories() == std::vector<fs::path>{fixture.program, fixture.user});
    const auto distinct = fixture.files(fixture.custom, true);
    REQUIRE(distinct.directories() == std::vector<fs::path>{fixture.custom, fixture.program, fixture.user});
    const auto implicit = fixture.files(fixture.custom, false);
    REQUIRE_FALSE(implicit.explicitly_named());
    REQUIRE(implicit.directories() == std::vector<fs::path>{fixture.program, fixture.user});
}

TEST_CASE("File lookup searches complete relative paths and absolute files without fallback", "[file-lookup]") {
    Fixture fixture;
    const auto files = fixture.files(fixture.custom, true);
    const fs::path name = "Icons/Bigicon.svg";
    Fixture::write(fixture.user / name, "user");
    REQUIRE(files.read_text(name).text == "user");
    Fixture::write(fixture.program / name, "portable");
    REQUIRE(files.require(name) == fixture.program / name);
    Fixture::write(fixture.custom / name, "custom");
    const auto loaded = files.read_text(name);
    REQUIRE(loaded.text == "custom");
    REQUIRE(fs::path(loaded.source) == fixture.custom / name);
    REQUIRE(files.require(fixture.user / name) == fixture.user / name);
    REQUIRE_FALSE(files.find(fixture.root / "missing.svg"));
    REQUIRE_THROWS_AS(files.require(fixture.root / "missing.svg"), launcher::ConfigError);
    REQUIRE_THROWS_AS(files.find("C:ambiguous.svg"), launcher::ConfigError);
}

TEST_CASE("File lookup reports higher-priority file errors without selecting a fallback", "[file-lookup]") {
    Fixture fixture;
    const auto files = fixture.files();
    Fixture::write(fixture.user / "locked.toml");
    Fixture::write(fixture.program / "locked.toml");
    const HANDLE locked = CreateFileW((fixture.program / "locked.toml").c_str(), GENERIC_READ, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(locked != INVALID_HANDLE_VALUE);
    std::string failure;
    try { (void)files.read_text("locked.toml"); }
    catch (const launcher::ConfigError& error) { failure = error.what(); }
    CloseHandle(locked);
    REQUIRE(failure.find("locked.toml") != std::string::npos);
    REQUIRE(failure.find("(32)") != std::string::npos);

    fs::create_directory(fixture.program / "directory.toml");
    Fixture::write(fixture.user / "directory.toml");
    REQUIRE_THROWS_AS(files.read_text("directory.toml"), launcher::ConfigError);
    Fixture::write(fixture.program / "Icons");
    Fixture::write(fixture.user / "Icons/x.svg");
    REQUIRE_THROWS_AS(files.require("Icons/x.svg"), launcher::ConfigError);
}

TEST_CASE("File lookup does not hide broken directory links behind a missing child", "[file-lookup]") {
    Fixture fixture;
    const auto link = fixture.program / "Icons";
    if (!CreateSymbolicLinkW(link.c_str(), (fixture.root / "missing-target").c_str(),
        SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        SKIP("Directory symbolic links are unavailable for this test token");
    }
    Fixture::write(fixture.user / "Icons/x.svg");
    REQUIRE_THROWS_AS(fixture.files().require("Icons/x.svg"), launcher::ConfigError);
}

TEST_CASE("Embedded fallback follows files and names the actual resource module", "[file-lookup]") {
    Fixture fixture;
    const auto files = fixture.files();
    const auto embedded = files.read_text("basic_colors.toml", launcher::FileFallback::embedded_resource);
    REQUIRE(embedded.source.ends_with("!resource:basic_colors.toml"));
    REQUIRE(embedded.source.find("LauncherTests.exe") != std::string::npos);
    REQUIRE_FALSE(embedded.text.empty());
    Fixture::write(fixture.user / "basic_colors.toml", "user");
    REQUIRE(files.read_text("basic_colors.toml", launcher::FileFallback::embedded_resource).text == "user");
    Fixture::write(fixture.program / "basic_colors.toml", "portable");
    REQUIRE(files.read_text("basic_colors.toml", launcher::FileFallback::embedded_resource).text == "portable");
}

TEST_CASE("Portable configuration discovery seeds only an omitted missing default", "[file-lookup][storage]") {
    Fixture fixture;
    constexpr auto starter = "[[tabs]]\nname='Starter'\nitems=[{icon='Icons/x.svg'}]\n";
    Fixture::write(fixture.program / "Launcher.default.toml", starter);
    Fixture::write(fixture.program / "Icons/x.svg");
    const auto files = fixture.files();
    REQUIRE_THROWS_AS(launcher::resolve_configuration_path("missing.toml", files), launcher::ConfigError);
    REQUIRE_FALSE(fs::exists(fixture.program / "Launcher.toml"));
    REQUIRE_FALSE(fs::exists(fixture.user));
    REQUIRE_THROWS_AS(launcher::resolve_configuration_path({}, fixture.files({}, true)), launcher::ConfigError);
    REQUIRE(launcher::resolve_configuration_path({}, files) == fixture.program / "Launcher.toml");
    REQUIRE(launcher::FileLookup::read_file(fixture.program / "Launcher.toml") == starter);
    REQUIRE_FALSE(fs::exists(fixture.user));
}

TEST_CASE("Installed configuration discovery requires a choice before creating a profile", "[file-lookup][storage]") {
    Fixture fixture;
    PWSTR program_files = nullptr;
    REQUIRE(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DONT_VERIFY, nullptr, &program_files)));
    const auto executable = fs::path(program_files) / L"Długosz/Launcher/Launcher.exe";
    CoTaskMemFree(program_files);
    const launcher::FileLookup files(executable, fixture.user);
    REQUIRE_FALSE(files.portable());
    REQUIRE(files.installation_scope() == launcher::InstallationScope::all_users);
    REQUIRE(files.directories() == std::vector<fs::path>{fixture.user});
    // Installed program payloads are exact sources, never editable-data roots.
    REQUIRE_FALSE(files.find(L"Launcher.exe"));
    REQUIRE_FALSE(files.find(L"Launcher.default.toml"));
    Fixture::write(fixture.user / "colors-custom.toml", "user palette");
    REQUIRE(files.read_text("colors-custom.toml").text == "user palette");
    const auto missing = files.missing_message("missing.toml");
    REQUIRE(missing.find("Program Files") == std::string::npos);
    // An explicit missing file must fail before any offer or seed.
    bool offered = false;
    REQUIRE_THROWS_AS(launcher::resolve_configuration_path(fixture.root / "missing.toml", files,
        [&](const auto&) { offered = true; return true; }), launcher::ConfigError);
    REQUIRE_FALSE(offered);
    REQUIRE_FALSE(fs::exists(fixture.user / "Launcher.toml"));
    REQUIRE_FALSE(files.find(L"Launcher.toml"));
    REQUIRE_THROWS_AS(launcher::resolve_configuration_path({}, files), launcher::ConfigError);
    REQUIRE(launcher::resolve_configuration_path({}, files, [&](const auto& destination) {
        REQUIRE(destination == fixture.user / L"Launcher.toml");
        offered = true;
        return false;
    }).empty());
    REQUIRE(offered);
    REQUIRE_FALSE(fs::exists(fixture.user / "Launcher.toml"));
}

TEST_CASE("A per-user installation stays installed with an explicit configuration", "[file-lookup]") {
    Fixture fixture;
    PWSTR local_app_data = nullptr;
    REQUIRE(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &local_app_data)));
    const auto executable = fs::path(local_app_data) / L"Programs/Długosz/Launcher/Launcher.exe";
    CoTaskMemFree(local_app_data);
    const launcher::FileLookup files(executable, fixture.user, fixture.custom, true);
    REQUIRE_FALSE(files.portable());
    REQUIRE(files.installation_scope() == launcher::InstallationScope::current_user);
    REQUIRE(files.directories() == std::vector<fs::path>{fixture.custom, fixture.user});
}

TEST_CASE("Installed lookup never appends its executable directory after AppData", "[file-lookup]") {
    Fixture fixture;
    PWSTR program_files = nullptr;
    REQUIRE(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DONT_VERIFY, nullptr, &program_files)));
    const auto executable = fs::path(program_files) / L"Długosz/Launcher/Launcher.exe";
    CoTaskMemFree(program_files);
    if (fixture.root.root_name() != executable.root_name()) {
        SKIP("The isolated relative-path decoy requires Temp and Program Files on the same drive");
    }
    const launcher::FileLookup files(executable, fixture.user);
    REQUIRE_FALSE(files.portable());
    const auto decoy = fixture.program / "colors-custom.toml";
    Fixture::write(decoy, "executable-relative decoy");
    // This request would reach the isolated decoy if the installed EXE were
    // appended as another root. No actual installation directory is modified.
    const auto request = decoy.lexically_relative(executable.parent_path());
    REQUIRE_FALSE(request.empty());
    REQUIRE_FALSE(fs::exists(fixture.user / request));
    REQUIRE(fs::equivalent(files.require(executable.parent_path() / request, launcher::FileFallback::exact_file), decoy));
    REQUIRE_FALSE(files.find(request));
    REQUIRE_THROWS_AS(files.require(request), launcher::ConfigError);
}
