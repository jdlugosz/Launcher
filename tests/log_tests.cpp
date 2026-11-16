#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/log.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

class LogFixture final {
public:
    LogFixture() : parent_(fs::weakly_canonical(fs::temp_directory_path())) {
        static std::atomic<unsigned> sequence = 0;
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            root_ = parent_ / ("LauncherLogTests-" + std::to_string(GetCurrentProcessId()) +
                "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence++));
            std::error_code error;
            if (fs::create_directory(root_, error)) { return; }
            if (error && error != std::errc::file_exists) {
                throw std::runtime_error("Cannot create log test directory");
            }
        }
        throw std::runtime_error("Cannot allocate log test directory");
    }
    LogFixture(const LogFixture&) = delete;
    LogFixture& operator=(const LogFixture&) = delete;
    ~LogFixture() {
        std::error_code error;
        const auto resolved = fs::weakly_canonical(root_, error);
        if (!error && resolved == root_ && resolved.parent_path() == parent_) {
            fs::remove_all(resolved, error);
        }
    }
    fs::path path(std::string_view name) const { return root_ / name; }
    const fs::path& directory() const { return root_; }

private:
    fs::path parent_;
    fs::path root_;
};

void write_file(const fs::path& path, std::string_view content, std::ios::openmode mode = std::ios::trunc) {
    std::ofstream file(path, std::ios::binary | mode);
    REQUIRE(file.good());
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    file.close();
    REQUIRE(file.good());
}

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::vector<std::string> record_bodies(std::string_view content) {
    const auto process = " pid=" + std::to_string(GetCurrentProcessId()) + " ";
    std::vector<std::string> bodies;
    while (!content.empty()) {
        const auto newline = content.find('\n');
        REQUIRE(newline != content.npos);
        const auto line = content.substr(0, newline);
        REQUIRE(line.size() >= 24 + process.size());
        CHECK(line[4] == '-');
        CHECK(line[7] == '-');
        CHECK(line[10] == 'T');
        CHECK(line[13] == ':');
        CHECK(line[16] == ':');
        CHECK(line[19] == '.');
        CHECK(line[23] == 'Z');
        for (const auto digit : std::array{0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U,
                 11U, 12U, 14U, 15U, 17U, 18U, 20U, 21U, 22U}) {
            CHECK((line[digit] >= '0' && line[digit] <= '9'));
        }
        REQUIRE(line.substr(24, process.size()) == process);
        bodies.emplace_back(line.substr(24 + process.size()));
        content.remove_prefix(newline + 1);
    }
    return bodies;
}

} // namespace

TEST_CASE("Deferred logging owns complete fragmented records and follows the configured destination", "[log]") {
    LogFixture fixture;
    const auto fallback = fixture.path("default.log");
    const auto destination = fixture.path("diagnostics/chosen.trace.log");
    {
        launcher::Log log(fallback);
        CHECK(log.filesystem_error_path() == fixture.path("default.filesystem-errors.log"));
        launcher::LoggingOptions options;
        options.file = fs::path("diagnostics/chosen.trace.log");
        log.configure(options, fixture.directory());
        CHECK(log.filesystem_error_path() == fixture.path("diagnostics/chosen.trace.filesystem-errors.log"));
        {
            std::string part = "original";
            const std::string surrounded = "[slice]";
            log.write("first ", std::string("temporary "), part, " ",
                std::string_view(surrounded).substr(1, 5), std::string_view{}, "!");
            part.assign("changed");
        }
        log.write("second");
        CHECK_FALSE(fs::exists(destination));
        CHECK_FALSE(fs::exists(fallback));
        CHECK_FALSE(fs::exists(destination.parent_path()));
        log.resume();
        log.resume();
        log.write("third", std::string_view{});
    }
    CHECK_FALSE(fs::exists(fallback));
    const auto bodies = record_bodies(read_file(destination));
    REQUIRE(bodies.size() == 3);
    CHECK(bodies[0] == "first temporary original slice!");
    CHECK(bodies[1] == "second");
    CHECK(bodies[2] == "third");
}

