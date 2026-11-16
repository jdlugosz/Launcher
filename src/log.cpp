#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#if LAUNCHER_USE_JMD_MODULES && !defined(__INTELLISENSE__)
import jmd.log;
#else
#include <jmd/log.hpp>
#endif

#include "launcher/log.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace launcher {
namespace {

constexpr std::size_t kMessageLimit = 4096;
constexpr std::size_t kPendingLimit = 256;
constexpr std::string_view kTruncated = " [truncated]";

} // namespace

Log::Log(std::filesystem::path default_path) : path_(std::move(default_path)) {}

void Log::configure(const LoggingOptions& options, const std::filesystem::path& config_directory, const FileLookup& files) {
    files_ = files;
    if (options.file) { path_ = config_directory / *options.file; }
}

std::filesystem::path Log::filesystem_error_path() const {
    return path_.parent_path() / (path_.stem().native() + L".filesystem-errors.log");
}

void Log::resume() noexcept {
    if (!deferred_) { return; }
    deferred_ = false;
    try {
        path_ = files_.exact_path(path_);
        write_record({});
        for (const auto& record : pending_) { write_record(record); }
    } catch (...) {
        // Diagnostics must never prevent the launcher from starting.
    }
    pending_.clear();
}

void Log::write_parts(std::span<const std::string_view> parts) noexcept {
    try {
        if (deferred_ && pending_.size() >= kPendingLimit) { return; }
        std::size_t length = 0;
        bool truncated = false;
        for (const auto part : parts) {
            const auto count = std::min(part.size(), kMessageLimit - length);
            length += count;
            truncated = truncated || count < part.size();
        }
        SYSTEMTIME time{};
        GetSystemTime(&time);
        std::array<char, 64> prefix;
        const auto formatted = std::format_to_n(prefix.data(), prefix.size(), "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z pid={} ",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, GetCurrentProcessId());
        const auto prefix_length = static_cast<std::size_t>(formatted.size);
        std::string record;
        record.reserve(prefix_length + length + (truncated ? kTruncated.size() : 0) + 1);
        record.append(prefix.data(), prefix_length);
        auto remaining = length;
        for (const auto part : parts) {
            const auto count = std::min(part.size(), remaining);
            record.append(part.substr(0, count));
            remaining -= count;
            if (remaining == 0) { break; }
        }
        if (truncated) { record.append(kTruncated); }
        record.push_back('\n');
        if (deferred_) { pending_.push_back(std::move(record)); }
        else { write_record(record); }
    } catch (...) {
    }
}

void Log::write_record(std::string_view record) noexcept {
    (void)jmd::append_rotating_log(path_, record, {.fit_record_before_rotation = false});
}

void Log::result(std::string_view operation, std::int32_t value) noexcept {
    try {
        std::array<char, 8> hexadecimal;
        const auto formatted = std::format_to_n(hexadecimal.data(), hexadecimal.size(), "{:X}", static_cast<std::uint32_t>(value));
        write(operation, " HRESULT=0x", std::string_view(hexadecimal.data(), static_cast<std::size_t>(formatted.size)));
    } catch (...) {
    }
}

} // namespace launcher
