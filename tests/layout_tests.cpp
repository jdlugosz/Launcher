#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/config.hpp"
#include "launcher/hover.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <initializer_list>
#include <string>
#include <utility>

namespace {

std::string icon_path(const launcher::Item& item) {
    const auto path = std::get<launcher::FileIcon>(item.icon_source).path.u8string();
    return {path.begin(), path.end()};
}

launcher::Item icon(std::string label = "Dummy") {
    return {launcher::ItemKind::icon, std::move(label), launcher::StockIcon::folder};
}

launcher::Item spacer() {
    return {launcher::ItemKind::spacer, {}, {}};
}

launcher::Item line_break() {
    return {launcher::ItemKind::line_break, {}, {}};
}

launcher::Configuration configuration(std::initializer_list<launcher::Item> items) {
    launcher::Configuration result;
    result.grid.columns = 3;
    result.tabs.push_back({"General", "Gen", 0xffaabb, items});
    return result;
}

launcher::Configuration parse_single_item(std::string_view fields, std::string_view prefix = {}) {
    return launcher::parse_configuration(std::string(prefix) + "\n[[tabs]]\nname = 'General'\nitems = [{" +
        std::string(fields) + "}]\n");
}

TEST_CASE("Adobe root_path accepts a directory or list with normal path substitutions", "[config][adobe-roots]") {
    const auto single = parse_single_item("kind='adobeCC', target='Photoshop', root_path='$Adobe'", "[paths]\nAdobe='D:\\Creative Apps'");
    REQUIRE(single.tabs[0].items[0].launch->root_paths == std::vector<std::filesystem::path>{L"D:\\Creative Apps"});
    const auto multiple = parse_single_item("kind='adobeCC', target='Photoshop', shift_target='Photoshop (Beta)', root_path=['C:\\Apps\\Adobe', 'D:\\Apps\\Adobe', 'portable']");
    const auto& item = multiple.tabs[0].items[0];
    REQUIRE(item.launch->root_paths.size() == 3);
    CHECK(item.launch->root_paths[1] == L"D:\\Apps\\Adobe");
    REQUIRE(item.shift_launch);
    CHECK(item.shift_launch->root_paths == item.launch->root_paths);
    CHECK(parse_single_item("kind='adobeCC', target='Photoshop', root_path=[]").tabs[0].items[0].launch->root_paths.empty());
    const auto environment = parse_single_item("kind='adobeCC', target='Photoshop', root_path='%WINDIR%'");
    CHECK(environment.tabs[0].items[0].launch->root_paths.front().is_absolute());
}

TEST_CASE("Adobe root_path rejects malformed directories and non Adobe handlers", "[config][adobe-roots]") {
    for (const auto value : {"42", "true", "{}", "['valid', 42]", "''", "['']", "'D:relative'", "'\\rooted'", "'D:\\Apps\\*'", "'D:\\Apps\\?'", "\"bad\\u0000path\""}) {
        CHECK_THROWS_AS(parse_single_item(std::string("kind='adobeCC', target='Photoshop', root_path=") + value), launcher::ConfigError);
    }
    CHECK_THROWS_AS(parse_single_item("target='app.exe', root_path='D:\\Adobe'"), launcher::ConfigError);
    CHECK_THROWS_AS(parse_single_item("root_path='D:\\Adobe'"), launcher::ConfigError);
}

TEST_CASE("Adobe executable targets accept filename patterns and reject paths", "[config][adobe-executable]") {
    const auto config = parse_single_item("kind='adobeCC', target='Dreamweaver*.EXE', root_path=['C:\\Apps', 'D:\\Apps']");
    REQUIRE(config.tabs[0].items[0].launch->target == "Dreamweaver*.EXE");
    CHECK_THROWS_AS(parse_single_item("kind='adobeCC', target='D:\\Apps\\Dreamweaver.exe'"), launcher::ConfigError);
    CHECK_THROWS_AS(parse_single_item("kind='adobeCC', target='subdir/Dreamweaver*.exe'"), launcher::ConfigError);
}

void require_position(const launcher::ItemPlacement& placement,
                      std::size_t item_index,
                      std::size_t column,
                      std::size_t row) {
    REQUIRE(placement.item_index == item_index);
    REQUIRE(placement.column == column);
    REQUIRE(placement.row == row);
}

} // namespace

TEST_CASE("Row-major layout wraps and every tab shares the largest required grid", "[layout]") {
    auto config = configuration({icon("One")});
    config.tabs.push_back({"Documents", "Docs", 0xffffaa,
                           {icon(), icon(), icon(), icon(), icon(), icon(), icon()}});

    const auto layout = launcher::layout_configuration(config);
    REQUIRE(layout.columns == 3);
    REQUIRE(layout.rows == 3);
    REQUIRE(layout.tabs.size() == 2);
    REQUIRE(layout.tabs[0].used_rows == 1);
    require_position(layout.tabs[1].placements[3], 3, 0, 1);
    require_position(layout.tabs[1].placements[6], 6, 0, 2);
}

TEST_CASE("All item kinds retain an optional unsigned flags bitmask", "[config][item-flags]") {
    REQUIRE(parse_single_item("target = 'example.exe'").tabs[0].items[0].flags == 0);
    for (const auto fields : {"target = 'example.exe', flags = 1", "kind = 'adobeCC', target = 'Adobe Photoshop', flags = 1",
            "kind = 'spacer', flags = 1", "kind = 'break', flags = 1"}) {
        REQUIRE(parse_single_item(fields).tabs[0].items[0].flags == launcher::item_flag_log_icon_import);
    }
    REQUIRE(parse_single_item("flags = 5").tabs[0].items[0].flags == 5);
    REQUIRE(parse_single_item("flags = 0xffffffff").tabs[0].items[0].flags == 0xffffffffU);
    for (const auto value : {"-1", "4294967296", "1.0", "true", "'1'"}) {
        REQUIRE_THROWS_AS(parse_single_item(std::string("flags = ") + value), launcher::ConfigError);
    }
}

TEST_CASE("Breaks at run boundaries are no-ops and consecutive breaks do not add empty runs", "[layout]") {
    const auto config = configuration({line_break(), icon(), icon(), icon(),
                                       line_break(), line_break(), icon(), line_break()});
    const auto layout = launcher::layout_configuration(config);
    REQUIRE(layout.rows == 2);
    REQUIRE(layout.tabs[0].placements.size() == 4);
    require_position(layout.tabs[0].placements[0], 1, 0, 0);
    require_position(layout.tabs[0].placements[3], 6, 0, 1);
}

TEST_CASE("A break after a partial run moves the next icon to the next run", "[layout]") {
    const auto config = configuration({icon(), line_break(), icon()});
    const auto layout = launcher::layout_configuration(config);
    REQUIRE(layout.rows == 2);
    require_position(layout.tabs[0].placements[1], 2, 0, 1);
}

TEST_CASE("Trailing breaks never enlarge the occupied grid", "[layout]") {
    SECTION("After a partial row") {
        const auto layout = launcher::layout_configuration(
            configuration({icon(), icon(), line_break(), line_break()}));
        REQUIRE(layout.rows == 1);
    }
    SECTION("After a full row") {
        const auto layout = launcher::layout_configuration(
            configuration({icon(), icon(), icon(), line_break()}));
        REQUIRE(layout.rows == 1);
    }
    SECTION("An empty tab remains usable") {
        const auto layout = launcher::layout_configuration(configuration({line_break()}));
        REQUIRE(layout.columns == 3);
        REQUIRE(layout.rows == 1);
        REQUIRE(layout.tabs[0].placements.empty());
    }
}

TEST_CASE("A spacer consumes a real cell and can cause wrapping", "[layout]") {
    const auto layout = launcher::layout_configuration(
        configuration({icon(), spacer(), icon(), icon()}));
    REQUIRE(layout.rows == 2);
    REQUIRE(layout.tabs[0].placements.size() == 4);
    require_position(layout.tabs[0].placements[1], 1, 1, 0);
    require_position(layout.tabs[0].placements[3], 3, 0, 1);
}

TEST_CASE("Column-major flow transposes wrapping and structural breaks", "[layout]") {
    auto config = configuration({icon(), icon(), line_break(), spacer(), icon(), icon()});
    config.grid.columns = std::nullopt;
    config.grid.rows = 3;
    config.grid.flow = launcher::Flow::column_major;

    const auto layout = launcher::layout_configuration(config);
    REQUIRE(layout.columns == 2);
    REQUIRE(layout.rows == 3);
    require_position(layout.tabs[0].placements[0], 0, 0, 0);
    require_position(layout.tabs[0].placements[1], 1, 0, 1);
    require_position(layout.tabs[0].placements[2], 3, 1, 0);
    require_position(layout.tabs[0].placements[4], 5, 1, 2);
}

