#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/colors.hpp"
#include "launcher/config.hpp"
#include "launcher/resource_module.hpp"

#include <catch2/catch_test_macros.hpp>

#include <barrier>
#include <chrono>
#include <filesystem>
#include <future>
#include <initializer_list>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

struct PaletteFixture {
    std::filesystem::path directory = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()) /
        ("LauncherColorsTests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    PaletteFixture() {
        REQUIRE(std::filesystem::create_directory(directory));
        launcher::color_library.clear();
    }
    ~PaletteFixture() {
        launcher::color_library.clear();
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(directory, error);
        if (!error && resolved == directory && resolved.parent_path() == std::filesystem::weakly_canonical(std::filesystem::temp_directory_path())) {
            std::filesystem::remove_all(resolved, error);
        }
    }
    void load(std::string_view name) const {
        const auto filename = name == "basic_colors" || name == "extra_colors"
            ? std::string(name) + ".toml" : "colors-" + std::string(name) + ".toml";
        const auto loaded = launcher::load_colors(filename, directory, directory);
        REQUIRE(loaded.ends_with("!resource:" + filename));
        REQUIRE(std::filesystem::path(loaded.substr(0, loaded.find("!resource:"))).is_absolute());
    }
    void defaults() const { load("basic_colors"); load("extra_colors"); }
};

void require_same_library(const launcher::ColorLibrary& expected) {
    REQUIRE(launcher::color_library.size() == expected.size());
    for (const auto& [name, scheme] : expected) {
        INFO(name);
        const auto& actual = launcher::color_library.at(name);
        REQUIRE(actual.tab.gradient == scheme.tab.gradient);
        REQUIRE(actual.tab.hover == scheme.tab.hover);
        REQUIRE(actual.page.gradient == scheme.page.gradient);
        REQUIRE(actual.page.hover == scheme.page.hover);
        REQUIRE(actual.outline == scheme.outline);
        REQUIRE(actual.text == scheme.text);
    }
}

constexpr std::string_view complete_orange = R"toml(
[colors.ORANGE]
tab.gradient = [{at=0, color="#FFF8BB"}, {at=1, color="#FFDD88"}]
page.gradient = [{at=0, color="#FFDD88"}, {at=1, color="#FFD66B"}]
outline = "#112233"
)toml";

} // namespace

TEST_CASE("Embedded basic palettes preserve the existing named gradients", "[colors][bundled]") {
    PaletteFixture fixture;
    fixture.load("basic_colors");
    REQUIRE(launcher::color_library.size() == 12);
    for (const auto& [name, scheme] : launcher::color_library) {
        INFO(name);
        REQUIRE(scheme.tab.gradient.size() >= 2);
        REQUIRE(scheme.page.gradient.size() >= 2);
        REQUIRE(scheme.tab.gradient.front().at == 0.0F);
        REQUIRE(scheme.tab.gradient.back().at == 1.0F);
        REQUIRE(scheme.page.gradient.front().at == 0.0F);
        REQUIRE(scheme.page.gradient.back().at == 1.0F);
        REQUIRE(scheme.tab.gradient.back().rgb == scheme.page.gradient.front().rgb);
        REQUIRE(scheme.page.gradient.front().rgb != scheme.page.gradient.back().rgb);
        REQUIRE(scheme.outline != scheme.page.gradient.front().rgb);
    }
    REQUIRE(launcher::color_library.at("orange").page.gradient.front().rgb == 0xef9843);
    REQUIRE(launcher::color_library.at("orange").page.gradient.back().rgb == 0xffd078);
    const auto basic = launcher::color_library;
    fixture.load("extra_colors");
    REQUIRE(launcher::color_library.size() == basic.size() + 9);
    REQUIRE(launcher::color_library.contains("turquoise"));
    REQUIRE(launcher::color_library.contains("copper"));
    REQUIRE(launcher::color_library.at("orange").page.gradient == basic.at("orange").page.gradient);
    for (const auto* selection : {"light", "dark"}) {
        INFO(selection);
        fixture.load(selection);
        for (const auto& [name, scheme] : basic) {
            INFO(name);
            REQUIRE(launcher::color_library.contains(name));
        }
        REQUIRE(launcher::color_library.contains("turquoise"));
    }
}

