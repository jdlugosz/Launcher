#include <windows.h>
#include <shlobj.h>
#include <wrl/client.h>

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "entry.hpp"
#include "editors.hpp"
#include "configuration.hpp"
#include "launcher/config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

std::string utf8(const std::wstring& value) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    REQUIRE(size > 0);
    std::string result(size, '\0');
    REQUIRE(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) == size);
    return result;
}

struct OleSession {
    OleSession() { REQUIRE(SUCCEEDED(OleInitialize(nullptr))); }
    ~OleSession() { OleUninitialize(); }
};

struct ConfigurationFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        (L"LauncherDropConfig-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::path program = root / "program", user = root / "user";
    ConfigurationFixture() {
        REQUIRE(std::filesystem::create_directory(root));
        REQUIRE(std::filesystem::create_directory(program));
    }
    ~ConfigurationFixture() {
        std::error_code ignored;
        for (const auto& path : {program / "Launcher.toml", program / "Launcher.default.toml", user / "Launcher.toml", program, user, root}) {
            std::filesystem::remove(path, ignored);
        }
    }
    void write(const std::filesystem::path& path, std::string_view text) {
        std::ofstream file(path, std::ios::binary);
        file << text;
        file.close();
        REQUIRE(file.good());
    }
    std::string read(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        REQUIRE(file.good());
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
};

} // namespace

TEST_CASE("Drop configuration shares portable lookup and first-use seeding", "[drop-target][storage]") {
    ConfigurationFixture fixture;
    const auto portable = fixture.program / "Launcher.toml";
    const auto user = fixture.user / "Launcher.toml";
    const auto user_directory = [&] { return fixture.user; };

    SECTION("Portable settings need no bundled template") {
        fixture.write(portable, "# Portable settings stay untouched\n");
        REQUIRE(launcher::drop::default_configuration(fixture.program, user_directory) == portable);
        REQUIRE(fixture.read(portable) == "# Portable settings stay untouched\n");
        REQUIRE_FALSE(std::filesystem::exists(fixture.user));
    }
    SECTION("Existing user settings are preserved without requiring a bundled template") {
        REQUIRE(std::filesystem::create_directory(fixture.user));
        fixture.write(user, "# Customized user settings\n");
        REQUIRE(launcher::drop::default_configuration(fixture.program, user_directory) == user);
        REQUIRE(fixture.read(user) == "# Customized user settings\n");
        REQUIRE_FALSE(std::filesystem::exists(portable));
    }
    SECTION("Missing portable settings receive a usable bundled starter beside the executable") {
        constexpr std::string_view starter = "[[tabs]]\nname='Starter'\nitems=[{target='calc.exe'}]\n";
        fixture.write(fixture.program / "Launcher.default.toml", starter);
        REQUIRE(launcher::drop::default_configuration(fixture.program, user_directory) == portable);
        REQUIRE(fixture.read(portable) == starter);
        REQUIRE(launcher::load_configuration(portable).tabs.front().name == "Starter");
        REQUIRE_FALSE(std::filesystem::exists(fixture.user));
    }
    SECTION("An unusable local configuration does not silently select AppData") {
        REQUIRE(std::filesystem::create_directory(portable));
        REQUIRE_THROWS_AS(launcher::drop::default_configuration(fixture.program, user_directory), launcher::ConfigError);
        REQUIRE_FALSE(std::filesystem::exists(fixture.user));
    }
    SECTION("An explicit relative filename searches the same roots without seeding") {
        fixture.write(fixture.program / "custom.toml", "# custom\n");
        REQUIRE(launcher::drop::default_configuration(fixture.program, user_directory, "custom.toml") == fixture.program / "custom.toml");
        REQUIRE_THROWS_AS(launcher::drop::default_configuration(fixture.program, user_directory, "missing.toml"), launcher::ConfigError);
        std::filesystem::remove(fixture.program / "custom.toml");
        REQUIRE_FALSE(std::filesystem::exists(portable));
        REQUIRE_FALSE(std::filesystem::exists(fixture.user));
    }
}

