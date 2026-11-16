#pragma once

#include "launcher/file_lookup.hpp"

#include <toml++/toml.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

// Schema-independent TOML preprocessing. Only column-one %STOP is implemented.
// Future include expansion belongs here so file-loading callers stay unchanged.
class config_text_builder {
public:
    explicit config_text_builder(const std::filesystem::path& filename, const FileLookup& files = FileLookup({}, {}));
    static config_text_builder from_text(std::string_view source, std::string_view source_name = {});

    std::string build();
    toml::table parse();

private:
    config_text_builder() = default;
    void append(std::string source);

    std::vector<std::string> pieces_;
    std::string source_name_;
};

} // namespace launcher
