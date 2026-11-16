#pragma once

#if !defined(LAUNCHER_VERSION_TEST_HOOKS) || !LAUNCHER_VERSION_TEST_HOOKS
#error Version scenario controls belong only in the explicitly instrumented build.
#endif

#include "launcher/launch.hpp"
#include "launcher/version_discovery.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace launcher {

enum class VersionScenarioCommandKind : std::uint16_t { close = 1, fatal_error, restart, select_page, checkpoint, activate_item, open_icon_menu };

struct VersionScenarioCommand final {
    VersionScenarioCommandKind kind;
    std::uint16_t page = 0;
};

// Test-only application adapter. Every path is explicit and confined to an
// owned fixture directory; searches can explicitly name another marked fixture
// while configuration and evidence remain internal. Dispatch is captured, and
// native work is gated by current-user-only named Events. There is no control
// thread or polling timer.
// Shared scanner captures retain the controller while a worker is held.
class VersionScenarios final : public std::enable_shared_from_this<VersionScenarios> {
public:
    static std::shared_ptr<VersionScenarios> load(const std::filesystem::path& scenario,
        const std::filesystem::path& explicit_configuration);
    ~VersionScenarios();
    VersionScenarios(const VersionScenarios&) = delete;
    VersionScenarios& operator=(const VersionScenarios&) = delete;

    VersionDiscoveryService::Scanner scanner();
    void capture_action(const PreparedLaunch& launch, std::string_view verb, bool reveal);
    void checkpoint(std::string_view stage);
    // The harness sends the manifest's registered message to its verified PID's
    // window, with command/page in WPARAM and that PID in LPARAM. Interpret this
    // in the outer message loop, so fatal_error unwinds the real UI/log owners.
    std::optional<VersionScenarioCommand> command(std::uint32_t message, std::uintptr_t parameter,
        std::intptr_t process) const noexcept;
    const std::filesystem::path& configuration_path() const noexcept;
    const std::filesystem::path& output_directory() const noexcept;
    // Real native notifications are opt-in for isolated interactive scenarios.
    bool notifications_enabled() const noexcept;

private:
    struct Impl;
    explicit VersionScenarios(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace launcher