TEST_CASE("A fixed grid accepts its final cell and rejects overflow", "[layout]") {
    auto config = configuration({icon(), icon(), icon(), icon(), icon(), icon()});
    config.grid.rows = 2;
    const auto layout = launcher::layout_configuration(config);
    require_position(layout.tabs[0].placements.back(), 5, 2, 1);

    config.tabs[0].items.push_back(spacer());
    REQUIRE_THROWS_AS(launcher::layout_configuration(config), launcher::ConfigError);
}

TEST_CASE("Break-induced overflow is rejected even if the icon count would fit", "[layout]") {
    auto config = configuration({icon(), line_break(), icon()});
    config.grid.rows = 1;
    REQUIRE_THROWS_AS(launcher::layout_configuration(config), launcher::ConfigError);
}

TEST_CASE("An auto run dimension finds the smallest capacity that honors the fixed cross dimension", "[layout]") {
    auto config = configuration({icon(), icon(), line_break(), icon(), icon(), icon()});
    config.grid.columns = std::nullopt;
    config.grid.rows = 2;

    const auto layout = launcher::layout_configuration(config);
    REQUIRE(layout.columns == 3);
    REQUIRE(layout.rows == 2);
    require_position(layout.tabs[0].placements[2], 3, 0, 1);

    config.grid.rows = 1;
    REQUIRE_THROWS_AS(launcher::layout_configuration(config), launcher::ConfigError);
}

TEST_CASE("Column-major auto rows honor the fixed column count", "[layout]") {
    auto config = configuration({icon(), icon(), line_break(), icon(), icon(), icon()});
    config.grid.columns = 2;
    config.grid.rows = std::nullopt;
    config.grid.flow = launcher::Flow::column_major;

    const auto layout = launcher::layout_configuration(config);
    REQUIRE(layout.columns == 2);
    REQUIRE(layout.rows == 3);
    require_position(layout.tabs[0].placements[2], 3, 1, 0);
    require_position(layout.tabs[0].placements.back(), 5, 1, 2);

    config.grid.columns = 1;
    REQUIRE_THROWS_AS(launcher::layout_configuration(config), launcher::ConfigError);
}

TEST_CASE("Both-auto has a deterministic six-column starting shape", "[layout]") {
    auto config = configuration({icon(), icon(), icon(), icon(), icon(), icon(), icon()});
    config.grid.columns = std::nullopt;
    config.grid.rows = std::nullopt;

    const auto row_major = launcher::layout_configuration(config);
    REQUIRE(row_major.columns == 6);
    REQUIRE(row_major.rows == 2);
    require_position(row_major.tabs[0].placements.back(), 6, 0, 1);

    config.grid.flow = launcher::Flow::column_major;
    const auto column_major = launcher::layout_configuration(config);
    REQUIRE(column_major.columns == 4);
    REQUIRE(column_major.rows == 2);
    require_position(column_major.tabs[0].placements.back(), 6, 3, 0);
}

TEST_CASE("A programmatic zero dimension is rejected before layout", "[layout]") {
    auto config = configuration({icon()});
    config.grid.columns = 0;
    REQUIRE_THROWS_AS(launcher::layout_configuration(config), launcher::ConfigError);
}

TEST_CASE("Shared prototype bounds reject excessive programmatic geometry", "[layout]") {
    auto config = configuration({icon()});
    SECTION("Too many columns") {
        config.grid.columns = launcher::max_grid_dimension + 1;
    }
    SECTION("Too many rows") {
        config.grid.rows = launcher::max_grid_dimension + 1;
    }
    SECTION("An excessive icon size") {
        config.grid.icon_size_dip = launcher::max_icon_size_dip + 1.0F;
    }
    SECTION("A zero icon size") {
        config.grid.icon_size_dip = 0.0F;
    }
    SECTION("An excessive pixel gap") {
        config.grid.icon_spacing_px = launcher::max_icon_spacing_px + 1U;
    }
    REQUIRE_THROWS_AS(launcher::layout_configuration(config), launcher::ConfigError);
}

TEST_CASE("Automatic dimensions honor the shared prototype ceiling", "[layout]") {
    auto config = configuration({});
    config.tabs[0].items.assign(launcher::max_grid_dimension + 1, icon());
    SECTION("Automatic rows with fixed columns") {
        config.grid.columns = 1;
        config.grid.rows = std::nullopt;
    }
    SECTION("Automatic columns with fixed rows") {
        config.grid.flow = launcher::Flow::column_major;
        config.grid.columns = std::nullopt;
        config.grid.rows = 1;
    }
    SECTION("Automatic run capacity with a fixed cross dimension") {
        config.grid.columns = std::nullopt;
        config.grid.rows = 1;
    }
    SECTION("Both-auto row-major") {
        config.tabs[0].items.assign(launcher::max_grid_dimension * 6 + 1, icon());
        config.grid.columns = std::nullopt;
        config.grid.rows = std::nullopt;
    }
    SECTION("Both-auto column-major") {
        config.tabs[0].items.assign(launcher::max_grid_dimension * 6 + 1, icon());
        config.grid.flow = launcher::Flow::column_major;
        config.grid.columns = std::nullopt;
        config.grid.rows = std::nullopt;
    }
    REQUIRE_THROWS_AS(launcher::layout_configuration(config), launcher::ConfigError);
}

TEST_CASE("The maximum grid accepts its final cell but rejects another cell", "[layout]") {
    auto config = configuration({});
    config.grid.columns = launcher::max_grid_dimension;
    config.grid.rows = launcher::max_grid_dimension;
    config.grid.icon_size_dip = launcher::max_icon_size_dip;
    config.tabs[0].items.assign(launcher::max_grid_dimension * launcher::max_grid_dimension,
                               icon());
    const auto layout = launcher::layout_configuration(config);
    REQUIRE(layout.columns == launcher::max_grid_dimension);
    REQUIRE(layout.rows == launcher::max_grid_dimension);
    require_position(layout.tabs[0].placements.back(),
                     config.tabs[0].items.size() - 1,
                     launcher::max_grid_dimension - 1,
                     launcher::max_grid_dimension - 1);

    config.tabs[0].items.push_back(spacer());
    REQUIRE_THROWS_AS(launcher::layout_configuration(config), launcher::ConfigError);
}

TEST_CASE("Hover frame padding uses the available physical-pixel gap", "[hover][layout]") {
    REQUIRE(launcher::icon_hover_padding_px(0) == 0.0F);
    REQUIRE(launcher::icon_hover_padding_px(1) == 1.0F);
    REQUIRE(launcher::icon_hover_padding_px(3) == 3.0F);
    REQUIRE(launcher::icon_hover_padding_px(4) == 4.0F);
    REQUIRE(launcher::icon_hover_padding_px(6) == 4.0F);
    REQUIRE(launcher::icon_hover_padding_px(7) == 4.0F);
    REQUIRE(launcher::icon_hover_padding_px(8) == 4.0F);
    REQUIRE(launcher::icon_hover_padding_px(9) == 4.5F);
    REQUIRE(launcher::icon_hover_padding_px(20) == 10.0F);
}

TEST_CASE("TOML parses labels, icon paths, colors and structural items", "[config]") {
    const auto config = launcher::parse_configuration(R"toml(
schema_version = 1
[grid]
columns = 4
rows = "auto"
flow = "row-major"
icon_size_dip = 72
[[tabs]]
name = "Documents"
short_name = "Docs"
color = "#A1b2C3"
items = [
    { label = "A document", icon = 'C:\Tools\document.ico' },
    { kind = "spacer" },
    { kind = "break" },
]
)toml");
    REQUIRE(config.grid.columns == 4);
    REQUIRE_FALSE(config.grid.rows.has_value());
    REQUIRE(config.grid.icon_size_dip == 72.0F);
    REQUIRE(config.tabs[0].name == "Documents");
    REQUIRE(config.tabs[0].short_name == "Docs");
    REQUIRE(config.tabs[0].color_rgb == 0xa1b2c3);
    REQUIRE(icon_path(config.tabs[0].items[0]) == R"(C:\Tools\document.ico)");
    REQUIRE(config.tabs[0].items[0].icon_explicit);
    REQUIRE_FALSE(config.tabs[0].items[0].launch.has_value());
    REQUIRE(config.tabs[0].items[1].kind == launcher::ItemKind::spacer);
    REQUIRE(config.tabs[0].items[2].kind == launcher::ItemKind::line_break);
}

TEST_CASE("Logging filenames are optional UTF-8 paths", "[config][logging]") {
    REQUIRE_FALSE(parse_single_item("target = 'example.exe'").logging.file);
    REQUIRE_FALSE(parse_single_item("target = 'example.exe'", "[logging]").logging.file);
    for (const std::string filename : {"diagnostics/session.log", "C:\\logs\\launcher.log", "資料/launcher.log"}) {
        CAPTURE(filename);
        const auto config = parse_single_item("target = 'example.exe'", "[logging]\nfile = '" + filename + "'");
        REQUIRE(config.logging.file == std::filesystem::path(std::u8string(filename.begin(), filename.end())));
    }
}

