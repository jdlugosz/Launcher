#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/target_versions.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

class TargetFixture final {
public:
    TargetFixture() : parent_(fs::weakly_canonical(fs::temp_directory_path())) {
        static std::atomic<unsigned> sequence = 0;
        root_ = parent_ / (L"LauncherTargetVersions-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence++));
        if (!fs::create_directory(root_)) { throw std::runtime_error("Cannot create target-version test directory"); }
    }
    ~TargetFixture() {
        std::error_code error;
        const auto resolved = fs::weakly_canonical(root_, error);
        if (!error && resolved == root_ && resolved.parent_path() == parent_) { fs::remove_all(root_, error); }
    }
    fs::path path(std::wstring_view name) const { return (root_ / name).lexically_normal(); }
    fs::path file(std::wstring_view name) const {
        const auto destination = path(name);
        fs::create_directories(destination.parent_path());
        std::ofstream output(destination, std::ios::binary);
        output << "An ordinary file without executable version resources.";
        output.close();
        if (!output) { throw std::runtime_error("Cannot create target-version test file"); }
        return destination;
    }

private:
    fs::path parent_;
    fs::path root_;
};

std::wstring uppercase(std::wstring text) {
    if (!text.empty()) { CharUpperBuffW(text.data(), static_cast<DWORD>(text.size())); }
    return text;
}

launcher::TargetVersion target_version(fs::path executable, const fs::path& pattern) {
    launcher::TargetVersion result;
    result.executable = std::move(executable);
    result.target_pattern = pattern.wstring();
    result.path_label = result.executable.parent_path().filename().wstring();
    return result;
}

bool includes(const std::vector<launcher::TargetVersion>& versions, const fs::path& path) {
    return std::ranges::any_of(versions, [&path](const auto& version) { return version.executable == path; });
}

} // namespace

TEST_CASE("Target version detection accepts native star and question wildcards", "[target-versions]") {
    CHECK(launcher::has_target_wildcards(L"C:\\Tools\\Blender *\\blender.exe"));
    CHECK(launcher::has_target_wildcards(L"C:\\Tools\\Blender?\\blender.exe"));
    CHECK(launcher::has_target_wildcards(L"Report *.*"));
    CHECK_FALSE(launcher::has_target_wildcards(L"C:\\Tools\\Blender 5.2\\blender.exe"));
    CHECK_FALSE(launcher::has_target_wildcards(LR"(\\?\C:\Tools\Blender 5.2\blender.exe)"));
    CHECK(launcher::has_target_wildcards(LR"(\\?\C:\Tools\Blender *\blender.exe)"));
    CHECK_FALSE(launcher::has_target_wildcards(L""));
}

TEST_CASE("Target versions traverse wildcard components and order natural version numbers", "[target-versions]") {
    const TargetFixture fixture;
    const auto old = fixture.file(L"Blender Foundation/Blender 5.9/blender.exe");
    const auto newest = fixture.file(L"Blender Foundation/Blender 5.10/blender.exe");
    const auto previous = fixture.file(L"Blender Foundation/Blender 4.5/blender.exe");
    fixture.file(L"Blender Foundation/Blender 99.0/Other.exe");
    fixture.file(L"Blender Foundation/Blender 100.0/Nested/blender.exe");
    fixture.file(L"Other Foundation/Blender 200.0/blender.exe");
    const auto pattern = fixture.path(L"Blender*/Blender*/blender.exe");
    const auto versions = launcher::scan_target_versions(pattern);
    REQUIRE(versions.size() == 3);
    CHECK(versions[0].executable == newest);
    CHECK(versions[1].executable == old);
    CHECK(versions[2].executable == previous);
    for (const auto& version : versions) {
        CHECK(version.target_pattern == pattern.wstring());
        CHECK_FALSE(version.has_version_resource);
    }
    const auto extended_pattern = fs::path(L"\\\\?\\" + pattern.wstring());
    const auto extended_versions = launcher::scan_target_versions(extended_pattern);
    REQUIRE(extended_versions.size() == versions.size());
    for (std::size_t index = 0; index < versions.size(); ++index) {
        CHECK(extended_versions[index].executable == fs::path(L"\\\\?\\" + versions[index].executable.wstring()));
    }
}

TEST_CASE("Target versions retain Win32 wildcard matching for files without extensions", "[target-versions]") {
    const TargetFixture fixture;
    const auto readme = fixture.file(L"Documents 5.10/README");
    const auto pdf = fixture.file(L"Documents 5.10/Report.pdf");
    REQUIRE(fs::create_directory(fixture.path(L"Documents 5.10/Only a directory")));
    const auto versions = launcher::scan_target_versions(fixture.path(L"Documents*/*.*"));
    REQUIRE(versions.size() == 2);
    CHECK(includes(versions, readme));
    CHECK(includes(versions, pdf));

    const auto no_suffix = fixture.file(L"Question/Tool.txt");
    const auto digit = fixture.file(L"Question/Tool1.txt");
    const auto letter = fixture.file(L"Question/ToolA.txt");
    fixture.file(L"Question/Tool10.txt");
    const auto questions = launcher::scan_target_versions(fixture.path(L"Question/Tool?.txt"));
    REQUIRE(questions.size() == 3);
    CHECK(includes(questions, no_suffix));
    CHECK(includes(questions, digit));
    CHECK(includes(questions, letter));
}

