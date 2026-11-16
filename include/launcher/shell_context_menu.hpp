#pragma once

#include "launcher/windows_pch.hpp"

#include <memory>
#include <string>

namespace launcher {

// Keep this object alive on the UI thread through menu tracking and invocation.
// append() borrows its menu; the caller owns and destroys that menu.
class ShellContextMenu final {
public:
    ShellContextMenu(HWND owner, const std::wstring& target);
    ~ShellContextMenu();

    ShellContextMenu(const ShellContextMenu&) = delete;
    ShellContextMenu& operator=(const ShellContextMenu&) = delete;

    bool available() const noexcept;
    HRESULT append(HMENU menu, UINT first_id, UINT last_id, bool extended = false);
    bool owns(UINT command) const noexcept;
    std::wstring verb(UINT command) const;
    HRESULT invoke(UINT command, POINT screen, const std::wstring& arguments = {},
        const std::wstring& working_directory = {});
    bool handle_message(UINT message, WPARAM wparam, LPARAM lparam,
        LRESULT& result) const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace launcher