TEST_CASE("Logging rejects malformed settings and ambiguous Windows paths", "[config][logging]") {
    for (const std::string source : {"logging = 'file.log'", "[logging]\nfile = 42", "[logging]\nfile = ''",
            "[logging]\nfile = \"bad\\u0000.log\"", "[logging]\nunknown = true",
            "[logging]\nfile = 'C:relative.log'", "[logging]\nfile = '\\rooted.log'"}) {
        CAPTURE(source);
        REQUIRE_THROWS_AS(parse_single_item("target = 'example.exe'", source), launcher::ConfigError);
    }
}

TEST_CASE("STOP directive excludes the example section before TOML parsing", "[config][stop]") {
    const std::string active = "[[tabs]]\nname='Active'\nitems=[{target='notepad.exe'}]\n";
    for (const std::string marker : {"%STOP", "%STOP ===========examples below\n",
            "%STOP\tExamples\r\n", "%STOP\r\n", "%STOP \"\"\" ' [{ ignored\n"}) {
        CAPTURE(marker);
        const auto config = launcher::parse_configuration(active + marker +
            (marker.back() == '\n' ? "Uncommented prose and { incomplete examples" : ""));
        REQUIRE(config.tabs.size() == 1);
        REQUIRE(config.tabs.front().name == "Active");
        REQUIRE(config.tabs.front().items.size() == 1);
    }
    REQUIRE(launcher::parse_configuration("\xEF\xBB\xBF" + active + "%STOP\nInvalid TOML[").tabs.size() == 1);
    REQUIRE_THROWS_AS(launcher::parse_configuration("[[tabs]\n%STOP\n"), launcher::ConfigError);
    REQUIRE_THROWS_AS(launcher::parse_configuration("[[tabs]]\nname=4\n%STOP\n"), launcher::ConfigError);
    for (const auto marker : {"%STOPPING", "%stop", "%STOP=examples", " %STOP", "\t%STOP", "% STOP", "%",
            "%INCLUDE \"novel.txt\""}) {
        REQUIRE_THROWS_AS(launcher::parse_configuration(active + marker + "\n%STOP\n"), launcher::ConfigError);
    }
}

