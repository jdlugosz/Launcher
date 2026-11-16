#pragma once

#include <filesystem>
#include <memory>

namespace launcher {

// One default drawer per Windows user, session, and installation directory.
class DefaultDrawer final {
public:
    explicit DefaultDrawer(const std::filesystem::path& installation);
    ~DefaultDrawer();
    DefaultDrawer(const DefaultDrawer&) = delete;
    DefaultDrawer& operator=(const DefaultDrawer&) = delete;

    // False means an ordinary launch handed off its reveal to the owner.
    // A monitored Setup launch needs to initialize itself and reports a conflict.
    bool start(bool forward_reveal);
    void reserve_restart();
    void finish_restart();
    HANDLE reveal_event() const noexcept { return reveal_.get(); }

private:
    struct CloseHandle final {
        void operator()(void* handle) const noexcept { ::CloseHandle(handle); }
    };
    using Handle = std::unique_ptr<void, CloseHandle>;

    Handle owner_;
    Handle transition_;
    Handle reveal_;
    bool owns_drawer_ = false;
    bool owns_transition_ = false;
};

} // namespace launcher
