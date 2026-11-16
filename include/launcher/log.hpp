#pragma once

#include "launcher/config.hpp"
#include "launcher/file_lookup.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

class Log final {
public:
    explicit Log(std::filesystem::path default_path);
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    void configure(const LoggingOptions& options, const std::filesystem::path& config_directory, const FileLookup& files = FileLookup({}, {}));
    std::filesystem::path filesystem_error_path() const;
    const FileLookup& files() const { return files_; }
    void resume() noexcept;

    // Views need only survive this call; deferred records own their text.
    template<class... Parts>
    void write(std::string_view first, const Parts&... rest) noexcept {
        const std::string_view parts[]{first, rest...};
        write_parts(parts);
    }

    void result(std::string_view operation, std::int32_t value) noexcept;

private:
    void write_parts(std::span<const std::string_view> parts) noexcept;
    void write_record(std::string_view record) noexcept;

    FileLookup files_{{}, {}};
    std::filesystem::path path_;
    bool deferred_ = true;
    std::vector<std::string> pending_;
};

} // namespace launcher
