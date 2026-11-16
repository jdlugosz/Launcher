#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/app_notification.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

TEST_CASE("Notification journal activation accepts absolute filesystem log paths", "[app-notification]") {
    // Validation is lexical: these tests create no identity, registry entries,
    // files, COM server or visible Windows notifications.
    constexpr std::array paths{
        LR"(C:\Users\Example\AppData\Local\Długosz\Launcher\launcher.filesystem-errors.log)",
        LR"(D:\Portable Launcher\Logs\custom.filesystem-errors.log)",
        LR"(C:/Portable/Logs/custom.filesystem-errors.log)",
        LR"(C:\资料\日志😀\启动器.filesystem-errors.log)",
        LR"(C:\Logs\John's & shared.filesystem-errors.log)",
        LR"(\\fileserver\Shared\Logs\custom.filesystem-errors.log)",
        LR"(\\?\C:\Logs\custom.filesystem-errors.log)",
        LR"(\\?\UNC\fileserver\Shared\Logs\custom.filesystem-errors.log)",
        LR"(C:\Logs\custom.FILESYSTEM-ERRORS.LOG)"};
    for (const auto* path : paths) {
        CAPTURE(path);
        CHECK(launcher::AppNotification::valid_journal_path(std::filesystem::path(path)));
    }
}

TEST_CASE("Notification journal activation rejects ambiguous and unrelated paths", "[app-notification]") {
    constexpr std::array paths{
        L"",
        LR"(launcher.filesystem-errors.log)",
        LR"(Logs\launcher.filesystem-errors.log)",
        LR"(C:Logs\launcher.filesystem-errors.log)",
        LR"(\Logs\launcher.filesystem-errors.log)",
        LR"(C:\Logs\launcher.log)",
        LR"(C:\Logs\launcher.filesystem-errors.log.exe)",
        LR"(C:\Logs\launcher.filesystem-errors.log.1)",
        LR"(C:\Logs\launcher.filesystem-errors.log\)",
        LR"(C:\Logs\*.filesystem-errors.log)",
        LR"(C:\Logs\version?\launcher.filesystem-errors.log)",
        LR"(C:\Logs\bad"name.filesystem-errors.log)",
        LR"(C:\Logs\bad<name.filesystem-errors.log)",
        LR"(C:\Logs\bad>name.filesystem-errors.log)",
        LR"(C:\Logs\bad|name.filesystem-errors.log)",
        LR"(https://example.com/launcher.filesystem-errors.log)",
        LR"(file:///C:/Logs/launcher.filesystem-errors.log)"};
    for (const auto* path : paths) {
        CAPTURE(path);
        CHECK_FALSE(launcher::AppNotification::valid_journal_path(std::filesystem::path(path)));
    }
    std::wstring embedded_nul = L"C:\\Logs\\prefix";
    embedded_nul += L'\0';
    embedded_nul += L"launcher.filesystem-errors.log";
    CHECK_FALSE(launcher::AppNotification::valid_journal_path(std::filesystem::path(embedded_nul)));
    for (wchar_t control = 1; control < 0x20; ++control) {
        CAPTURE(static_cast<unsigned>(control));
        const std::wstring path = L"C:\\Logs\\prefix" + std::wstring(1, control) + L"launcher.filesystem-errors.log";
        CHECK_FALSE(launcher::AppNotification::valid_journal_path(std::filesystem::path(path)));
    }
}

TEST_CASE("Notification journal activation bounds full native path storage", "[app-notification]") {
    constexpr std::wstring_view suffix = L".filesystem-errors.log";
    std::wstring path = L"C:\\";
    path += std::wstring(32767 - path.size() - suffix.size(), L'a');
    path += suffix;
    REQUIRE(path.size() == 32767);
    CHECK(launcher::AppNotification::valid_journal_path(std::filesystem::path(path)));
    path.insert(3, 1, L'a');
    REQUIRE(path.size() == 32768);
    CHECK_FALSE(launcher::AppNotification::valid_journal_path(std::filesystem::path(path)));
}
