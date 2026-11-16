#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/app_notification.hpp"

#include <NotificationActivationCallback.h>
#include <roapi.h>
#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace launcher {
namespace {

constexpr wchar_t normal_id[] = L"Dlugosz.Launcher.Filesystem";
constexpr wchar_t test_id[] = L"Dlugosz.Launcher.VersionTest.Filesystem";
constexpr wchar_t normal_clsid[] = L"{DD5845B7-853F-48CF-84D7-9CE70A868A0F}";
constexpr wchar_t test_clsid[] = L"{1EF312F7-3027-4558-966A-D4D213A97425}";
constexpr std::wstring_view activation_prefix = L"open-filesystem-log:";

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;
namespace notifications = ABI::Windows::UI::Notifications;
namespace xml = ABI::Windows::Data::Xml::Dom;

struct HResultFailure final { HRESULT result; };

void check_hresult(HRESULT result) {
    if (FAILED(result)) { throw HResultFailure{result}; }
}

void check_win32(LSTATUS result) { check_hresult(HRESULT_FROM_WIN32(result)); }

// Called only from an active catch. Keep ABI entry points nonthrowing while
// preserving native HRESULTs and allocation failures without a projection SDK.
HRESULT current_exception_result() noexcept {
    try { throw; }
    catch (const HResultFailure& error) { return error.result; }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (const std::invalid_argument&) { return E_INVALIDARG; }
    catch (const std::system_error& error) {
        return error.code().category() == std::system_category() ? HRESULT_FROM_WIN32(error.code().value()) : E_FAIL;
    }
    catch (...) { return E_FAIL; }
}

struct Handle final {
    explicit Handle(HANDLE value = nullptr) noexcept : value(value) {}
    ~Handle() { if (value != nullptr) { CloseHandle(value); } }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE value;
};

struct Inbox final {
    explicit Inbox(HANDLE owner_event) : event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (event.value == nullptr) { throw std::system_error(GetLastError(), std::system_category(), "Cannot create notification activation event"); }
        if (owner_event != nullptr && !DuplicateHandle(GetCurrentProcess(), owner_event, GetCurrentProcess(), &wake.value, EVENT_MODIFY_STATE, FALSE, 0)) {
            throw std::system_error(GetLastError(), std::system_category(), "Cannot retain notification owner event");
        }
    }
    Handle event;
    Handle wake;
    std::mutex mutex;
    std::optional<std::filesystem::path> pending;
    bool enabled = true;
    std::wstring app_id;
};

class Activator final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, INotificationActivationCallback> {
public:
    explicit Activator(std::shared_ptr<Inbox> inbox) : inbox_(std::move(inbox)) {}
    HRESULT STDMETHODCALLTYPE Activate(LPCWSTR app_id, LPCWSTR arguments, const NOTIFICATION_USER_INPUT_DATA*, ULONG) noexcept override {
        try {
            if (app_id == nullptr || arguments == nullptr) { return E_INVALIDARG; }
            const std::wstring_view text(arguments);
            if (!text.starts_with(activation_prefix)) { return E_INVALIDARG; }
            const std::filesystem::path path(text.substr(activation_prefix.size()));
            if (!AppNotification::valid_journal_path(path)) { return E_INVALIDARG; }
            const std::lock_guard lock(inbox_->mutex);
            if (!inbox_->enabled || inbox_->app_id != app_id) { return E_ACCESSDENIED; }
            inbox_->pending = path;
            SetEvent(inbox_->event.value);
            if (inbox_->wake.value != nullptr) { SetEvent(inbox_->wake.value); }
            return S_OK;
        } catch (...) { return current_exception_result(); }
    }
private:
    std::shared_ptr<Inbox> inbox_;
};