TEST_CASE("Log truncation applies to the assembled body and retains its marker when deferred", "[log]") {
    LogFixture fixture;
    const auto path = fixture.path("bounds.log");
    const std::string prefix(4090, 'a');
    {
        launcher::Log log(path);
        log.write(prefix, "bbbbbb");
        log.write(prefix, "bbbbbbbbbb");
        log.resume();
        log.write(std::string(4100, 'c'), "unreachable tail");
        log.write("", std::string_view{});
    }
    const auto bodies = record_bodies(read_file(path));
    REQUIRE(bodies.size() == 4);
    CHECK(bodies[0] == prefix + "bbbbbb");
    CHECK(bodies[1] == prefix + "bbbbbb [truncated]");
    CHECK(bodies[2] == std::string(4096, 'c') + " [truncated]");
    CHECK(bodies[3].empty());
}

TEST_CASE("Deferred logging keeps the first 256 records in order and continues after resuming", "[log]") {
    LogFixture fixture;
    const auto path = fixture.path("pending.log");
    {
        launcher::Log log(path);
        for (unsigned index = 0; index < 258; ++index) {
            log.write("pending-", std::to_string(index));
        }
        CHECK_FALSE(fs::exists(path));
        log.resume();
        log.write("live");
    }
    const auto bodies = record_bodies(read_file(path));
    REQUIRE(bodies.size() == 257);
    for (unsigned index = 0; index < 256; ++index) {
        CHECK(bodies[index] == "pending-" + std::to_string(index));
    }
    CHECK(bodies.back() == "live");
}

TEST_CASE("HRESULT logging retains the operation and formats uppercase hexadecimal bits", "[log]") {
    LogFixture fixture;
    const auto path = fixture.path("results.log");
    {
        launcher::Log log(path);
        log.result("Failure", static_cast<HRESULT>(0x8ABCDEF1U));
        log.resume();
        log.result("Success", S_OK);
    }
    const auto bodies = record_bodies(read_file(path));
    REQUIRE(bodies.size() == 2);
    CHECK(bodies[0] == "Failure HRESULT=0x8ABCDEF1");
    CHECK(bodies[1] == "Success HRESULT=0x0");
}

TEST_CASE("Log appends to an existing file without replacing its bytes", "[log]") {
    LogFixture fixture;
    const auto path = fixture.path("append.log");
    const std::string existing = "previous session bytes\n";
    write_file(path, existing);
    {
        launcher::Log log(path);
        log.resume();
        log.write("new session");
    }
    const auto content = read_file(path);
    REQUIRE(content.starts_with(existing));
    const auto bodies = record_bodies(std::string_view(content).substr(existing.size()));
    REQUIRE(bodies.size() == 1);
    CHECK(bodies[0] == "new session");
    CHECK_FALSE(fs::exists(fixture.path("append.log.1")));
}

TEST_CASE("Log rotation preserves the complete old file before the next record", "[log]") {
    LogFixture fixture;
    const auto path = fixture.path("rotate.log");
    const auto backup = fixture.path("rotate.log.1");
    constexpr std::size_t limit = 256U * 1024U;

    SECTION("an existing file at the limit rotates before deferred records") {
        const std::string existing(limit, 'x');
        write_file(path, existing);
        write_file(backup, "older backup");
        {
            launcher::Log log(path);
            log.write("first new record");
            log.resume();
        }
        CHECK(read_file(backup) == existing);
        const auto bodies = record_bodies(read_file(path));
        REQUIRE(bodies.size() == 1);
        CHECK(bodies[0] == "first new record");
    }

    SECTION("a record that crosses the limit stays whole in the rotated file") {
        const std::string existing(limit - 1, 'x');
        write_file(path, existing);
        {
            launcher::Log log(path);
            log.resume();
            log.write("crossing record");
            log.write("next record");
        }
        const auto old_content = read_file(backup);
        REQUIRE(old_content.starts_with(existing));
        const auto old_bodies = record_bodies(std::string_view(old_content).substr(existing.size()));
        REQUIRE(old_bodies.size() == 1);
        CHECK(old_bodies[0] == "crossing record");
        const auto bodies = record_bodies(read_file(path));
        REQUIRE(bodies.size() == 1);
        CHECK(bodies[0] == "next record");
    }
}