TEST_CASE("Dropped targets survive TOML parsing and Launcher's macro processing", "[drop-target][config]") {
    const std::vector<std::wstring> targets{
        LR"(C:\Program Files\Example\Example.exe)",
        LR"(D:\John's notes\budget $2026.txt)",
        LR"(\\server\share$\音乐\曲目.txt)",
        LR"(C:\Cash$$\notes.txt)",
        LR"(C:\Literal\%WINDIR%\%PATH%\50% done.txt)",
        LR"(C:\资料 $$\$/\%UNDEFINED_LAUNCHER_VARIABLE%.txt)",
        L"shell:AppsFolder\\Example.App!App",
        L"::{645FF040-5081-101B-9F08-00AA002F954E}",
        L"https://example.test/a?x='$value'",
        L"C:\\Unlikely\\tab\tquote\".txt"
    };
    std::string entries;
    for (const auto& target : targets) { entries += utf8(launcher::drop::format_entry(target)); }
    const auto config = launcher::parse_configuration("[[tabs]]\nname='Dropped'\nitems=[\n" + entries + "]\n");
    REQUIRE(config.tabs.size() == 1);
    REQUIRE(config.tabs.front().items.size() == targets.size());
    for (std::size_t index = 0; index < targets.size(); ++index) {
        REQUIRE(config.tabs.front().items[index].launch.has_value());
        REQUIRE(config.tabs.front().items[index].launch->target == utf8(targets[index]));
    }
    REQUIRE(launcher::drop::format_entry(targets.front()) == L"    { target = 'C:\\Program Files\\Example\\Example.exe' },\r\n");
    REQUIRE(launcher::drop::format_entry(targets[1]).find(L"target = \"") != std::wstring::npos);
}

TEST_CASE("Dropped targets reject names Launcher cannot represent literally", "[drop-target][config]") {
    REQUIRE_THROWS(launcher::drop::format_entry(L""));
    REQUIRE_THROWS(launcher::drop::format_entry(std::wstring(L"abc\0def", 7)));
    REQUIRE(launcher::drop::format_entry(L"C:\\Literal\\%WINDIR%\\file.txt") == L"    { target = 'C:\\Literal\\$/WINDIR$/\\file.txt' },\r\n");
    REQUIRE_NOTHROW(launcher::drop::format_entry(L"C:\\Literal\\50% complete.txt"));
}

TEST_CASE("Drop feedback accepts copy and link sources without moving items", "[drop-target][shell]") {
    using launcher::drop::drop_effect;
    REQUIRE(drop_effect(DROPEFFECT_COPY | DROPEFFECT_MOVE, true) == DROPEFFECT_COPY);
    REQUIRE(drop_effect(DROPEFFECT_LINK, true) == DROPEFFECT_LINK);
    REQUIRE(drop_effect(DROPEFFECT_MOVE, true) == DROPEFFECT_NONE);
    REQUIRE(drop_effect(DROPEFFECT_COPY, false) == DROPEFFECT_NONE);
}

