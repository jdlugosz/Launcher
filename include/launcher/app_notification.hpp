#pragma once

#include "launcher/windows_pch.hpp"

#include <filesystem>
#include <memory>
#include <optional>

namespace launcher {

// SDK-only persistent Windows app notifications. Construct/use/destroy on the
// COM-initialized owner thread. Activations enter an owned, bounded inbox; they
// never retain a Window pointer. Registration/history deliberately survive exit.
class AppNotification final {
public:
    explicit AppNotification(HANDLE owner_completion_event = nullptr);
    ~AppNotification();
    AppNotification(const AppNotification&) = delete;
    AppNotification& operator=(const AppNotification&) = delete;

    // Scenario builds use a separate identity, preserving the installed
    // application's registration. Cold activation requires no configuration.
    HRESULT register_server(const std::filesystem::path& executable, bool test_identity = false) noexcept;
    HRESULT show(const std::filesystem::path& journal) noexcept;
    std::optional<std::filesystem::path> take_activation() noexcept;
    HANDLE activation_event() const noexcept;

    static bool valid_journal_path(const std::filesystem::path& path) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace launcher