class ActivatorFactory final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IClassFactory> {
public:
    explicit ActivatorFactory(std::shared_ptr<Inbox> inbox) : inbox_(std::move(inbox)) {}
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** result) noexcept override {
        if (result == nullptr) { return E_POINTER; }
        *result = nullptr;
        if (outer != nullptr) { return CLASS_E_NOAGGREGATION; }
        try {
            const auto activator = Microsoft::WRL::Make<Activator>(inbox_);
            return activator ? activator->QueryInterface(iid, result) : E_OUTOFMEMORY;
        } catch (...) { return current_exception_result(); }
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override { return S_OK; }
private:
    std::shared_ptr<Inbox> inbox_;
};

void set_registry_string(std::wstring_view key_name, const wchar_t* value_name, std::wstring_view value) {
    HKEY key = nullptr;
    check_win32(RegCreateKeyExW(HKEY_CURRENT_USER, std::wstring(key_name).c_str(), 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr));
    struct KeyOwner final { HKEY key; ~KeyOwner() { RegCloseKey(key); } } owner{key};
    const std::wstring terminated(value);
    check_win32(RegSetValueExW(key, value_name, 0, REG_SZ, reinterpret_cast<const BYTE*>(terminated.c_str()),
        static_cast<DWORD>((terminated.size() + 1) * sizeof(wchar_t))));
}

std::wstring xml_attribute(std::wstring_view value) {
    std::wstring result;
    for (const wchar_t unit : value) {
        switch (unit) {
        case L'&': result += L"&amp;"; break;
        case L'\"': result += L"&quot;"; break;
        case L'<': result += L"&lt;"; break;
        case L'>': result += L"&gt;"; break;
        case L'\'': result += L"&apos;"; break;
        default: result += unit; break;
        }
    }
    return result;
}

} // namespace

struct AppNotification::Impl final {
    explicit Impl(HANDLE owner_event) : inbox(std::make_shared<Inbox>(owner_event)) {}
    ~Impl() {
        { const std::lock_guard lock(inbox->mutex); inbox->enabled = false; inbox->pending.reset(); }
        if (registration != 0) { CoRevokeClassObject(registration); }
        if (runtime_initialized) { RoUninitialize(); }
    }
    std::shared_ptr<Inbox> inbox;
    DWORD registration = 0;
    bool runtime_initialized = false;
    std::wstring app_id;
};

AppNotification::AppNotification(HANDLE owner_completion_event) : impl_(std::make_unique<Impl>(owner_completion_event)) {}
AppNotification::~AppNotification() = default;

HRESULT AppNotification::register_server(const std::filesystem::path& executable, bool test_identity) noexcept {
    try {
        if (impl_->registration != 0) { return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED); }
        if (!executable.is_absolute() || executable.native().find_first_of(L"\"\r\n") != std::wstring::npos ||
                executable.native().find(L'\0') != std::wstring::npos) { return E_INVALIDARG; }
        if (!impl_->runtime_initialized) {
            const HRESULT initialized = RoInitialize(RO_INIT_SINGLETHREADED);
            if (FAILED(initialized)) { return initialized; }
            impl_->runtime_initialized = true;
        }
        impl_->app_id = test_identity ? test_id : normal_id;
        const std::wstring clsid_text = test_identity ? test_clsid : normal_clsid;
        CLSID clsid{};
        check_hresult(CLSIDFromString(clsid_text.c_str(), &clsid));
        const std::wstring server = L"\"" + executable.native() + L"\" --notification-activated";
        if (server.size() >= 32767) { return E_INVALIDARG; }
        // Microsoft desktop notification compatibility registration uses these
        // per-user keys; no Start-menu shortcut or package identity is required.
        set_registry_string(L"Software\\Classes\\CLSID\\" + clsid_text + L"\\LocalServer32", nullptr, server);
        const std::wstring identity_key = L"Software\\Classes\\AppUserModelId\\" + impl_->app_id;
        set_registry_string(identity_key, L"DisplayName", test_identity ? L"Launcher filesystem test" : L"Długosz Launcher");
        set_registry_string(identity_key, L"CustomActivator", clsid_text);
        { const std::lock_guard lock(impl_->inbox->mutex); impl_->inbox->app_id = impl_->app_id; }
        const auto factory = Microsoft::WRL::Make<ActivatorFactory>(impl_->inbox);
        if (!factory) { return E_OUTOFMEMORY; }
        return CoRegisterClassObject(clsid, factory.Get(), CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE, &impl_->registration);
    } catch (...) { return current_exception_result(); }
}

