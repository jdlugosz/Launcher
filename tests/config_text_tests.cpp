#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/config_text.hpp"
#include "launcher/config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

class TemporaryTomlFile {
public:
    TemporaryTomlFile() {
        static std::atomic<unsigned> sequence = 0;
        const auto parent = std::filesystem::temp_directory_path();
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            const auto directory = parent / ("LauncherConfigText-" + std::to_string(timestamp) + "-" + std::to_string(sequence++));
            std::error_code error;
            if (std::filesystem::create_directory(directory, error)) {
                path_ = directory / "document.toml";
                return;
            }
            if (error && error != std::errc::file_exists) { throw std::runtime_error(error.message()); }
        }
        throw std::runtime_error("Cannot allocate a temporary TOML directory");
    }
    TemporaryTomlFile(const TemporaryTomlFile&) = delete;
    TemporaryTomlFile& operator=(const TemporaryTomlFile&) = delete;
    ~TemporaryTomlFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.parent_path(), ignored);
    }
    const std::filesystem::path& path() const { return path_; }
    void write(std::string_view source) const {
        std::ofstream file(path_, std::ios::binary);
        file.write(source.data(), static_cast<std::streamsize>(source.size()));
        file.close();
        REQUIRE(file.good());
    }
    std::string read() const {
        std::ifstream file(path_, std::ios::binary);
        REQUIRE(file.good());
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("Config text builder owns its source and preserves the retained bytes", "[config-text][stop]") {
    const std::string retained = "\xEF\xBB\xBF" "answer=42\r\n# Keep this formatting\r\n";
    std::string source = retained + "%STOP notes\r\nUnparsed examples [";
    auto builder = launcher::config_text_builder::from_text(source);
    source.clear();
    SECTION("Build retains the exact bytes") {
        REQUIRE(builder.build() == retained);
        REQUIRE(launcher::config_text_builder::from_text("").build().empty());
    }
    SECTION("Parse reads the retained TOML") {
        REQUIRE(builder.parse()["answer"].value<int>() == 42);
    }
}

TEST_CASE("Config text tables have no Launcher schema", "[config-text]") {
    const auto document = launcher::config_text_builder::from_text("answer=42\n[arbitrary]\nvalue='text'").parse();
    REQUIRE(document["answer"].value<int>() == 42);
    REQUIRE(document["arbitrary"]["value"].value<std::string>() == "text");
}

TEST_CASE("Config text builder loads a file without modifying it", "[config-text][storage][stop]") {
    TemporaryTomlFile file;
    const std::string retained = "answer=42\n";
    const auto source = retained + "%STOP\nUnparsed examples [";
    file.write(source);
    launcher::config_text_builder builder(file.path());
    SECTION("Build retains the exact bytes") {
        REQUIRE(builder.build() == retained);
    }
    SECTION("Parse reads the retained TOML") {
        REQUIRE(builder.parse()["answer"].value<int>() == 42);
    }
    REQUIRE(file.read() == source);
}

TEST_CASE("Config text parser reports its filename and original line number", "[config-text][storage]") {
    TemporaryTomlFile file;
    const std::string source = "answer=42\n[unfinished\n%STOP\nIgnored examples";
    file.write(source);
    try {
        (void)launcher::config_text_builder(file.path()).parse();
        FAIL("Malformed TOML must be rejected");
    } catch (const launcher::ConfigError& error) {
        const std::string message = error.what();
        REQUIRE(message.find("document.toml") != message.npos);
        REQUIRE(message.find("line 2") != message.npos);
    }
    REQUIRE(file.read() == source);
}

TEST_CASE("Config text builder rejects a missing input file", "[config-text][storage]") {
    TemporaryTomlFile file;
    REQUIRE_THROWS_AS(launcher::config_text_builder(file.path()), launcher::ConfigError);
}