TEST_CASE("Ad hoc colors are cached under normalized hexadecimal names", "[colors]") {
    PaletteFixture fixture;
    fixture.defaults();
    const auto count = launcher::color_library.size();
    const auto name = launcher::normalize_color_name("#A1B2c3");
    REQUIRE(name == "#a1b2c3");
    const auto& first = launcher::find_color(name);
    REQUIRE(first.page.gradient.front().rgb == 0xa1b2c3);
    REQUIRE(first.tab.gradient.back().rgb == 0xa1b2c3);
    REQUIRE(first.page.gradient.back().rgb != 0xa1b2c3);
    REQUIRE(&launcher::find_color(name) == &first);
    REQUIRE(&launcher::color_library.at(name) == &first);
    REQUIRE(launcher::color_library.size() == count + 1);
    for (const auto invalid : {"#12345", "#1234567", "#12345z", "#123456tail"}) {
        INFO(invalid);
        REQUIRE_THROWS_AS(launcher::find_color(invalid), launcher::ConfigError);
    }
    REQUIRE(launcher::color_library.size() == count + 1);
    REQUIRE_THROWS_AS(launcher::adhoc_color(0x1000000), launcher::ConfigError);
}

TEST_CASE("Generated page colors change gradually without a highlight band", "[colors]") {
    PaletteFixture fixture;
    fixture.load("basic_colors");
    for (const auto rgb : {0xf3c982U, 0x000000U, 0xffffffU, 0xff0000U, 0x0000ffU, 0x808080U}) {
        launcher::color_library.emplace(std::to_string(rgb), launcher::adhoc_color(rgb));
    }
    for (const auto& [name, scheme] : launcher::color_library) {
        INFO(name);
        const auto& gradient = scheme.page.gradient;
        for (std::size_t index = 1; index < gradient.size(); ++index) {
            const auto& previous = gradient[index - 1];
            const auto& current = gradient[index];
            for (unsigned shift : {16U, 8U, 0U}) {
                const auto before = static_cast<int>((previous.rgb >> shift) & 0xffU);
                const auto after = static_cast<int>((current.rgb >> shift) & 0xffU);
                const auto change = before < after ? after - before : before - after;
                REQUIRE(change <= 128.0F * (current.at - previous.at) + 1.0F);
            }
        }
    }
}

TEST_CASE("Full color definitions replace entries without retaining optional fields", "[colors]") {
    PaletteFixture fixture;
    fixture.defaults();
    launcher::parse_colors("[colors.orange]\npartial=true\ntext='#F2F2F2'\ntab.hover='#000000'\n");
    REQUIRE(launcher::color_library.at("orange").text == 0xf2f2f2);
    launcher::parse_colors(complete_orange);
    const auto& orange = launcher::color_library.at("orange");
    REQUIRE(orange.text == 0x131313);
    REQUIRE(orange.outline == 0x112233);
    REQUIRE(orange.tab.gradient.front().rgb == 0xfff8bb);
    REQUIRE(orange.tab.hover != 0);
    REQUIRE(orange.page.gradient.back().rgb == 0xffd66b);
    launcher::parse_colors("[colors.orange]\npartial=true\noutline='#223344'\n");
    REQUIRE(launcher::color_library.at("orange").outline == 0x223344);
    launcher::parse_colors(std::string(complete_orange) + "partial=false\n");
    REQUIRE(launcher::color_library.at("orange").outline == 0x112233);
    const auto before = launcher::color_library;
    REQUIRE_THROWS_AS(launcher::parse_colors("[colors.orange]\noutline='#000000'\n"), launcher::ConfigError);
    require_same_library(before);
}

