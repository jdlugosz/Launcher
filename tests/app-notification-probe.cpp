#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/app_notification.hpp"

#include <notificationactivationcallback.h>
#include <roapi.h>
#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>
#include <wrl/wrappers/corewrappers.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;
namespace notifications = ABI::Windows::UI::Notifications;
namespace xml = ABI::Windows::Data::Xml::Dom;

constexpr wchar_t test_application_id[] = L"Dlugosz.Launcher.VersionTest.Filesystem";
constexpr CLSID test_activator{0x1ef312f7, 0x3027, 0x4558, {0x96, 0x6a, 0xd4, 0xd2, 0x13, 0xa9, 0x74, 0x25}};

class ProbeFailure final : public std::runtime_error {
public:
    ProbeFailure(std::string_view operation, HRESULT result) : std::runtime_error(std::format("{} failed: 0x{:08X}", operation, static_cast<std::uint32_t>(result))), result_(result) {}
    HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_;
};

void check(HRESULT result, std::string_view operation) {
    if (FAILED(result)) { throw ProbeFailure(operation, result); }
}

class RuntimeApartment final {
public:
    RuntimeApartment() { check(RoInitialize(RO_INIT_SINGLETHREADED), "RoInitialize(STA)"); }
    ~RuntimeApartment() { RoUninitialize(); }
    RuntimeApartment(const RuntimeApartment&) = delete;
    RuntimeApartment& operator=(const RuntimeApartment&) = delete;
};

std::wstring_view view(const HString& value) noexcept {
    UINT32 count = 0;
    const auto text = value.GetRawBuffer(&count);
    return count == 0 ? std::wstring_view{} : std::wstring_view(text, count);
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) { return {}; }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) { throw std::length_error("Probe text is too long."); }
    const auto length = static_cast<int>(value.size());
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, nullptr, 0, nullptr, nullptr);
    if (size == 0) { throw ProbeFailure("Encode probe evidence", HRESULT_FROM_WIN32(GetLastError())); }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, result.data(), size, nullptr, nullptr) != size) {
        throw ProbeFailure("Encode probe evidence", HRESULT_FROM_WIN32(GetLastError()));
    }
    return result;
}

std::string json(std::string_view value) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char unit : value) {
        if (unit == '"' || unit == '\\') { result += '\\'; result += static_cast<char>(unit); }
        else if (unit < 0x20) {
            result += "\\u00";
            result += digits[unit >> 4];
            result += digits[unit & 0xf];
        } else { result += static_cast<char>(unit); }
    }
    return result + '"';
}

std::string json(std::wstring_view value) { return json(utf8(value)); }
std::string hresult(HRESULT result) { return json(std::format("0x{:08X}", static_cast<std::uint32_t>(result))); }

ComPtr<notifications::IToastNotificationHistory> history() {
    ComPtr<notifications::IToastNotificationManagerStatics2> manager;
    check(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(),
        IID_PPV_ARGS(manager.GetAddressOf())), "Get toast manager");
    ComPtr<notifications::IToastNotificationHistory> result;
    check(manager->get_History(result.GetAddressOf()), "Get toast history");
    return result;
}

std::string history_json(bool clear_test_history) {
    const auto first = history();
    if (clear_test_history) {
        // This command is deliberately explicit and can only affect the fixed
        // instrumented identity. Normal Launcher history is never addressed.
        check(first->ClearWithId(HStringReference(test_application_id).Get()), "Clear test notification history");
    }
    ComPtr<notifications::IToastNotificationHistory2> second;
    check(first.As(&second), "Get history enumeration interface");
    ComPtr<ABI::Windows::Foundation::Collections::IVectorView<notifications::ToastNotification*>> items;
    check(second->GetHistoryWithId(HStringReference(test_application_id).Get(), items.GetAddressOf()), "Enumerate test notification history");
    UINT32 count = 0;
    check(items->get_Size(&count), "Read notification count");
    std::string result = "{\"mode\":" + json(clear_test_history ? "clear-test-history" : "history") +
        ",\"application_id\":" + json(std::wstring_view(test_application_id)) + ",\"count\":" + std::to_string(count) + ",\"notifications\":[";
    for (UINT32 index = 0; index < count; ++index) {
        ComPtr<notifications::IToastNotification> toast;
        check(items->GetAt(index, toast.GetAddressOf()), "Read history notification");
        ComPtr<notifications::IToastNotification2> identity;
        check(toast.As(&identity), "Read notification identity");
        HString tag;
        HString group;
        check(identity->get_Tag(tag.GetAddressOf()), "Read notification tag");
        check(identity->get_Group(group.GetAddressOf()), "Read notification group");
        ComPtr<xml::IXmlDocument> content;
        check(toast->get_Content(content.GetAddressOf()), "Read notification content");
        ComPtr<xml::IXmlNodeSerializer> serializer;
        check(content.As(&serializer), "Get notification XML serializer");
        HString text;
        check(serializer->GetXml(text.GetAddressOf()), "Serialize notification XML");
        if (index != 0) { result += ','; }
        result += "{\"tag\":" + json(view(tag)) + ",\"group\":" + json(view(group)) + ",\"xml\":" + json(view(text)) + '}';
    }
    return result + "],\"success\":true}\n";
}