TEST_CASE("Shell drop conversion preserves filesystem and virtual item identity", "[drop-target][shell]") {
    OleSession ole;
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    REQUIRE(length > 0);
    REQUIRE(length < executable.size());
    executable.resize(length);
    ComPtr<IShellItem> file;
    REQUIRE(SUCCEEDED(SHCreateItemFromParsingName(executable.c_str(), nullptr, IID_PPV_ARGS(&file))));
    REQUIRE(launcher::drop::shell_target(file.Get()) == executable);
    ComPtr<IDataObject> file_data;
    REQUIRE(SUCCEEDED(file->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&file_data))));
    FORMATETC file_format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    REQUIRE(SUCCEEDED(file_data->QueryGetData(&file_format)));
    ComPtr<IShellItemArray> file_items;
    REQUIRE(SUCCEEDED(SHCreateShellItemArrayFromDataObject(file_data.Get(), IID_PPV_ARGS(&file_items))));
    ComPtr<IShellItem> dropped_file;
    REQUIRE(SUCCEEDED(file_items->GetItemAt(0, &dropped_file)));
    REQUIRE(launcher::drop::shell_target(dropped_file.Get()) == executable);

    ComPtr<IShellItem> recycle;
    REQUIRE(SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&recycle))));
    const auto target = launcher::drop::shell_target(recycle.Get());
    REQUIRE(target.starts_with(L"::"));
    ComPtr<IShellItem> recovered;
    REQUIRE(SUCCEEDED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&recovered))));
    int order = 1;
    REQUIRE(SUCCEEDED(recycle->Compare(recovered.Get(), SICHINT_CANONICAL, &order)));
    REQUIRE(order == 0);

    // Exercise the Shell ID-list data-object route used by the OLE drop target.
    struct PidlOwner {
        PIDLIST_ABSOLUTE value = nullptr;
        ~PidlOwner() { CoTaskMemFree(value); }
    } owned;
    REQUIRE(SUCCEEDED(SHGetIDListFromObject(recycle.Get(), &owned.value)));
    ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    REQUIRE(SUCCEEDED(SHBindToParent(owned.value, IID_PPV_ARGS(&parent), &child)));
    ComPtr<IDataObject> data;
    REQUIRE(SUCCEEDED(parent->GetUIObjectOf(nullptr, 1, &child, IID_IDataObject, nullptr, reinterpret_cast<void**>(data.GetAddressOf()))));
    ComPtr<IShellItemArray> items;
    REQUIRE(SUCCEEDED(SHCreateShellItemArrayFromDataObject(data.Get(), IID_PPV_ARGS(&items))));
    DWORD count = 0;
    REQUIRE(SUCCEEDED(items->GetCount(&count)));
    REQUIRE(count == 1);
    ComPtr<IShellItem> dropped;
    REQUIRE(SUCCEEDED(items->GetItemAt(0, &dropped)));
    REQUIRE(launcher::drop::shell_target(dropped.Get()) == target);
}

TEST_CASE("Dropped shortcuts retain their own path and launch settings", "[drop-target][shell]") {
    OleSession ole;
    const auto directory = std::filesystem::temp_directory_path() / (L"LauncherDrop-" + std::to_wstring(GetCurrentProcessId()));
    REQUIRE(std::filesystem::create_directory(directory));
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
    } cleanup{directory};
    const auto shortcut = directory / L"John's shortcut.lnk";
    ComPtr<IShellLinkW> link;
    REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))));
    REQUIRE(SUCCEEDED(link->SetPath(L"C:\\Windows\\System32\\notepad.exe")));
    REQUIRE(SUCCEEDED(link->SetArguments(L"\"C:\\Notes\\example.txt\"")));
    REQUIRE(SUCCEEDED(link->SetWorkingDirectory(directory.c_str())));
    ComPtr<IPersistFile> persisted;
    REQUIRE(SUCCEEDED(link.As(&persisted)));
    REQUIRE(SUCCEEDED(persisted->Save(shortcut.c_str(), TRUE)));
    ComPtr<IShellItem> item;
    REQUIRE(SUCCEEDED(SHCreateItemFromParsingName(shortcut.c_str(), nullptr, IID_PPV_ARGS(&item))));
    REQUIRE(launcher::drop::shell_target(item.Get()) == shortcut.wstring());
    REQUIRE(std::filesystem::exists(shortcut));
}

TEST_CASE("Editor discovery excludes missing programs and passes executable paths intact", "[drop-target][editors]") {
    const auto editors = launcher::drop::discover_editors([](std::wstring_view name) -> std::wstring {
        if (name == L"notepad++.exe") { return LR"(C:\Program Files\Notepad++\notepad++.exe)"; }
        if (name == L"Code.exe") { return LR"(C:\Users\Example\VS Code\Code.exe)"; }
        return {};
    });
    REQUIRE(editors.size() == 2);
    REQUIRE(editors[0].name == L"Notepad++");
    REQUIRE(editors[0].executable == LR"(C:\Program Files\Notepad++\notepad++.exe)");
    REQUIRE(editors[1].name == L"Visual Studio Code");
    REQUIRE(editors[1].executable == LR"(C:\Users\Example\VS Code\Code.exe)");
    REQUIRE(launcher::drop::discover_editors([](const wchar_t*) { return std::wstring{}; }).empty());
}