TEST_CASE("Explicit partial definitions preserve unspecified fields and replace gradient arrays", "[colors][hover]") {
    PaletteFixture fixture;
    fixture.defaults();
    const auto original = launcher::color_library.at("orange");
    launcher::parse_colors(R"toml(
[colors.ORANGE]
partial = true
outline = "#84520A"
text = "#F2F2F2"
tab.hover = "#000000"
page.hover = "#123456"
[colors.Meadow]
tab = [{at=0, color="#EAFCC0"}, {at=0.4, color="#C6DF80"}, {at=1, color="#ABC864"}]
page = [{at=0, color="#ABC864"}, {at=1, color="#DCF0B0"}]
outline = "#71843D"
)toml");
    REQUIRE(launcher::color_library.at("orange").outline == 0x84520a);
    REQUIRE(launcher::color_library.at("orange").text == 0xf2f2f2);
    REQUIRE(launcher::color_library.at("orange").tab.gradient == original.tab.gradient);
    REQUIRE(launcher::color_library.at("orange").page.gradient == original.page.gradient);
    REQUIRE(launcher::color_library.at("orange").tab.hover == 0);
    REQUIRE(launcher::color_library.at("orange").page.hover == 0x123456);
    REQUIRE(launcher::find_color("meadow").tab.gradient.size() == 3);
    REQUIRE(launcher::find_color("meadow").text == 0x131313);
    launcher::parse_colors("[colors.orange]\npartial=true\ntab.gradient=[{at=0,color='#FFFFCC'},{at=1,color='#EF9843'}]\n");
    REQUIRE(launcher::color_library.at("orange").tab.gradient.size() == 2);
    REQUIRE(launcher::color_library.at("orange").tab.hover == 0);
    REQUIRE(launcher::color_library.at("orange").page.hover == 0x123456);
    const auto before = launcher::color_library;
    launcher::parse_colors("");
    require_same_library(before);
}

TEST_CASE("Explicit gradients retain authored solid fills and sharp stops", "[colors]") {
    PaletteFixture fixture;
    fixture.load("basic_colors");
    launcher::parse_colors(R"toml(
[colors.solid]
tab.gradient = [{at=0, color="#F3C982"}, {at=1, color="#F3C982"}]
page.gradient = [{at=0, color="#F3C982"}, {at=1, color="#F3C982"}]
outline = "#80502B"
[colors.orange]
partial = true
page.gradient = [{at=0, color="#EF9843"}, {at=0.03, color="#FFF8EE"}, {at=1, color="#FFD078"}]
)toml");
    const std::vector<launcher::ColorStop> solid{{0.0F, 0xf3c982}, {1.0F, 0xf3c982}};
    REQUIRE(launcher::color_library.at("solid").tab.gradient == solid);
    REQUIRE(launcher::color_library.at("solid").page.gradient == solid);
    const std::vector<launcher::ColorStop> highlight{{0.0F, 0xef9843}, {0.03F, 0xfff8ee}, {1.0F, 0xffd078}};
    REQUIRE(launcher::color_library.at("orange").page.gradient == highlight);
    REQUIRE(launcher::color_library.at("orange").tab.gradient.back().rgb == 0xef9843);
}

TEST_CASE("Invalid palette files publish no definitions", "[colors]") {
    PaletteFixture fixture;
    fixture.defaults();
    const auto before = launcher::color_library;
    for (const auto definition : {
            "outline='#112233'", "partial='true'", "partial=1", "partial=true\nunknown=1",
            "partial=true\ntab.hover=4", "partial=true\npage.hover='#12345Z'",
            "partial=true\ntab.unknown='#112233'", "partial=true\npage.gradient=4",
            "partial=true\noutline='tablast'", "partial=true\ntext=4", "partial=true\ntext='#FFFFF'",
            "partial=true\ntab=[{at=0,color='#112233'}]",
            "partial=true\ntab=[{at=0.1,color='#112233'},{at=1,color='#EF9843'}]",
            "partial=true\ntab=[{at=0,color='#112233'},{at=0,color='#112244'},{at=1,color='#EF9843'}]",
            "partial=true\ntab=[{at=0,color='#112233'},{at=inf,color='#EF9843'}]",
            "partial=true\ntab=[{at=0,color='#112233'},{at=nan,color='#112244'},{at=1,color='#EF9843'}]",
            "partial=true\ntab=[{at=0,color='#112233'},{at=1.1,color='#EF9843'}]",
            "partial=true\ntab=[{at=0,color='#112233'},{at=0.99999999999,color='#EF9843'}]",
            "partial=true\noutline='#ZZ2233'"}) {
        INFO(definition);
        const auto source = "[colors.aaa]\ntab=[{at=0,color='#112233'},{at=1,color='#445566'}]\n"
            "page=[{at=0,color='#445566'},{at=1,color='#778899'}]\noutline='#000000'\n"
            "[colors.orange]\n" + std::string(definition);
        REQUIRE_THROWS_AS(launcher::parse_colors(source, "test-invalid.toml"), launcher::ConfigError);
        require_same_library(before);
    }
    for (const auto source : {"colors=4", "[colors", "[colors.missing]\npartial=true\noutline='#112233'",
            "[colors.orange]\npartial=true\noutline='#112233'\n[colors.ORANGE]\npartial=true\noutline='#223344'"}) {
        INFO(source);
        REQUIRE_THROWS_AS(launcher::parse_colors(source), launcher::ConfigError);
        require_same_library(before);
    }
}

