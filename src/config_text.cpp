#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/config_text.hpp"
#include "launcher/config.hpp"
#include "launcher/file_lookup.hpp"

#include <ranges>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace launcher {

using std::size_t;
using std::string;
using std::string_view;

namespace {

bool is_stop_directive(string_view line, bool first_line) {
    if (first_line && line.starts_with("\xEF\xBB\xBF")) { line.remove_prefix(3); }
    return line.starts_with("%STOP") && (line.size() == 5 ||
        line[5] == ' ' || line[5] == '\t' || line[5] == '\r');
}

} // namespace

config_text_builder::config_text_builder(const std::filesystem::path& filename, const FileLookup& files) {
    auto loaded = files.read_text(filename, FileFallback::exact_file);
    source_name_ = std::move(loaded.source);
    append(std::move(loaded.text));
}

config_text_builder config_text_builder::from_text(string_view source, string_view source_name) {
    config_text_builder builder;
    builder.source_name_ = source_name;
    builder.append(string(source));
    return builder;
}

void config_text_builder::append(string source) {
    size_t start = 0;
    // Preprocessing is independent of TOML strings, comments and containers.
    for (const auto line_range : source | std::views::split('\n')) {
        const string_view line(line_range);
        if (is_stop_directive(line, start == 0)) {
            source.resize(start);
            break;
        }
        start += line.size() + 1;
    }
    pieces_.push_back(std::move(source));
}

string config_text_builder::build() {
    if (pieces_.size() == 1) {
        return std::move(pieces_.front());
    }
    size_t length = 0;
    for (const auto& piece : pieces_) {
        length += piece.size();
    }
    string result;
    result.reserve(length);
    for (const auto& piece : pieces_) { result.append(piece); }
    return result;
}

toml::table config_text_builder::parse() {
    const auto source = build();
    try {
        return toml::parse(source, source_name_);
    } catch (const toml::parse_error& error) {
        std::ostringstream message;
        message << "Invalid TOML: " << error;
        ConfigError diagnostic(message.str());
        diagnostic.source_line = error.source().begin.line;
        diagnostic.source_column = error.source().begin.column;
        throw diagnostic;
    }
}

} // namespace launcher