TEST_CASE("Log rotates data appended by another writer during the current run", "[log]") {
    LogFixture fixture;
    const auto path = fixture.path("live.log");
    const auto backup = fixture.path("live.log.1");
    launcher::Log log(path);
    log.resume();
    log.write("before external append");
    const std::string appended(256U * 1024U, 'x');
    write_file(path, appended, std::ios::app);
    const auto before_rotation = read_file(path);
    REQUIRE(before_rotation.ends_with(appended));

    log.write("after external append");

    CHECK(read_file(backup) == before_rotation);
    const auto bodies = record_bodies(read_file(path));
    REQUIRE(bodies.size() == 1);
    CHECK(bodies.front() == "after external append");
}

TEST_CASE("Log keeps recent archives across rotations and prunes expired archives on startup", "[log]") {
    LogFixture fixture;
    const auto path = fixture.path("retention.log");
    const auto newest = fixture.path("retention.log.1");
    const auto now = fs::file_time_type::clock::now();
    constexpr std::size_t limit = 256U * 1024U;
    const std::string first(limit, 'a');
    const std::string second(limit, 'b');
    const std::string third(limit, 'c');
    const auto rotate = [&](std::string_view contents) {
        write_file(path, contents);
        launcher::Log log(path);
        log.resume();
        CHECK(read_file(newest) == contents);
    };
    rotate(first);
    fs::last_write_time(newest, now - std::chrono::hours(47));
    rotate(second);
    rotate(third);

    fs::path first_archive;
    fs::path second_archive;
    for (const auto& entry : fs::directory_iterator(fixture.directory())) {
        if (entry.path().filename().native().starts_with(L"retention.log.1.")) {
            const auto contents = read_file(entry.path());
            if (contents == first) { first_archive = entry.path(); }
            if (contents == second) { second_archive = entry.path(); }
        }
    }
    REQUIRE_FALSE(first_archive.empty());
    REQUIRE_FALSE(second_archive.empty());

    const auto expired = now - std::chrono::hours(49);
    fs::last_write_time(first_archive, expired);
    fs::last_write_time(newest, expired);
    const auto notes = fixture.path("retention.log.1.notes");
    write_file(notes, "unrelated file");
    fs::last_write_time(notes, expired);
    const auto numbered = fixture.path("retention.log.1.1");
    write_file(numbered, "unrelated numbered file");
    fs::last_write_time(numbered, expired);
    const auto directory = fixture.path("retention.log.1.134000000000000000");
    REQUIRE(fs::create_directory(directory));
    fs::last_write_time(directory, expired);

    launcher::Log restarted(path);
    restarted.resume();

    CHECK_FALSE(fs::exists(first_archive));
    CHECK(read_file(second_archive) == second);
    CHECK(read_file(newest) == third);
    CHECK(read_file(notes) == "unrelated file");
    CHECK(read_file(numbered) == "unrelated numbered file");
    CHECK(fs::is_directory(directory));
}