TEST_CASE("Color names normalize ASCII at ingestion and reject non-ASCII or controls", "[colors][config]") {
    PaletteFixture fixture;
    REQUIRE(launcher::normalize_color_name("OrAnGe") == "orange");
    REQUIRE(launcher::normalize_color_name("#A1B2c3") == "#a1b2c3");
    for (const std::string invalid : {std::string{}, std::string("a\0b", 3), std::string("a\nb"),
            std::string("a\x7f"), std::string("caf\xc3\xa9"), std::string(1, static_cast<char>(0xff))}) {
        REQUIRE_THROWS_AS(launcher::normalize_color_name(invalid), launcher::ConfigError);
    }
    for (const auto name : {"caf\xc3\xa9", "\xe8\x93\x9d"}) {
        const auto definition = "[colors.\"" + std::string(name) + "\"]\n"
            "tab=[{at=0,color='#112233'},{at=1,color='#445566'}]\n"
            "page=[{at=0,color='#445566'},{at=1,color='#778899'}]\noutline='#000000'\n";
        REQUIRE_THROWS_AS(launcher::parse_colors(definition), launcher::ConfigError);
        REQUIRE(launcher::color_library.empty());
        REQUIRE_THROWS_AS(launcher::parse_configuration("[[tabs]]\nname='Bad'\ncolor='" + std::string(name) + "'\n"), launcher::ConfigError);
        launcher::color_library.clear();
    }
    const auto config = launcher::parse_configuration("[[tabs]]\nname='Named'\ncolor='SaLmOn'\n[[tabs]]\nname='Ad hoc'\ncolor='#a1B2c3'\n");
    REQUIRE(config.tabs[0].colors.outline == launcher::color_library.at("salmon").outline);
    REQUIRE(config.tabs[1].color_rgb == 0xa1b2c3);
    REQUIRE(launcher::color_library.contains("#a1b2c3"));
}

TEST_CASE("Unknown color reports list every currently defined name in map order", "[colors]") {
    PaletteFixture fixture;
    fixture.defaults();
    const auto count = launcher::color_library.size();
    REQUIRE_THROWS_AS(launcher::find_color("not-defined"), launcher::UnknownColorError);
    REQUIRE(launcher::color_library.size() == count);
    const auto report = launcher::color_library_report("not-defined");
    REQUIRE(report.find("not-defined") != std::string::npos);
    std::size_t next = 0;
    for (const auto& [name, scheme] : launcher::color_library) {
        const auto position = report.find(name, next);
        INFO(name);
        REQUIRE(position != std::string::npos);
        next = position + name.size();
    }
}

TEST_CASE("Unknown color reports point to readable examples only when embedded definitions were loaded", "[colors][bundled]") {
    PaletteFixture fixture;
    std::vector<std::string> sources;
    bool embedded = false;
    SECTION("embedded basic and extra palettes") {
        for (const auto filename : {"basic_colors.toml", "extra_colors.toml"}) {
            sources.push_back(launcher::load_colors(filename, fixture.directory, fixture.directory));
        }
        embedded = true;
    }
    SECTION("file palettes only") {
        sources.push_back((fixture.directory / "colors-custom.toml").string());
        launcher::parse_colors(complete_orange, sources.back());
    }
    SECTION("no source information") {
        launcher::parse_colors(complete_orange);
    }
    const auto report = launcher::color_library_report("not-defined", sources);
    const auto note = report.find("examples directory beside the running executable");
    REQUIRE((note != std::string::npos) == embedded);
    if (embedded) {
        REQUIRE(report.find("examples directory beside the running executable", note + 1) == std::string::npos);
    }
}