TEST_CASE("STOP text away from column one remains TOML string or comment content", "[config][stop]") {
    for (const std::string quote : {"'", "\"", "'''", "\"\"\""}) {
        CAPTURE(quote);
        const bool multiline = quote.size() == 3;
        const auto content = std::string(multiline ? "\n " : "") + "%STOP ===========examples below";
        const auto config = launcher::parse_configuration("# %STOP ignored; quotes here: \"\"\" '''\n"
            "[[tabs]]\nname = " + quote + content + quote + "\nitems=[{label='STOP'}]\n"
            "%STOP ===========real end\nThis is not TOML");
        REQUIRE(config.tabs.front().name == std::string(multiline ? " " : "") + "%STOP ===========examples below");
        REQUIRE(config.tabs.front().items.front().label == "STOP");
    }
    for (const std::string quote : {"'''", "\"\"\""}) {
        for (const auto extra : {0, 1, 2}) {
            const auto config = launcher::parse_configuration("[paths]\nExample=" + quote +
                "\n %STOP inside the string\n" + quote + std::string(extra, quote.front()) +
                "\n[[tabs]]\nname='Active'\n%STOP\nInvalid TOML[");
            REQUIRE(config.tabs.front().name == "Active");
        }
    }
    const auto escaped = launcher::parse_configuration(R"toml(
[[tabs]]
name = """
An escaped triple quote: \"""
 %STOP is still part of this string
"""
%STOP ===========examples below
Invalid TOML[
)toml");
    REQUIRE(escaped.tabs.front().name == "An escaped triple quote: \"\"\"\n %STOP is still part of this string\n");
}

TEST_CASE("STOP preprocessing ignores TOML string and container context", "[config][stop]") {
    for (const std::string quote : {"'''", "\"\"\""}) {
        CAPTURE(quote);
        // Truncation at the first STOP leaves the string unfinished, so TOML rejects it.
        const auto source = "[[tabs]]\nname=" + quote + "\n%STOP\n" + quote + "\n";
        REQUIRE_THROWS_AS(launcher::parse_configuration(source), launcher::ConfigError);
    }
    REQUIRE_THROWS_AS(launcher::parse_configuration("[[tabs]]\nname='Active'\nitems=[\n%STOP\n]\n"),
        launcher::ConfigError);
}

TEST_CASE("INCLUDE remains unimplemented while ordinary TOML multiline strings work", "[config][stop]") {
    const auto config = launcher::parse_configuration(R"toml(
[[tabs]]
name = """
%INCLUDE "novel.txt"
"""
%STOP
Unparsed examples
)toml");
    REQUIRE(config.tabs.front().name == "%INCLUDE \"novel.txt\"\n");
}

TEST_CASE("Icon-only placeholders and target-derived icons preserve stable defaults", "[config][launch]") {
    const auto placeholder = parse_single_item("label = 'Placeholder'");
    const auto& old_item = placeholder.tabs[0].items[0];
    REQUIRE(std::get<launcher::StockIcon>(old_item.icon_source) == launcher::StockIcon::application);
    REQUIRE_FALSE(old_item.icon_explicit);
    REQUIRE_FALSE(old_item.launch.has_value());
    REQUIRE(old_item.context_verbs.empty());

    const auto executable = parse_single_item(
        R"(label = 'Editor', target = 'C:\Program Files\Editor\editor.exe')");
    const auto& new_item = executable.tabs[0].items[0];
    REQUIRE(std::get<launcher::StockIcon>(new_item.icon_source) == launcher::StockIcon::application);
    REQUIRE_FALSE(new_item.icon_explicit);
    REQUIRE(new_item.launch.has_value());
    REQUIRE(new_item.launch->target == R"(C:\Program Files\Editor\editor.exe)");
    REQUIRE(new_item.launch->handler == "shell");
    REQUIRE(new_item.launch->show == launcher::LaunchShow::normal);
    REQUIRE(new_item.launch->arguments.empty());
    REQUIRE(new_item.launch->working_directory.empty());
}

TEST_CASE("Icon strings and tables become typed sources", "[config][icon-source]") {
    const auto string_file = parse_single_item(R"(icon = 'Icons\资料.svg')");
    const auto table_file = parse_single_item(R"(icon = { file = 'Icons\资料.svg' })");
    REQUIRE(string_file.tabs[0].items[0].icon_source == table_file.tabs[0].items[0].icon_source);
    REQUIRE(icon_path(table_file.tabs[0].items[0]) == R"(Icons\资料.svg)");
    for (const auto name : launcher::stock_icon_names) {
        const auto config = parse_single_item("icon = { stock = '" + std::string(name) + "' }");
        REQUIRE(std::get<launcher::StockIcon>(config.tabs[0].items[0].icon_source) == *launcher::stock_icon_named(name));
        REQUIRE(config.tabs[0].items[0].icon_explicit);
    }
    const auto indexed = parse_single_item(R"(icon = { file = '$W\SHELL32.dll', index = 0 })",
        "[paths]\nW = '%SystemRoot%\\System32'\n");
    const auto& group = std::get<launcher::IconGroup>(indexed.tabs[0].items[0].icon_source);
    REQUIRE(group.path.is_absolute());
    REQUIRE(group.path.filename() == "SHELL32.dll");
    REQUIRE(group.path.parent_path().filename() == "System32");
    REQUIRE(std::get<launcher::IconIndex>(group.selector).value == 0);
    const auto identified = parse_single_item(R"(icon = { file = 'Icons\groups.dll', id = 65535 })");
    REQUIRE(std::get<launcher::IconResourceId>(std::get<launcher::IconGroup>(identified.tabs[0].items[0].icon_source).selector).value == 65535);
    const auto maximum_index = parse_single_item("icon = { file = 'groups.dll', index = 2147483647 }");
    REQUIRE(std::get<launcher::IconIndex>(std::get<launcher::IconGroup>(maximum_index.tabs[0].items[0].icon_source).selector).value == 2147483647);
    // A string is a filename, even if it resembles the former stock notation.
    REQUIRE(icon_path(parse_single_item("icon = 'stock:folder'").tabs[0].items[0]) == "stock:folder");
}

TEST_CASE("PE icon tables distinguish resource selectors from icon groups", "[config][icon-source][pe-resource]") {
    const auto named = parse_single_item(R"(icon = { file = '$Art\library.dll', type = 'SVG', name = 'LOGO', language = 0 })",
        "[paths]\nArt = 'Artwork'\n");
    const auto& resource = std::get<launcher::PeResourceIcon>(named.tabs[0].items[0].icon_source);
    REQUIRE(resource.path == std::filesystem::path(R"(Artwork\library.dll)"));
    REQUIRE(std::get<std::string>(resource.type) == "SVG");
    REQUIRE(std::get<std::string>(resource.name) == "LOGO");
    REQUIRE(resource.language == 0);
    const auto numeric = parse_single_item("icon = { file = 'x.dll', type = 10, id = 96, language = 1033 }");
    const auto& numbers = std::get<launcher::PeResourceIcon>(numeric.tabs[0].items[0].icon_source);
    REQUIRE(std::get<std::uint16_t>(numbers.type) == 10);
    REQUIRE(std::get<std::uint16_t>(numbers.name) == 96);
    REQUIRE(numbers.language == 1033);
    const auto mixed = parse_single_item("icon = { file = 'x.dll', type = 23, name = 'LOGO' }");
    REQUIRE_FALSE(std::get<launcher::PeResourceIcon>(mixed.tabs[0].items[0].icon_source).language);
    const auto named_type = parse_single_item("icon = { file = 'x.dll', type = 'ZPIMGRES', id = 65535 }");
    REQUIRE(std::get<std::uint16_t>(std::get<launcher::PeResourceIcon>(named_type.tabs[0].items[0].icon_source).name) == 65535);
    const auto boundary = parse_single_item("icon = { file = 'x.dll', type = 65535, id = 0, language = 65535 }");
    REQUIRE(std::get<launcher::PeResourceIcon>(boundary.tabs[0].items[0].icon_source).language == 65535);
    const auto unicode = parse_single_item("icon = { file = 'x.dll', type = '图片', name = '图标' }");
    REQUIRE(std::get<std::string>(std::get<launcher::PeResourceIcon>(unicode.tabs[0].items[0].icon_source).name) == "图标");
}

TEST_CASE("PE icon tables reject incomplete conflicting and invalid resource selectors", "[config][icon-source][pe-resource]") {
    for (const auto fields : {"type = 'SVG'", "type = 'SVG', name = 'X', id = 1", "type = 'SVG', name = 'X', index = 0",
            "name = 'X'", "id = 1, language = 0", "type = '', name = 'X'", "type = true, id = 1",
            "type = -1, id = 1", "type = 65536, id = 1", "type = 10.0, id = 1", "type = '#10', id = 1",
            "type = 'SVG', name = ''", "type = 'SVG', name = 1", "type = 'SVG', name = '#1'",
            "type = 10, id = '1'", "type = 10, id = 65536", "type = 10, id = -1", "type = 10, id = 1.0",
            "type = 10, id = 1, language = -1", "type = 10, id = 1, language = 65536",
            "type = 10, id = 1, language = '1033'", "type = 10, id = 1, language = true"}) {
        INFO(fields);
        REQUIRE_THROWS_AS(parse_single_item("icon = { file = 'x.dll', " + std::string(fields) + " }"), launcher::ConfigError);
    }
    REQUIRE_THROWS_AS(parse_single_item(R"(icon = { file = 'x.dll', type = "SV\u0000G", name = 'X' })"), launcher::ConfigError);
}

TEST_CASE("Icon tables reject ambiguous or unsupported selectors", "[config][icon-source]") {
    for (const auto value : {"42", "true", "[]", "{}", "{ stock = 'unknown' }", "{ stock = '' }",
            "{ stock = 1 }", "{ stock = 'folder', file = 'x.dll' }", "{ stock = 'folder', index = 0 }",
            "{ index = 0 }", "{ file = '' }", "{ file = true }", "{ file = 'x.dll', index = 0, id = 3 }",
            "{ file = 'x.dll', index = -1 }", "{ file = 'x.dll', index = 2147483648 }",
            "{ file = 'x.dll', index = 1.0 }", "{ file = 'x.dll', id = -1 }", "{ file = 'x.dll', id = 65536 }",
            "{ file = 'x.dll', id = true }", "{ file = 'x.dll', id = '3' }", "{ file = 'x.dll', type = 'SVG' }",
            "{ file = 'x.dll', typo = 1 }"}) {
        INFO(value);
        REQUIRE_THROWS_WITH(parse_single_item("icon = " + std::string(value)),
            Catch::Matchers::ContainsSubstring("tabs[0].items[0].icon"));
    }
    REQUIRE_THROWS_AS(parse_single_item(R"(icon = { file = "bad\u0000.dll", id = 3 })"), launcher::ConfigError);
    REQUIRE_THROWS_AS(parse_single_item(R"(icon = { file = '$Undefined\x.dll', id = 3 })"), launcher::ConfigError);
}

TEST_CASE("Adobe CC product entries derive labels and remain regular grid icons", "[config][adobe-cc]") {
    const auto config = parse_single_item("kind = 'adobeCC', target = 'Adobe After Effects'");
    const auto& item = config.tabs[0].items[0];
    REQUIRE(item.kind == launcher::ItemKind::icon);
    REQUIRE(item.label == "Adobe After Effects");
    REQUIRE_FALSE(item.label_explicit);
    REQUIRE(std::get<launcher::StockIcon>(item.icon_source) == launcher::StockIcon::application);
    REQUIRE_FALSE(item.icon_explicit);
    REQUIRE(item.launch.has_value());
    REQUIRE(item.launch->handler == "adobeCC");
    REQUIRE(item.launch->target == "Adobe After Effects");
    REQUIRE(item.launch->include_beta);
    REQUIRE(item.launch->arguments.empty());
    REQUIRE(item.launch->working_directory.empty());
    REQUIRE(item.launch->show == launcher::LaunchShow::normal);
    const auto layout = launcher::layout_configuration(config);
    REQUIRE(layout.tabs[0].placements.size() == 1);
    require_position(layout.tabs[0].placements[0], 0, 0, 0);
}

TEST_CASE("Item labels are optional and explicit empty labels remain distinct", "[config][launch][optional-label]") {
    const auto target_only = parse_single_item(R"(target = 'C:\Tools\Editor.exe')");
    const auto& inferred = target_only.tabs[0].items[0];
    REQUIRE(inferred.label.empty());
    REQUIRE_FALSE(inferred.label_explicit);
    REQUIRE_FALSE(inferred.icon_explicit);
    REQUIRE(std::get<launcher::StockIcon>(inferred.icon_source) == launcher::StockIcon::application);
    REQUIRE(inferred.launch->target == R"(C:\Tools\Editor.exe)");
    REQUIRE(inferred.launch->handler == "shell");

    const auto named = parse_single_item(R"(target = 'C:\Tools\Editor.exe', label = '编辑器')");
    REQUIRE(named.tabs[0].items[0].label == "编辑器");
    REQUIRE(named.tabs[0].items[0].label_explicit);

    const auto unnamed = parse_single_item(R"(target = 'C:\Tools\Editor.exe', label = '')");
    REQUIRE(unnamed.tabs[0].items[0].label.empty());
    REQUIRE(unnamed.tabs[0].items[0].label_explicit);
    REQUIRE(unnamed.tabs[0].items[0].launch.has_value());

    const auto custom = parse_single_item(R"(target = 'C:\Tools\Editor.exe', icon = { stock = 'folder' })");
    REQUIRE(custom.tabs[0].items[0].label.empty());
    REQUIRE_FALSE(custom.tabs[0].items[0].label_explicit);
    REQUIRE(custom.tabs[0].items[0].icon_explicit);

    const auto placeholder = parse_single_item("icon = { stock = 'document' }");
    REQUIRE(placeholder.tabs[0].items[0].label.empty());
    REQUIRE_FALSE(placeholder.tabs[0].items[0].launch.has_value());

    const auto adobe = parse_single_item("kind = 'adobeCC', target = 'Adobe Audition', label = ''");
    REQUIRE(adobe.tabs[0].items[0].label.empty());
    REQUIRE(adobe.tabs[0].items[0].label_explicit);
    REQUIRE(adobe.tabs[0].items[0].launch->handler == "adobeCC");

    REQUIRE_THROWS_WITH(parse_single_item("target = 'Editor.exe', icon = ''"),
        Catch::Matchers::ContainsSubstring("'icon' must not be empty"));
}

TEST_CASE("Adobe CC entries preserve launch options and configured context verbs", "[config][adobe-cc]") {
    const auto config = parse_single_item(
        R"(kind = 'adobeCC', target = 'Adobe Audition', label = 'Audio 资料', )"
        R"(handler = 'adobeCC', icon = 'D:\Icons\Audio.ico', arguments = '"D:\资料 Files\sample.wav"', )"
        R"(working_directory = 'D:\项目 Files', show = 'minimized', include_beta = true, )"
        R"(context_verbs = [{label = 'Open project', verb = 'open'}])");
    const auto& item = config.tabs[0].items[0];
    REQUIRE(item.label == "Audio 资料");
    REQUIRE(item.icon_explicit);
    REQUIRE(icon_path(item) == R"(D:\Icons\Audio.ico)");
    REQUIRE(item.launch->handler == "adobeCC");
    REQUIRE(item.launch->include_beta);
    REQUIRE(item.launch->arguments == R"("D:\资料 Files\sample.wav")");
    REQUIRE(item.launch->working_directory == R"(D:\项目 Files)");
    REQUIRE(item.launch->show == launcher::LaunchShow::minimized);
    REQUIRE(item.context_verbs.size() == 1);
    REQUIRE(item.context_verbs[0].label == "Open project");
    REQUIRE(item.context_verbs[0].verb == "open");
}

TEST_CASE("Adobe CC entries reject missing products and conflicting handlers", "[config][adobe-cc]") {
    const std::string_view invalid_items[] = {
        "kind = 'adobeCC'", "kind = 'adobeCC', label = 'After Effects'",
        "kind = 'adobeCC', target = ''", "kind = 'adobeCC', target = 42",
        "kind = 'adobeCC', target = 'Adobe Audition', handler = 'shell'",
        "kind = 'adobeCC', target = 'Adobe Audition', handler = 'future'",
        "kind = 'adobeCC', target = 'Adobe Audition', handler = ''",
        "kind = 'adobeCC', target = 'Adobe Audition', handler = 42",
        R"(kind = 'adobeCC', target = "Adobe\u0000Audition")",
    };
    for (const auto fields : invalid_items) {
        CAPTURE(fields);
        REQUIRE_THROWS_AS(parse_single_item(fields), launcher::ConfigError);
    }
}

TEST_CASE("Beta discovery is a Boolean option limited to Adobe CC handlers", "[config][adobe-cc]") {
    const auto enabled = parse_single_item(
        "label = 'Audio', target = 'Adobe Audition', handler = 'adobeCC', include_beta = true");
    REQUIRE(enabled.tabs[0].items[0].launch->include_beta);
    const auto disabled = parse_single_item(
        "kind = 'adobeCC', target = 'Adobe Audition', include_beta = false");
    REQUIRE_FALSE(disabled.tabs[0].items[0].launch->include_beta);
    const auto ordinary = parse_single_item("label = 'Editor', target = 'editor.exe'");
    REQUIRE_FALSE(ordinary.tabs[0].items[0].launch->include_beta);

    const std::string_view invalid_items[] = {
        "kind = 'adobeCC', target = 'Adobe Audition', include_beta = 'true'",
        "kind = 'adobeCC', target = 'Adobe Audition', include_beta = 1",
        "kind = 'adobeCC', target = 'Adobe Audition', include_beta = []",
        "label = 'Editor', target = 'editor.exe', include_beta = false",
        "label = 'Editor', target = 'editor.exe', handler = 'future', include_beta = true",
        "label = 'Placeholder', include_beta = false",
        "kind = 'spacer', include_beta = false", "kind = 'break', include_beta = true",
    };
    for (const auto fields : invalid_items) {
        CAPTURE(fields);
        REQUIRE_THROWS_AS(parse_single_item(fields), launcher::ConfigError);
    }
}

TEST_CASE("Context verbs preserve Unicode labels and canonical Shell names", "[config][launch][context-verbs]") {
    const auto config = parse_single_item(
        R"(label = 'File', target = '资料.txt', arguments = '--example "quoted"', )"
        R"(working_directory = 'D:\项目', show = 'minimized', context_verbs = [)"
        R"({label = '打印', verb = 'print'}, {label = 'Custom & action', verb = 'vendor.custom-command'}])");
    const auto& item = config.tabs[0].items[0];
    REQUIRE(item.context_verbs.size() == 2);
    REQUIRE(item.context_verbs[0].label == "打印");
    REQUIRE(item.context_verbs[0].verb == "print");
    REQUIRE(item.context_verbs[1].label == "Custom & action");
    REQUIRE(item.context_verbs[1].verb == "vendor.custom-command");
    REQUIRE(item.launch->arguments == R"(--example "quoted")");
    REQUIRE(item.launch->working_directory == R"(D:\项目)");
    REQUIRE(item.launch->show == launcher::LaunchShow::minimized);
}

TEST_CASE("Context verbs require Shell targets and valid nonempty table strings", "[config][launch][context-verbs]") {
    const std::string_view invalid_items[] = {
        "label = 'File', context_verbs = []",
        "label = 'File', target = 'file.txt', handler = 'future', context_verbs = []",
        "kind = 'spacer', context_verbs = []", "kind = 'break', context_verbs = []",
        "label = 'File', target = 'file.txt', context_verbs = 'print'",
        "label = 'File', target = 'file.txt', context_verbs = {}",
        "label = 'File', target = 'file.txt', context_verbs = ['print']",
        "label = 'File', target = 'file.txt', context_verbs = [{verb = 'print'}]",
        "label = 'File', target = 'file.txt', context_verbs = [{label = 'Print'}]",
        "label = 'File', target = 'file.txt', context_verbs = [{label = '', verb = 'print'}]",
        "label = 'File', target = 'file.txt', context_verbs = [{label = 'Print', verb = ''}]",
        "label = 'File', target = 'file.txt', context_verbs = [{label = 42, verb = 'print'}]",
        "label = 'File', target = 'file.txt', context_verbs = [{label = 'Print', verb = false}]",
        "label = 'File', target = 'file.txt', context_verbs = [{label = 'Print', verb = 'print', extra = 1}]",
        R"(label = 'File', target = 'file.txt', context_verbs = [{label = "bad\u0000label", verb = 'print'}])",
        R"(label = 'File', target = 'file.txt', context_verbs = [{label = 'Print', verb = "bad\u0000verb"}])",
    };
    for (const auto fields : invalid_items) {
        CAPTURE(fields);
        REQUIRE_THROWS_AS(parse_single_item(fields), launcher::ConfigError);
    }
}

TEST_CASE("Launch strings retain Unicode paths, literal arguments and future handlers", "[config][launch]") {
    const auto config = parse_single_item(
        R"(label = '资料工具', icon = 'D:\Icons\资料图标.ico', )"
        R"(target = 'C:\Program Files\资料 Tools\viewer.exe', )"
        R"(arguments = '"资料 文件.txt" /mode=read & | > output.txt', )"
        R"(working_directory = 'D:\项目 Files', handler = 'adobe-cc', show = 'hidden')");
    const auto& item = config.tabs[0].items[0];
    REQUIRE(item.icon_explicit);
    REQUIRE(icon_path(item) == R"(D:\Icons\资料图标.ico)");
    REQUIRE(item.launch.has_value());
    REQUIRE(item.launch->target == R"(C:\Program Files\资料 Tools\viewer.exe)");
    REQUIRE(item.launch->arguments == R"("资料 文件.txt" /mode=read & | > output.txt)");
    REQUIRE(item.launch->working_directory == R"(D:\项目 Files)");
    REQUIRE(item.launch->handler == "adobe-cc");
    REQUIRE(item.launch->show == launcher::LaunchShow::hidden);
}

TEST_CASE("Path macros expand target icon and raw arguments before native preparation", "[config][paths][launch]") {
    const auto config = parse_single_item(
        R"(label = '$P literal label', target = '$P\CrystalDiskInfo\DiskInfo64.exe', )"
        R"(icon = '$C/icons/资料.ico', arguments = '$Args\file.txt" /mode=read & | > output.txt', )"
        R"(working_directory = '$C')",
        R"([paths]
P = 'C:\Program Files'
C = 'D:\Project Files\资料'
Args = '--open "D:\Project Files\资料'
)");
    const auto& item = config.tabs[0].items[0];
    REQUIRE(item.label == "$P literal label");
    REQUIRE(item.icon_explicit);
    REQUIRE(icon_path(item) == R"(D:\Project Files\资料/icons/资料.ico)");
    REQUIRE(item.launch->target == R"(C:\Program Files\CrystalDiskInfo\DiskInfo64.exe)");
    REQUIRE(item.launch->arguments == R"(--open "D:\Project Files\资料\file.txt" /mode=read & | > output.txt)");
    REQUIRE(item.launch->working_directory == "$C");
}

TEST_CASE("Leading path identifiers are case sensitive and stop at punctuation", "[config][paths]") {
    const std::string_view paths = R"([paths]
P = 'upper'
p = 'lower'
P_2 = 'numbered'
_Root = 'underscored'
)";
    const std::pair<std::string_view, std::string_view> values[] = {
        {"$P", "upper"}, {R"($P\tool.exe)", R"(upper\tool.exe)"},
        {"$P/tool.exe", "upper/tool.exe"}, {"$P.exe", "upper.exe"},
        {"$P-file", "upper-file"}, {"$p file", "lower file"},
        {"$P_2.ico", "numbered.ico"}, {"$_Root/file", "underscored/file"},
    };
    for (const auto& [source, expected] : values) {
        CAPTURE(source);
        const auto config = parse_single_item("label = 'File', target = '" + std::string(source) + "'", paths);
        REQUIRE(config.tabs[0].items[0].launch->target == expected);
    }
}