bool AppNotification::valid_journal_path(const std::filesystem::path& path) noexcept {
    try {
        const auto& text = path.native();
        constexpr std::wstring_view suffix = L".filesystem-errors.log";
        if (!path.is_absolute() || text.empty() || text.size() > 32767 || text.find(L'\0') != std::wstring::npos ||
                std::ranges::any_of(text, [](wchar_t unit) { return unit < 0x20; }) || text.size() < suffix.size()) { return false; }
        const std::wstring_view name = text.starts_with(L"\\\\?\\") ? std::wstring_view(text).substr(4) : std::wstring_view(text);
        if (name.find_first_of(L"\"<>|?*") != std::wstring_view::npos) { return false; }
        return CompareStringOrdinal(text.data() + text.size() - suffix.size(), static_cast<int>(suffix.size()),
            suffix.data(), static_cast<int>(suffix.size()), TRUE) == CSTR_EQUAL;
    } catch (...) { return false; }
}

HRESULT AppNotification::show(const std::filesystem::path& journal) noexcept {
    try {
        if (impl_->registration == 0 || !valid_journal_path(journal)) { return E_INVALIDARG; }
        const std::wstring argument = std::wstring(activation_prefix) + journal.native();
        const std::wstring content = L"<toast launch=\"" + xml_attribute(argument) + L"\"><visual><binding template=\"ToastGeneric\">"
            L"<text>Filesystem errors found</text><text>Launcher could not read a file or directory. Click to open the error log for full paths and Windows error details.</text>"
            L"</binding></visual><audio silent=\"true\"/></toast>";
        ComPtr<IInspectable> instance;
        check_hresult(RoActivateInstance(HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(), instance.GetAddressOf()));
        ComPtr<xml::IXmlDocument> document;
        check_hresult(instance.As(&document));
        ComPtr<xml::IXmlDocumentIO> document_io;
        check_hresult(document.As(&document_io));
        HString payload;
        check_hresult(payload.Set(content.c_str(), static_cast<UINT32>(content.size())));
        check_hresult(document_io->LoadXml(payload.Get()));
        ComPtr<notifications::IToastNotificationFactory> factory;
        check_hresult(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotification).Get(),
            IID_PPV_ARGS(factory.GetAddressOf())));
        ComPtr<notifications::IToastNotification> notification;
        check_hresult(factory->CreateToastNotification(document.Get(), notification.GetAddressOf()));
        ComPtr<notifications::IToastNotification2> identity;
        check_hresult(notification.As(&identity));
        check_hresult(identity->put_Tag(HStringReference(L"fs-errors").Get()));
        check_hresult(identity->put_Group(HStringReference(L"filesystem").Get()));
        ComPtr<notifications::IToastNotificationManagerStatics> manager;
        check_hresult(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(),
            IID_PPV_ARGS(manager.GetAddressOf())));
        ComPtr<notifications::IToastNotifier> notifier;
        check_hresult(manager->CreateToastNotifierWithId(HStringReference(impl_->app_id.c_str()).Get(), notifier.GetAddressOf()));
        return notifier->Show(notification.Get());
    } catch (...) { return current_exception_result(); }
}

std::optional<std::filesystem::path> AppNotification::take_activation() noexcept {
    try {
        const std::lock_guard lock(impl_->inbox->mutex);
        auto result = std::move(impl_->inbox->pending);
        impl_->inbox->pending.reset();
        ResetEvent(impl_->inbox->event.value);
        return result;
    } catch (...) { return {}; }
}

HANDLE AppNotification::activation_event() const noexcept { return impl_->inbox->event.value; }

} // namespace launcher