TEST_CASE("Rotation through a configured symlink keeps the link active", "[log]") {
    LogFixture fixture;
    const auto path = fixture.path("linked.log");
    const auto target = fixture.path("target.log");
    const auto backup = fixture.path("target.log.1");
    const auto legacy = fixture.path("legacy.txt");
    constexpr std::size_t limit = 256U * 1024U;
    const std::string first(limit, 'a');
    const std::string second(limit, 'b');
    write_file(target, first);
    write_file(legacy, "keep this file");
    if (!CreateSymbolicLinkW(path.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        SKIP("File symbolic links are unavailable for this test token");
    }
    REQUIRE(CreateSymbolicLinkW(backup.c_str(), legacy.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE));

    launcher::Log log(path);
    log.resume();
    CHECK(fs::is_symlink(path));
    CHECK(read_file(backup) == first);
    write_file(target, second);
    log.write("after second rotation");
    CHECK(fs::is_symlink(path));
    CHECK(read_file(backup) == second);
    CHECK(read_file(legacy) == "keep this file");
    const auto bodies = record_bodies(read_file(path));
    REQUIRE(bodies.size() == 1);
    CHECK(bodies.front() == "after second rotation");

    REQUIRE(fs::remove(target));
    log.write("reopened after interruption");
    CHECK(fs::is_symlink(path));
    CHECK(read_file(backup) == second);
    const auto reopened = record_bodies(read_file(path));
    REQUIRE(reopened.size() == 1);
    CHECK(reopened.front() == "reopened after interruption");
}

TEST_CASE("Blocked log rotation preserves the active file", "[log]") {
    LogFixture fixture;
    const auto path = fixture.path("blocked.log");
    const auto backup = fixture.path("blocked.log.1");
    REQUIRE(fs::create_directory(backup));
    const auto marker = backup / "keep.txt";
    write_file(marker, "keep this file");
    const std::string original(256U * 1024U, 'x');
    write_file(path, original);
    {
        launcher::Log log(path);
        log.resume();
        log.write("while rotation is blocked");
    }
    CHECK(read_file(marker) == "keep this file");
    const auto preserved = read_file(path);
    REQUIRE(preserved.starts_with(original));

    REQUIRE(fs::remove(marker));
    REQUIRE(fs::remove(backup));
    launcher::Log restarted(path);
    restarted.resume();
    restarted.write("after rotation can run");

    CHECK(read_file(backup) == preserved);
    const auto bodies = record_bodies(read_file(path));
    REQUIRE(bodies.size() == 1);
    CHECK(bodies.front() == "after rotation can run");
}

TEST_CASE("An unusable log destination never turns diagnostics into an exception", "[log]") {
    LogFixture fixture;
    const auto blocker = fixture.path("ordinary-file");
    write_file(blocker, "keep this file");
    {
        launcher::Log log(blocker / "child.log");
        REQUIRE_NOTHROW(log.write("deferred", std::string(" temporary")));
        REQUIRE_NOTHROW(log.resume());
        REQUIRE_NOTHROW(log.write("live"));
        REQUIRE_NOTHROW(log.result("Failure", E_FAIL));
        REQUIRE_NOTHROW(log.resume());
    }
    CHECK(read_file(blocker) == "keep this file");
}

TEST_CASE("Separate log writers append intact records to one file", "[log]") {
    LogFixture fixture;
    const auto path = fixture.path("shared.log");
    launcher::Log first(path);
    launcher::Log second(path);
    first.resume();
    second.resume();
    std::jthread first_writer([&] {
        for (int index = 0; index < 64; ++index) { first.write("first-", std::to_string(index)); }
    });
    std::jthread second_writer([&] {
        for (int index = 0; index < 64; ++index) { second.write("second-", std::to_string(index)); }
    });
    first_writer.join();
    second_writer.join();
    const auto bodies = record_bodies(read_file(path));
    REQUIRE(bodies.size() == 128);
    for (int index = 0; index < 64; ++index) {
        CHECK(std::ranges::count(bodies, "first-" + std::to_string(index)) == 1);
        CHECK(std::ranges::count(bodies, "second-" + std::to_string(index)) == 1);
    }
}

TEST_CASE("Log destinations stay exact when other search roots contain the same filename", "[log]") {
    LogFixture fixture;
    const auto program = fixture.path("portable");
    const auto user = fixture.path("user");
    const auto destination = fixture.path("chosen/launcher.log");
    REQUIRE(fs::create_directory(program));
    REQUIRE(fs::create_directory(user));
    write_file(program / "launcher.log", "portable log stays here");
    write_file(user / "launcher.log", "user log stays here");
    const launcher::FileLookup files(program / "Launcher.exe", user);
    {
        launcher::Log log(destination);
        log.configure({}, destination.parent_path(), files);
        log.write("exact destination");
        CHECK_FALSE(fs::exists(destination));
        log.resume();
    }
    CHECK(read_file(program / "launcher.log") == "portable log stays here");
    CHECK(read_file(user / "launcher.log") == "user log stays here");
    const auto bodies = record_bodies(read_file(destination));
    REQUIRE(bodies.size() == 1);
    CHECK(bodies.front() == "exact destination");
}