TEST_CASE("Quoted argument prefixes preserve authored quotes and raw suffixes", "[config][paths][launch][quoted-prefix]") {
    const std::string_view paths = R"([paths]
Docs = 'C:\Program Files\资料'
Literal = '$Other\cost$$.txt'
Environment = '%WINDIR%\Read Me.md'
Other = 'must not expand'
)";
    const std::pair<std::string_view, std::string_view> values[] = {
        {R"("$Docs\Read Me.md" --flag)", R"("C:\Program Files\资料\Read Me.md" --flag)"},
        {R"("$Literal" --raw %WINDIR% $/PATH$/ $$)", R"("$Other\cost$.txt" --raw %WINDIR% %PATH% $)"},
        {R"("$Environment")", R"("%WINDIR%\Read Me.md")"},
        {R"("$$Docs\literal.md")", R"("$Docs\literal.md")"},
        {R"("literal %WINDIR% $$EXE")", R"("literal %WINDIR% $EXE")"},
        // Arguments remain a raw command line. Expansion does not balance or
        // repair quotes, including an intentionally unfinished quoted prefix.
        {R"("$Docs\Read Me.md)", R"("C:\Program Files\资料\Read Me.md)"},
    };
    for (const auto& [source, expected] : values) {
        CAPTURE(source);
        const auto config = parse_single_item("target='notepad.exe', arguments='" + std::string(source) + "'", paths);
        REQUIRE(config.tabs[0].items[0].launch->arguments == expected);
    }
    REQUIRE_THROWS_WITH(parse_single_item(R"(target='notepad.exe', arguments='"$Missing\Read Me.md"')", paths),
        "tabs[0].items[0]: 'arguments' refers to undefined path 'Missing'");
    for (const auto source : {R"("$9bad")", R"("$")", R"("${Docs}")", R"("$Docs\Read Me.md" "$Other")", R"(--open "$Docs\Read Me.md")"}) {
        CAPTURE(source);
        REQUIRE_THROWS_AS(parse_single_item("target='notepad.exe', arguments='" + std::string(source) + "'", paths), launcher::ConfigError);
    }
    for (const auto field : {"target", "icon"}) {
        CAPTURE(field);
        auto fields = std::string("label='File', ");
        if (std::string_view(field) == "icon") { fields += "target='notepad.exe', "; }
        fields += std::string(field) + R"(='"$Docs\Read Me.md"')";
        REQUIRE_THROWS_AS(parse_single_item(fields, paths), launcher::ConfigError);
    }
}