int activate(std::wstring_view argument) {
    const std::filesystem::path path(argument);
    if (!path.is_absolute() || path.native().find(L'\0') != std::wstring::npos ||
        !path.filename().native().ends_with(L".filesystem-errors.log") || !std::filesystem::is_regular_file(path)) {
        throw std::invalid_argument("--activate requires an existing absolute owned *.filesystem-errors.log fixture.");
    }
    ComPtr<INotificationActivationCallback> callback;
    const auto created = CoCreateInstance(test_activator, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(callback.GetAddressOf()));
    HRESULT activated = E_PENDING;
    if (SUCCEEDED(created)) {
        const auto payload = L"open-filesystem-log:" + path.native();
        activated = callback->Activate(test_application_id, payload.c_str(), nullptr, 0);
    }
    const bool success = SUCCEEDED(created) && SUCCEEDED(activated);
    std::cout << "{\"mode\":\"activate\",\"application_id\":" << json(std::wstring_view(test_application_id)) <<
        ",\"path\":" << json(path.native()) << ",\"create_hresult\":" << hresult(created) <<
        ",\"activate_hresult\":" << hresult(activated) << ",\"success\":" << (success ? "true" : "false") << "}\n" << std::flush;
    return success ? 0 : 1;
}

int show(std::wstring_view journal, std::wstring_view server) {
    const std::filesystem::path path(journal);
    const std::filesystem::path executable(server);
    if (!launcher::AppNotification::valid_journal_path(path) || !std::filesystem::is_regular_file(path) ||
            !executable.is_absolute() || executable.filename() != L"LauncherVersionTest.exe" || !std::filesystem::is_regular_file(executable)) {
        throw std::invalid_argument("--show requires an existing owned journal and absolute LauncherVersionTest.exe server.");
    }
    launcher::AppNotification notification;
    const auto registered = notification.register_server(executable, true);
    const auto shown = SUCCEEDED(registered) ? notification.show(path) : E_PENDING;
    const bool success = SUCCEEDED(registered) && SUCCEEDED(shown);
    std::cout << "{\"mode\":\"show\",\"path\":" << json(path.native()) << ",\"register_hresult\":" << hresult(registered) <<
        ",\"show_hresult\":" << hresult(shown) << ",\"success\":" << (success ? "true" : "false") << "}\n" << std::flush;
    return success ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const bool read_history = argc == 1 || (argc == 2 && std::wstring_view(argv[1]) == L"--history");
        const bool clear_history = argc == 2 && std::wstring_view(argv[1]) == L"--clear-test-history";
        const bool activate_test = argc == 3 && std::wstring_view(argv[1]) == L"--activate";
        const bool show_test = argc == 4 && std::wstring_view(argv[1]) == L"--show";
        if (!read_history && !clear_history && !activate_test && !show_test) {
            throw std::invalid_argument("Usage: LauncherAppNotificationProbe [--history | --activate <owned-journal> | --show <owned-journal> <LauncherVersionTest.exe> | --clear-test-history]");
        }
        const RuntimeApartment apartment;
        if (activate_test) { return activate(argv[2]); }
        if (show_test) { return show(argv[2], argv[3]); }
        std::cout << history_json(clear_history) << std::flush;
        return 0;
    } catch (const ProbeFailure& error) {
        std::cout << "{\"success\":false,\"error\":" << json(error.what()) << ",\"hresult\":" << hresult(error.result()) << "}\n" << std::flush;
        return 1;
    } catch (const std::exception& error) {
        std::cout << "{\"success\":false,\"error\":" << json(error.what()) << "}\n" << std::flush;
        return 2;
    }
}