TEST_CASE("Each configuration load starts a fresh library and applies selected sets in order", "[colors][config]") {
    PaletteFixture fixture;
    const auto first = launcher::parse_configuration("colors=['light','dark']\n[[tabs]]\nname='Dark'\ncolor='orange'\n");
    const auto dark = first.tabs.front().colors;
    launcher::find_color("#123456");
    const auto plain = launcher::parse_configuration("colors=[]\n[[tabs]]\nname='Default'\ncolor='orange'\n");
    REQUIRE(plain.color_sets.empty());
    REQUIRE(plain.tabs.front().colors.page.gradient.front().rgb == 0xef9843);
    REQUIRE(plain.tabs.front().colors.page.gradient != dark.page.gradient);
    REQUIRE_FALSE(launcher::color_library.contains("#123456"));
    REQUIRE(launcher::color_library.contains("copper"));
    const auto second = launcher::parse_configuration("colors=['dark','light']\n[[tabs]]\nname='Light'\ncolor='orange'\n");
    REQUIRE(second.color_sets == std::vector<std::string>{"dark", "light"});
    REQUIRE(second.tabs.front().colors.page.gradient.front().rgb == 0xffe0b7);
    REQUIRE_THROWS_AS(launcher::parse_configuration("colors='absent'\n[[tabs]]\nname='Bad'\n"), launcher::ConfigError);
}

TEST_CASE("Concurrent configuration loads retain their own selected palettes", "[colors][config][concurrency]") {
    PaletteFixture fixture;
    const std::string light_source = "colors='light'\n[[tabs]]\nname='Named'\ncolor='orange'\n"
        "[[tabs]]\nname='Extra'\ncolor='copper'\n[[tabs]]\nname='Ad hoc'\ncolor='#123456'\n";
    const std::string dark_source = "colors='dark'\n[[tabs]]\nname='Named'\ncolor='orange'\n"
        "[[tabs]]\nname='Extra'\ncolor='copper'\n[[tabs]]\nname='Ad hoc'\ncolor='#ABCDEF'\n";
    const auto light = launcher::parse_configuration(light_source);
    const auto dark = launcher::parse_configuration(dark_source);
    REQUIRE(light.tabs.front().colors.outline != dark.tabs.front().colors.outline);

    constexpr std::size_t worker_count = 6;
    std::barrier start(worker_count);
    std::vector<std::future<std::vector<launcher::Configuration>>> workers;
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.push_back(std::async(std::launch::async, [&, use_dark = worker % 2 != 0] {
            start.arrive_and_wait();
            std::vector<launcher::Configuration> configurations;
            for (unsigned iteration = 0; iteration < 3; ++iteration) {
                configurations.push_back(launcher::parse_configuration(use_dark ? dark_source : light_source));
            }
            return configurations;
        }));
    }
    // Inspect only copied results on the test thread, after all global-library
    // rebuilding by a worker has finished. Worker exceptions propagate here.
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        const auto configurations = workers[worker].get();
        const auto& expected = worker % 2 != 0 ? dark : light;
        REQUIRE(configurations.size() == 3);
        for (const auto& actual : configurations) {
            REQUIRE(actual.color_sets == expected.color_sets);
            REQUIRE(actual.tabs.size() == expected.tabs.size());
            for (std::size_t tab = 0; tab < actual.tabs.size(); ++tab) {
                const auto& wanted = expected.tabs[tab].colors;
                const auto& received = actual.tabs[tab].colors;
                REQUIRE(actual.tabs[tab].color_rgb == expected.tabs[tab].color_rgb);
                REQUIRE(received.tab.gradient == wanted.tab.gradient);
                REQUIRE(received.tab.hover == wanted.tab.hover);
                REQUIRE(received.page.gradient == wanted.page.gradient);
                REQUIRE(received.page.hover == wanted.page.hover);
                REQUIRE(received.outline == wanted.outline);
                REQUIRE(received.text == wanted.text);
            }
        }
    }
}

TEST_CASE("Current executable resource text remains valid after its module wrapper closes", "[colors][resource-module][bundled]") {
    PaletteFixture fixture;
    std::string_view text;
    {
        launcher::ResourceModule module{nullptr};
        text = module.FetchText(std::string("LAUNCHER_TOML"), std::string("basic_colors.toml"));
        REQUIRE_FALSE(text.empty());
        REQUIRE_THROWS_AS(module.FetchText(std::string("LAUNCHER_TOML"), std::string("missing-palette.toml")), std::system_error);
    }
    launcher::parse_colors(text, "resource:basic_colors.toml");
    REQUIRE(launcher::color_library.size() == 12);
    REQUIRE(launcher::color_library.at("orange").page.gradient.front().rgb == 0xef9843);
    REQUIRE(launcher::color_library.at("orange").page.gradient.back().rgb == 0xffd078);
}