TEST_CASE("Dollar symbols decode once without recursively expanding inserted path names", "[config][paths]") {
    const auto escaped = parse_single_item(
        R"(label = 'File', target = '$$P\file$$_tail$$$$.exe', icon = 'Icons\cost$$.ico', )"
        R"(arguments = '--price $$5 --macro $$P')");
    const auto& item = escaped.tabs[0].items[0];
    REQUIRE(item.launch->target == R"($P\file$_tail$$.exe)");
    REQUIRE(icon_path(item) == R"(Icons\cost$.ico)");
    REQUIRE(item.launch->arguments == "--price $5 --macro $P");

    const auto literal = parse_single_item("label = 'File', target = '$P', arguments = '$P/$$C'",
        R"([paths]
P = '$C\literal$$.exe'
C = 'must not expand'
)");
    REQUIRE(literal.tabs[0].items[0].launch->target == R"($C\literal$.exe)");
    REQUIRE(literal.tabs[0].items[0].launch->arguments == R"($C\literal$.exe/$C)");
}

TEST_CASE("Path macros work for icon-only entries and Adobe default labels", "[config][paths][adobe-cc]") {
    const auto placeholder = parse_single_item("label = 'Folder', icon = '$I'",
        "[paths]\nI = 'Icons\\folder.ico'\n");
    REQUIRE(icon_path(placeholder.tabs[0].items[0]) == R"(Icons\folder.ico)");
    REQUIRE(placeholder.tabs[0].items[0].icon_explicit);
    REQUIRE_FALSE(placeholder.tabs[0].items[0].launch);
    const auto adobe = parse_single_item("kind = 'adobeCC', target = '$A'",
        "[paths]\nA = 'Adobe Audition'\n");
    REQUIRE(adobe.tabs[0].items[0].label == "Adobe Audition");
    REQUIRE(adobe.tabs[0].items[0].launch->target == "Adobe Audition");
    REQUIRE_FALSE(adobe.tabs[0].items[0].icon_explicit);
}

TEST_CASE("Percent escapes follow TOML decoding and preserve literal dollar escapes", "[config][paths][percent]") {
    const auto config = parse_single_item(
        R"(label = 'File', target = "C:\\Literal\\\u0024/WINDIR\u0024/\\$$/file.txt", )"
        R"(icon = 'Icons\$/WINDIR$/.ico', arguments = '--raw %WINDIR% --escaped $/WINDIR$/ --dollar $$/' )");
    const auto& item = config.tabs[0].items[0];
    REQUIRE(item.launch->target == R"(C:\Literal\%WINDIR%\$/file.txt)");
    REQUIRE(icon_path(item) == R"(Icons\%WINDIR%.ico)");
    REQUIRE(item.launch->arguments == "--raw %WINDIR% --escaped %WINDIR% --dollar $/");
    const auto dollar = parse_single_item("label = 'File', target = '$$/WINDIR$$/.txt'");
    REQUIRE(dollar.tabs[0].items[0].launch->target == "$/WINDIR$/.txt");
    const auto opening_only = parse_single_item("target = 'C:\\Literal\\$/WINDIR%.txt'");
    REQUIRE(opening_only.tabs[0].items[0].launch->target == R"(C:\Literal\%WINDIR%.txt)");
}

TEST_CASE("Percent escapes in substituted path prefixes decode in the final symbol pass", "[config][paths][percent]") {
    const auto config = parse_single_item(
        R"(label = 'File', target = '$P\$/PATH$/.exe', working_directory = 'Working $/PATH$/')",
        R"([paths]
P = 'C:\预算$2026\literal$$'
)");
    const auto& action = *config.tabs[0].items[0].launch;
    REQUIRE(action.target == R"(C:\预算$2026\literal$\%PATH%.exe)");
    REQUIRE(action.working_directory == "Working %PATH%");
    const auto literal = parse_single_item("label = 'File', target = '$P'", "[paths]\nP = 'C:\\literal$/.exe'\n");
    REQUIRE(literal.tabs[0].items[0].launch->target == R"(C:\literal%.exe)");
}

TEST_CASE("Percent escapes remain protected in each explicit icon-source form", "[config][paths][percent]") {
    for (const auto source : {"{file='$/WINDIR$/.dll'}", "{file='$/WINDIR$/.dll',index=0}", "{file='$/WINDIR$/.dll',type='SVG',name='ART'}"}) {
        const auto config = parse_single_item(std::string("label='Icon', icon=") + source);
        std::visit([](const auto& icon) {
            using T = std::decay_t<decltype(icon)>;
            if constexpr (std::is_same_v<T, launcher::FileIcon> || std::is_same_v<T, launcher::IconGroup> || std::is_same_v<T, launcher::PeResourceIcon>) {
                REQUIRE(icon.path == std::filesystem::path(L"%WINDIR%.dll"));
            } else { FAIL("Expected an explicit file icon"); }
        }, config.tabs[0].items[0].icon_source);
    }
}

TEST_CASE("Undefined path macros report the item field and complete identifier", "[config][paths]") {
    for (const auto field : {"target", "icon", "arguments"}) {
        CAPTURE(field);
        auto fields = std::string("label = 'File', ");
        if (std::string_view(field) != "target") { fields += "target = 'tool.exe', "; }
        fields += std::string(field) + " = '$P_2/file'";
        REQUIRE_THROWS_WITH(parse_single_item(fields, "[paths]\nP = 'wrong prefix'\n"),
            "tabs[0].items[0]: '" + std::string(field) + "' refers to undefined path 'P_2'");
    }
}

TEST_CASE("Path definitions reject invalid tables identifiers types and embedded NUL", "[config][paths]") {
    const std::string_view definitions[] = {
        "paths = 42", "paths = []", "paths = 'folder'",
        "[paths]\nP = 42", "[paths]\nP = false", "[paths]\nP = []", "[paths]\nP = {}",
        "[paths]\n'bad-key' = 'folder'", "[paths]\n'2P' = 'folder'", "[paths]\n'' = 'folder'",
        R"([paths]
P = "before\u0000after")",
    };
    for (const auto definition : definitions) {
        CAPTURE(definition);
        REQUIRE_THROWS_AS(parse_single_item("label = 'File', target = 'tool.exe'", definition), launcher::ConfigError);
    }
    REQUIRE_THROWS_AS(parse_single_item("label = 'File', target = '$P'", "[paths]\nP = ''"), launcher::ConfigError);
    REQUIRE_THROWS_AS(parse_single_item("label = 'File', icon = '$P'", "[paths]\nP = ''"), launcher::ConfigError);
}

