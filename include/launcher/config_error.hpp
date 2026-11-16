#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace launcher {

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;

    // Populated by the loader that selected the file, independent of diagnostic text.
    std::filesystem::path source_file;
    std::uint32_t source_line = 0;
    std::uint32_t source_column = 0;
};

class UnknownColorError : public ConfigError {
public:
    explicit UnknownColorError(std::string color_name, std::string location = {})
        : ConfigError((location.empty() ? std::string{} : location + ": ") +
              "unknown color palette '" + color_name + "'"), name(std::move(color_name)) {}

    std::string name;
    std::vector<std::string> sources;
};

} // namespace launcher