TEST_CASE("Editor registrations must name an existing absolute executable without arguments", "[drop-target][editors]") {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    REQUIRE(length > 0);
    REQUIRE(length < executable.size());
    executable.resize(length);
    REQUIRE(launcher::drop::editor_path(executable) == executable);
    REQUIRE(launcher::drop::editor_path(L"\"" + executable + L"\"") == executable);
    REQUIRE(launcher::drop::editor_path(L"\"" + executable + L"\" --argument").empty());
    REQUIRE(launcher::drop::editor_path(executable + L" --argument").empty());
    REQUIRE(launcher::drop::editor_path(L"Code.exe").empty());
    REQUIRE(launcher::drop::editor_path(L"C:Code.exe").empty());
    REQUIRE(launcher::drop::editor_path(LR"(C:\Unlikely-to-exist-LauncherDrop\Code.exe)").empty());
    REQUIRE(launcher::drop::editor_path(std::wstring(L"C:\\Code.exe\0hidden", 18)).empty());
    REQUIRE(launcher::drop::editor_path(L"").empty());
    REQUIRE(launcher::drop::editor_path(std::filesystem::temp_directory_path().wstring()).empty());
    const auto file = std::filesystem::path(executable);
    REQUIRE(launcher::drop::editor_on_path(file.filename().c_str(), file.parent_path().wstring()) == executable);
    REQUIRE(launcher::drop::editor_on_path(file.filename().c_str(), L"").empty());
    REQUIRE(launcher::drop::editor_on_path(L"Missing-LauncherDrop-editor.exe", file.parent_path().wstring()).empty());
}

TEST_CASE("Editor App Paths handle quoted and expandable registrations and reject stale entries", "[drop-target][editors]") {
    // A volatile scratch root keeps real editor registrations and settings untouched.
    struct RegistryFixture {
        std::wstring name = L"Software\\LauncherDropTests-" + std::to_wstring(GetCurrentProcessId());
        HKEY root = nullptr, entry = nullptr;
        RegistryFixture() {
            REQUIRE(RegCreateKeyExW(HKEY_CURRENT_USER, name.c_str(), 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS);
            REQUIRE(RegCreateKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Example.exe", 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &entry, nullptr) == ERROR_SUCCESS);
        }
        ~RegistryFixture() {
            if (entry) { RegCloseKey(entry); }
            if (root) { RegCloseKey(root); RegDeleteTreeW(HKEY_CURRENT_USER, name.c_str()); }
        }
        void set(DWORD type, const std::wstring& value) {
            const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
            REQUIRE(RegSetValueExW(entry, nullptr, 0, type, reinterpret_cast<const BYTE*>(value.c_str()), bytes) == ERROR_SUCCESS);
        }
    } registry;
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    REQUIRE(length > 0);
    REQUIRE(length < executable.size());
    executable.resize(length);
    registry.set(REG_SZ, L"\"" + executable + L"\"");
    REQUIRE(launcher::drop::registered_editor(L"Example.exe", registry.root, 0) == executable);
    registry.set(REG_EXPAND_SZ, LR"(%WINDIR%\System32\notepad.exe)");
    const auto expanded = launcher::drop::registered_editor(L"Example.exe", registry.root, 0);
    REQUIRE_FALSE(expanded.empty());
    REQUIRE(expanded.find(L'%') == std::wstring::npos);
    registry.set(REG_SZ, LR"(C:\Unlikely-to-exist-LauncherDrop\Example.exe)");
    REQUIRE(launcher::drop::registered_editor(L"Example.exe", registry.root, 0).empty());
    REQUIRE(launcher::drop::registered_editor(L"Missing.exe", registry.root, 0).empty());
}