TEST_CASE("Unescaped reserved dollar forms remain unavailable until implemented", "[config][paths]") {
    for (const auto field : {"target", "icon", "arguments"}) {
        for (const auto source : {"$", "$\\file", "$[0041]", "$[]", "folder/$P", "\"$P/file\""}) {
            CAPTURE(field, source);
            auto fields = std::string("label = 'File', ");
            if (std::string_view(field) != "target") { fields += "target = 'tool.exe', "; }
            fields += std::string(field) + " = '" + source + "'";
            REQUIRE_THROWS_AS(parse_single_item(fields), launcher::ConfigError);
        }
    }
    REQUIRE_THROWS_AS(parse_single_item(R"(label = 'File', icon = "bad\u0000icon")"), launcher::ConfigError);
}

TEST_CASE("Launch actions accept each supported visibility value", "[config][launch]") {
    const std::pair<std::string_view, launcher::LaunchShow> values[] = {
        {"normal", launcher::LaunchShow::normal},
        {"hidden", launcher::LaunchShow::hidden},
        {"minimized", launcher::LaunchShow::minimized},
    };
    for (const auto& [name, expected] : values) {
        CAPTURE(name);
        const auto config = parse_single_item("label = 'File', target = 'file.lnk', "
            "arguments = '', working_directory = '', show = '" +
            std::string(name) + "'");
        REQUIRE(config.tabs[0].items[0].launch.has_value());
        REQUIRE(config.tabs[0].items[0].launch->show == expected);
    }
}

TEST_CASE("Shift targets are independent actions sharing launch options", "[config][launch][shift-target]") {
    for (const bool normal : {false, true}) {
        const auto config = parse_single_item(std::string(normal ? "target = 'normal.exe', " : "") +
            R"(shift_target = '$Tools\edit-资料.exe', arguments = '--edit', working_directory = 'Work', show = 'minimized')",
            "[paths]\nTools = 'C:\\Tools'\n");
        const auto& item = config.tabs[0].items[0];
        REQUIRE(item.launch.has_value() == normal);
        REQUIRE(item.shift_launch);
        CHECK(item.shift_launch->target == R"(C:\Tools\edit-资料.exe)");
        CHECK(item.shift_launch->arguments == "--edit");
        CHECK(item.shift_launch->working_directory == "Work");
        CHECK(item.shift_launch->show == launcher::LaunchShow::minimized);
        CHECK(item.shift_launch->handler == "shell");
        if (normal) {
            auto expected = *item.launch;
            expected.target = item.shift_launch->target;
            CHECK(*item.shift_launch == expected);
        }
    }
    const auto normal = parse_single_item("target = 'normal.exe'");
    CHECK_FALSE(normal.tabs[0].items[0].shift_launch);
    const auto neither = parse_single_item("label = 'Decoration'");
    CHECK_FALSE(neither.tabs[0].items[0].launch);
    CHECK_FALSE(neither.tabs[0].items[0].shift_launch);
    const auto adobe = parse_single_item("kind = 'adobeCC', shift_target = 'Adobe Photoshop', include_beta = false");
    REQUIRE(adobe.tabs[0].items[0].shift_launch);
    CHECK(adobe.tabs[0].items[0].shift_launch->handler == "adobeCC");
    CHECK_FALSE(adobe.tabs[0].items[0].shift_launch->include_beta);
}

TEST_CASE("Shift targets validate strings and path substitutions", "[config][launch][shift-target]") {
    for (const auto value : {"42", "false", "[]", "''", R"("before\u0000after")", "'$Missing/tool.exe'"}) {
        CAPTURE(value);
        REQUIRE_THROWS_AS(parse_single_item(std::string("shift_target = ") + value), launcher::ConfigError);
    }
    const auto escaped = parse_single_item(R"(shift_target = 'C:\资料\$/PATH$/-$$.exe')");
    REQUIRE(escaped.tabs[0].items[0].shift_launch);
    CHECK(escaped.tabs[0].items[0].shift_launch->target == R"(C:\资料\%PATH%-$.exe)");
}

TEST_CASE("Launch options require a target even when their value is empty", "[config][launch]") {
    const std::string_view fields[] = {
        "arguments = ''", "arguments = 'one two'",
        "working_directory = ''", "working_directory = 'D:\\Work'",
        "show = 'normal'", "handler = 'shell'",
    };
    for (const auto field : fields) {
        CAPTURE(field);
        REQUIRE_THROWS_AS(parse_single_item("label = 'Placeholder', " + std::string(field)),
            launcher::ConfigError);
    }
}

TEST_CASE("Launch actions reject empty targets, malformed types and unsupported visibility", "[config][launch]") {
    const std::string_view fields[] = {
        "target = ''", "target = 42", "target = false",
        "target = 'tool.exe', handler = ''", "target = 'tool.exe', handler = 42",
        "target = 'tool.exe', arguments = 42", "target = 'tool.exe', arguments = []",
        "target = 'tool.exe', working_directory = false",
        "target = 'tool.exe', show = 'maximized'", "target = 'tool.exe', show = 'Normal'",
        "target = 'tool.exe', show = ''", "target = 'tool.exe', show = false",
        "target = 'tool.exe', icon = ''", "icon = ''", "icon = false",
    };
    for (const auto field : fields) {
        CAPTURE(field);
        REQUIRE_THROWS_AS(parse_single_item("label = 'File', " + std::string(field)),
            launcher::ConfigError);
    }
}

TEST_CASE("Launch strings reject escaped embedded NUL before reaching native APIs", "[config][launch]") {
    const std::string_view fields[] = {"target", "arguments", "working_directory", "handler", "show"};
    for (const auto field : fields) {
        CAPTURE(field);
        auto source = std::string("label = 'File', ");
        if (field != "target") { source += "target = 'tool.exe', "; }
        source += std::string(field) + R"( = "before\u0000after")";
        bool rejected = false;
        try {
            (void)parse_single_item(source);
        } catch (const launcher::ConfigError& error) {
            rejected = true;
            REQUIRE(std::string_view(error.what()).find("embedded NUL") != std::string_view::npos);
        }
        REQUIRE(rejected);
    }
}

TEST_CASE("Structural entries reject launch fields as well as labels and icons", "[config][launch]") {
    const std::string_view kinds[] = {"spacer", "break"};
    const std::string_view fields[] = {
        "target = 'file.txt'", "shift_target = 'file.txt'", "arguments = ''", "working_directory = ''",
        "handler = 'shell'", "show = 'normal'", "icon = { stock = 'folder' }", "label = 'File'",
    };
    for (const auto kind : kinds) {
        const auto prefix = "kind = '" + std::string(kind) + "'";
        const auto valid = parse_single_item(prefix);
        REQUIRE_FALSE(valid.tabs[0].items[0].launch.has_value());
        for (const auto field : fields) {
            CAPTURE(kind, field);
            REQUIRE_THROWS_AS(parse_single_item(prefix + ", " + std::string(field)),
                launcher::ConfigError);
        }
    }
}