TEST_CASE("Target versions accept document files and omit absent final branches", "[target-versions]") {
    const TargetFixture fixture;
    const auto old = fixture.file(L"Release 5.9/Guide 资料.pdf");
    const auto newest = fixture.file(L"Release 5.10/Guide 资料.pdf");
    fixture.file(L"Release 99.0/Unrelated.pdf");
    const auto versions = launcher::scan_target_versions(fixture.path(L"Release */Guide 资料.pdf"));
    REQUIRE(versions.size() == 2);
    CHECK(versions[0].executable == newest);
    CHECK(versions[1].executable == old);
    CHECK(launcher::scan_target_versions(fixture.path(L"Absent */Guide 资料.pdf")).empty());
    CHECK(launcher::scan_target_versions(fixture.path(L"Release */Missing.pdf")).empty());
}

TEST_CASE("Target version filtering normalizes pattern keys and duplicate paths case insensitively", "[target-versions]") {
    const TargetFixture fixture;
    const auto pattern = fixture.path(L"Tools */Report.pdf");
    const auto newest = fixture.path(L"Tools 5.10/Report.pdf");
    const auto old = fixture.path(L"Tools 5.9/Report.pdf");
    const std::vector catalog{
        target_version(old, pattern),
        target_version(newest, pattern),
        target_version(uppercase(newest.wstring()), uppercase(pattern.wstring())),
        target_version(fixture.path(L"Other 99.0/Report.pdf"), fixture.path(L"Other */Report.pdf")),
        target_version(fixture.path(L"Adobe.exe"), {})
    };
    auto alternative = uppercase((fixture.path(L"Unused") / L".." / L"Tools *" / L"Report.pdf").wstring());
    std::ranges::replace(alternative, L'\\', L'/');
    const auto versions = launcher::target_versions_for_pattern(catalog, alternative);
    REQUIRE(versions.size() == 2);
    CHECK(uppercase(versions[0].executable.wstring()) == uppercase(newest.wstring()));
    CHECK(uppercase(versions[1].executable.wstring()) == uppercase(old.wstring()));
    CHECK(launcher::target_versions_for_pattern(catalog, fixture.path(L"Unmatched */Report.pdf")).empty());
}

TEST_CASE("Target version ordering uses numeric product and file versions when available", "[target-versions]") {
    const fs::path pattern = L"C:\\Tools\\*\\tool.exe";
    auto old = target_version(L"C:\\Tools\\Version 99.0\\tool.exe", pattern);
    old.has_version_resource = true;
    old.version = {5, 9, 0, 0};
    auto newest = target_version(L"C:\\Tools\\Version 1.0\\tool.exe", pattern);
    newest.has_version_resource = true;
    newest.version = {5, 10, 0, 0};
    newest.file_version_numbers = {5, 10, 0, 1};
    auto rebuilt = target_version(L"C:\\Tools\\Version 0.1\\tool.exe", pattern);
    rebuilt.has_version_resource = true;
    rebuilt.version = newest.version;
    rebuilt.file_version_numbers = {5, 10, 0, 2};
    std::vector versions{old, newest, rebuilt};
    launcher::sort_target_versions(versions);
    REQUIRE(versions.size() == 3);
    CHECK(versions[0].executable == rebuilt.executable);
    CHECK(versions[1].executable == newest.executable);
    CHECK(versions[2].executable == old.executable);
}

TEST_CASE("Target version ordering prefers metadata and naturally breaks equal metadata ties", "[target-versions]") {
    const fs::path pattern = L"C:\\Tools\\Version *\\tool.exe";
    auto older_label = target_version(L"C:\\Tools\\Version 5.9\\tool.exe", pattern);
    older_label.has_version_resource = true;
    older_label.version = {1, 0, 0, 0};
    older_label.file_version_numbers = {1, 0, 0, 0};
    older_label.product_version = L"1.0";
    auto newest_label = older_label;
    newest_label.executable = L"C:\\Tools\\Version 5.10\\tool.exe";
    newest_label.path_label = L"Version 5.10";
    const auto without_metadata = target_version(L"C:\\Tools\\Version 99.0\\tool.exe", pattern);
    std::vector versions{without_metadata, older_label, newest_label};
    launcher::sort_target_versions(versions);
    REQUIRE(versions.size() == 3);
    CHECK(versions[0].executable == newest_label.executable);
    CHECK(versions[1].executable == older_label.executable);
    CHECK(versions[2].executable == without_metadata.executable);
}