TEST_CASE("Malformed TOML and invalid typed settings yield ConfigError", "[config]") {
    SECTION("Malformed syntax") {
        REQUIRE_THROWS_AS(launcher::parse_configuration("[grid\ncolumns = 3"), launcher::ConfigError);
    }
    SECTION("Zero dimension") {
        REQUIRE_THROWS_AS(launcher::parse_configuration(R"toml(
[grid]
columns = 0
[[tabs]]
name = "General"
)toml"), launcher::ConfigError);
    }
    SECTION("Fractional dimension") {
        REQUIRE_THROWS_AS(launcher::parse_configuration(R"toml(
[grid]
rows = 2.5
[[tabs]]
name = "General"
)toml"), launcher::ConfigError);
    }
    SECTION("An excessive integer dimension") {
        REQUIRE_THROWS_AS(launcher::parse_configuration(R"toml(
[grid]
columns = 129
[[tabs]]
name = "General"
)toml"), launcher::ConfigError);
    }
    SECTION("Unknown flow") {
        REQUIRE_THROWS_AS(launcher::parse_configuration(R"toml(
[grid]
flow = "diagonal"
[[tabs]]
name = "General"
)toml"), launcher::ConfigError);
    }
    SECTION("No tabs") {
        REQUIRE_THROWS_AS(launcher::parse_configuration("schema_version = 1"), launcher::ConfigError);
    }
    SECTION("Unknown coordinates are not accepted") {
        REQUIRE_THROWS_AS(launcher::parse_configuration(R"toml(
[[tabs]]
name = "General"
items = [{ label = "Dummy", column = 3 }]
)toml"), launcher::ConfigError);
    }
    SECTION("A structural entry cannot have an icon") {
        REQUIRE_THROWS_AS(launcher::parse_configuration(R"toml(
[[tabs]]
name = "General"
items = [{ kind = "break", icon = { stock = "folder" } }]
)toml"), launcher::ConfigError);
    }
    SECTION("Bad hexadecimal color") {
        REQUIRE_THROWS_AS(launcher::parse_configuration(R"toml(
[[tabs]]
name = "General"
color = "#GG0102"
)toml"), launcher::ConfigError);
    }
    SECTION("Nonfinite icon size") {
        REQUIRE_THROWS_AS(launcher::parse_configuration(R"toml(
[grid]
icon_size_dip = inf
[[tabs]]
name = "General"
)toml"), launcher::ConfigError);
    }
    SECTION("An excessive icon size") {
        REQUIRE_THROWS_AS(launcher::parse_configuration(R"toml(
[grid]
icon_size_dip = 257
[[tabs]]
name = "General"
)toml"), launcher::ConfigError);
    }
    SECTION("An icon size that underflows the native float") {
        REQUIRE_THROWS_AS(launcher::parse_configuration(R"toml(
[grid]
icon_size_dip = 1e-300
[[tabs]]
name = "General"
)toml"), launcher::ConfigError);
    }
}

TEST_CASE("Dock settings have stable defaults when omitted", "[config][dock]") {
    const auto config = launcher::parse_configuration(R"toml(
[[tabs]]
name = "General"
)toml");
    REQUIRE(config.dock.edge == launcher::Edge::right);
    REQUIRE(config.dock.display == "primary");
    REQUIRE(config.dock.auto_hide);
    REQUIRE(config.dock.hover_delay_ms == 180);
    REQUIRE(config.dock.hide_delay_ms == 350);
    REQUIRE(config.dock.reveal_duration_ms == 220);
    REQUIRE(config.dock.hide_duration_ms == 220);
    REQUIRE(config.dock.suppress_fullscreen);
}

TEST_CASE("TOML accepts each dock edge and retains omitted defaults", "[config][dock]") {
    const std::pair<std::string_view, launcher::Edge> edges[] = {
        {"top", launcher::Edge::top},
        {"bottom", launcher::Edge::bottom},
        {"left", launcher::Edge::left},
        {"right", launcher::Edge::right},
    };
    for (const auto& [name, expected] : edges) {
        CAPTURE(name);
        const auto source = "[dock]\nedge = '" + std::string(name) +
                            "'\n[[tabs]]\nname = 'General'\n";
        const auto config = launcher::parse_configuration(source);
        REQUIRE(config.dock.edge == expected);
        REQUIRE(config.dock.display == "primary");
        REQUIRE(config.dock.hover_delay_ms == 180);
        REQUIRE(config.dock.hide_delay_ms == 350);
    }
}

TEST_CASE("Travel durations are independent of dwell and share a default return speed", "[config][motion]") {
    const auto parse = [](std::string settings) {
        return launcher::parse_configuration("[dock]\n" + settings + "\n[[tabs]]\nname='General'\n");
    };
    const auto shared = parse("reveal_duration_ms=450");
    REQUIRE(shared.dock.reveal_duration_ms == 450);
    REQUIRE(shared.dock.hide_duration_ms == 450);
    REQUIRE(shared.dock.hover_delay_ms == 180);
    REQUIRE(shared.dock.hide_delay_ms == 350);
    const auto separate = parse("reveal_duration_ms=0\nhide_duration_ms=5000");
    REQUIRE(separate.dock.reveal_duration_ms == 0);
    REQUIRE(separate.dock.hide_duration_ms == 5000);
    for (const auto key : {"reveal_duration_ms", "hide_duration_ms"}) {
        for (const auto value : {"-1", "5001", "1.5", "true", "'200'"}) {
            REQUIRE_THROWS_AS(parse(std::string(key) + "=" + value), launcher::ConfigError);
        }
    }
}

TEST_CASE("Tab text direction and prism reversal are independent options", "[config][orientation]") {
    const auto parse = [](std::string settings) {
        return launcher::parse_configuration("[appearance]\n" + settings + "\n[[tabs]]\nname='Playing'\n");
    };
    REQUIRE_FALSE(parse("").appearance.reverse_thickness);
    REQUIRE(parse("").appearance.tab_text_direction == launcher::TabTextDirection::automatic);
    const auto configured = parse("reverse_thickness=true\ntab_text_direction='top-to-bottom'");
    REQUIRE(configured.appearance.reverse_thickness);
    REQUIRE(configured.appearance.tab_text_direction == launcher::TabTextDirection::top_to_bottom);
    REQUIRE(parse("tab_text_direction='bottom-to-top'").appearance.tab_text_direction == launcher::TabTextDirection::bottom_to_top);
    for (const auto setting : {"tab_text_direction='sideways'", "tab_text_direction=true", "reverse_thickness=1"}) {
        REQUIRE_THROWS_AS(parse(setting), launcher::ConfigError);
    }
}

TEST_CASE("Dock settings preserve a named display and accept both delay endpoints", "[config][dock]") {
    const auto config = launcher::parse_configuration(R"toml(
[dock]
edge = "left"
display = '\\.\DISPLAY2'
auto_hide = false
hover_delay_ms = 0
hide_delay_ms = 5000
suppress_fullscreen = false
[[tabs]]
name = "General"
)toml");
    REQUIRE(config.dock.edge == launcher::Edge::left);
    REQUIRE(config.dock.display == R"(\\.\DISPLAY2)");
    REQUIRE_FALSE(config.dock.auto_hide);
    REQUIRE(config.dock.hover_delay_ms == 0);
    REQUIRE(config.dock.hide_delay_ms == launcher::max_dock_delay_ms);
    REQUIRE_FALSE(config.dock.suppress_fullscreen);
}

TEST_CASE("Dock settings reject unknown values and incorrect TOML types", "[config][dock]") {
    const std::string_view invalid_settings[] = {
        "edge = 'center'",
        "edge = 1",
        "display = ''",
        "display = 1",
        "auto_hide = 'true'",
        "auto_hide = 1",
        "hover_delay_ms = -1",
        "hover_delay_ms = 5001",
        "hover_delay_ms = 1.5",
        "hover_delay_ms = '180'",
        "hide_delay_ms = -1",
        "hide_delay_ms = 5001",
        "hide_delay_ms = 1.5",
        "hide_delay_ms = true",
        "suppress_fullscreen = 'false'",
        "suppress_fullscreen = 0",
        "unexpected = 1",
    };
    for (const auto setting : invalid_settings) {
        CAPTURE(setting);
        const auto source = "[dock]\n" + std::string(setting) +
                            "\n[[tabs]]\nname = 'General'\n";
        REQUIRE_THROWS_AS(launcher::parse_configuration(source), launcher::ConfigError);
    }
    REQUIRE_THROWS_AS(launcher::parse_configuration("dock = 'right'\n[[tabs]]\nname = 'General'"),
                      launcher::ConfigError);
}

TEST_CASE("Icon tooltip delay retains its default when appearance settings are omitted",
          "[config][tooltip]") {
    const std::string_view prefixes[] = {
        "",
        "[appearance]\n",
        "[appearance]\nfont_weight=600\n",
    };
    for (const auto prefix : prefixes) {
        CAPTURE(prefix);
        const auto config = launcher::parse_configuration(
            std::string(prefix) + "[[tabs]]\nname='General'\n");
        REQUIRE(config.appearance.tooltip_delay_ms == 700);
    }
}

TEST_CASE("Icon tooltip delay accepts both millisecond endpoints", "[config][tooltip]") {
    const std::uint32_t delays[] = {0, launcher::max_tooltip_delay_ms};
    for (const auto delay : delays) {
        CAPTURE(delay);
        const auto config = launcher::parse_configuration(
            "[appearance]\ntooltip_delay_ms=" + std::to_string(delay) +
            "\n[[tabs]]\nname='General'\n");
        REQUIRE(config.appearance.tooltip_delay_ms == delay);
        REQUIRE_NOTHROW(launcher::layout_configuration(config));
    }
}

TEST_CASE("Icon tooltip delay rejects invalid lengths and TOML types", "[config][tooltip]") {
    const std::string_view values[] = {"-1", "5001", "1.5", "700.0", "'700'", "true"};
    for (const auto value : values) {
        CAPTURE(value);
        const auto source = "[appearance]\ntooltip_delay_ms=" + std::string(value) +
                            "\n[[tabs]]\nname='General'\n";
        REQUIRE_THROWS_AS(launcher::parse_configuration(source), launcher::ConfigError);
    }
}

TEST_CASE("Programmatic configuration also bounds icon tooltip delay", "[config][tooltip]") {
    auto config = configuration({icon()});
    REQUIRE(config.appearance.tooltip_delay_ms == 700);
    config.appearance.tooltip_delay_ms = launcher::max_tooltip_delay_ms + 1;
    REQUIRE_THROWS_AS(launcher::layout_configuration(config), launcher::ConfigError);
}
