#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#if defined(LAUNCHER_USE_JMD_MODULES)
import jmd.updater;
#endif
#include "launcher/seenthemall.hh"
#endif

#include "updater_integration.hpp"
#include "launcher/file_lookup.hpp"
#include "launcher/launch.hpp"
#include "launcher_resource.h"
#include "win32/window_text_reply.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace launcher {
namespace {
namespace fs = std::filesystem;

constexpr wchar_t launcher_update_state[] = L"Software\\Długosz\\Updater\\launcher";
constexpr wchar_t automatic_check_day[] = L"LastSuccessfulAutomaticCheckDay";
constexpr wchar_t automatic_retry_after[] = L"AutomaticRetryAfterUtc";
constexpr wchar_t updater_key[] = L"Software\\Długosz\\Updater\\Client";
constexpr wchar_t launcher_portable_page[] = L"https://updates.dlugosz.enterprises/downloads/launcher/";
constexpr wchar_t updater_information_page[] = LAUNCHER_UPDATER_INFO_URL;
constexpr UINT milliseconds_per_hour = 60 * 60 * 1000;
constexpr UINT normal_check_delay = 23 * milliseconds_per_hour + milliseconds_per_hour / 2;
constexpr ULONGLONG ticks_per_millisecond = 10'000;
constexpr ULONGLONG ticks_per_hour = milliseconds_per_hour * ticks_per_millisecond;
constexpr ULONGLONG ticks_per_day = 24 * ticks_per_hour;
constexpr ULONGLONG normal_check_ticks = normal_check_delay * ticks_per_millisecond;
constexpr ULONGLONG review_retry_ticks = 5 * ticks_per_hour / 60;

using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;

std::wstring registry_string(HKEY root, const wchar_t* key, const wchar_t* name) {
    constexpr DWORD flags = RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY;
    DWORD bytes = 0;
    if (RegGetValueW(root, key, name, flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
            bytes < sizeof(wchar_t) || bytes > 65536 || bytes % sizeof(wchar_t) != 0) {
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, name, flags, nullptr, value.data(), &bytes) != ERROR_SUCCESS ||
            bytes < sizeof(wchar_t) || bytes % sizeof(wchar_t) != 0) {
        return {};
    }
    value.resize(bytes / sizeof(wchar_t));
    if (value.back() != L'\0') { return {}; }
    value.pop_back();
    return value.find(L'\0') == value.npos ? value : std::wstring{};
}

constexpr bool version_at_least(std::wstring_view version, const std::array<unsigned int, 4>& minimum) noexcept {
    std::array<unsigned int, 4> parts{};
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const auto separator = version.find(L'.');
        const auto part = version.substr(0, separator);
        if (part.empty() || part.size() > 5) { return false; }
        for (const auto digit : part) {
            if (digit < L'0' || digit > L'9') { return false; }
            parts[index] = parts[index] * 10 + static_cast<unsigned int>(digit - L'0');
        }
        if (parts[index] > 65535) { return false; }
        if (separator == version.npos) { return index >= 2 && parts >= minimum; }
        version.remove_prefix(separator + 1);
    }
    return false;
}

bool supports_review_commands(bool machine_scope) {
    return version_at_least(registry_string(machine_scope ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, updater_key, L"Version"),
        {1, 0, 20, 0});
}

Handle start_process(const fs::path& executable, std::wstring_view arguments, DWORD flags = 0, wchar_t* environment = nullptr,
        bool grant_foreground = false) {
    auto command = win32::quote_command_argument(executable.native()) + L" ";
    command += arguments;
    const auto directory = executable.parent_path();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            flags, environment, directory.c_str(), &startup, &child)) {
        return Handle(nullptr, CloseHandle);
    }
    if (grant_foreground) { AllowSetForegroundWindow(child.dwProcessId); }
    CloseHandle(child.hThread);
    return Handle(child.hProcess, CloseHandle);
}

Handle start_review_command(const fs::path& executable, bool machine_scope, std::wstring_view option) {
    auto arguments = std::wstring(L"Launcher ") + std::wstring(option) + L" --scope ";
    arguments += machine_scope ? L"machine" : L"user";
    return start_process(executable, arguments);
}

bool open_page(HWND owner, const wchar_t* url) {
    NativeLaunchAction action;
    action.target = url;
    return launch_prepared_action(action, reinterpret_cast<std::uintptr_t>(owner)).success;
}

#if defined(LAUNCHER_USE_JMD_MODULES) && !defined(__INTELLISENSE__)
constexpr jmd::UpdaterProduct updater_product{
    .install_directory = L"Launcher",
    .suite = L"Launcher",
    .display_name = L"Launcher",
    .portable_page = launcher_portable_page,
    .information_page = updater_information_page,
    .open_page = open_page,
    .bootstrap_resource = LAUNCHER_UPDATER_BOOTSTRAP_ENABLED ? IDR_UPDATER_BOOTSTRAP : 0,
};
#else
constexpr wchar_t bootstrap_name[] = L"UpdaterBootstrap.ps1";
constexpr wchar_t bootstrap_anchor_property[] = L"Dlugosz.Launcher.UpdaterBootstrapAnchor";
constexpr DWORD bootstrap_cancelled = 3;

struct InstallScope {
    fs::path programs;
    HKEY registry;
    std::wstring_view argument;
};

struct RegisteredUpdater {
    fs::path executable;
    bool supports_update_check_options = false;
};

struct BootstrapRequest {
    InstallScope scope;
    HWND owner = nullptr;

    HWND anchor() const noexcept {
        return owner && GetPropW(owner, bootstrap_anchor_property) == this ? owner : nullptr;
    }

    ~BootstrapRequest() {
        if (const auto window = anchor()) { RemovePropW(window, bootstrap_anchor_property); }
    }
};

struct StagedBootstrap {
    fs::path directory;
    fs::path script;
    fs::path installer;

    ~StagedBootstrap() {
        if (directory.empty()) { return; }
        DeleteFileW(script.c_str());
        DeleteFileW(installer.c_str());
        RemoveDirectoryW(directory.c_str());
    }
};

std::atomic_flag bootstrap_pending;

fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR text = nullptr;
    const auto result = SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &text);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> storage(text, CoTaskMemFree);
    return SUCCEEDED(result) && text ? fs::path(text) : fs::path{};
}

bool equal_ordinal(std::wstring_view left, std::wstring_view right) noexcept {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool same_directory(const fs::path& left, const fs::path& right) {
    if (equal_ordinal(left.native(), right.native())) { return true; }
    if (GetDriveTypeW(left.root_path().c_str()) == DRIVE_REMOTE || GetDriveTypeW(right.root_path().c_str()) == DRIVE_REMOTE) {
        return false;
    }
    std::error_code error;
    return fs::equivalent(left, right, error) && !error;
}

InstallScope installed_scope(const fs::path& launcher_directory) {
    const auto machine = known_folder(FOLDERID_ProgramFiles);
    if (!machine.empty() && same_directory(launcher_directory, machine / L"Długosz" / L"Launcher")) {
        return {machine, HKEY_LOCAL_MACHINE, L"machine"};
    }
    const auto user = known_folder(FOLDERID_LocalAppData);
    if (!user.empty()) {
        const auto programs = user / L"Programs";
        if (same_directory(launcher_directory, programs / L"Długosz" / L"Launcher")) {
            return {programs, HKEY_CURRENT_USER, L"user"};
        }
    }
    return {};
}

bool normal_path(const fs::path& path, bool directory) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
        ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
}

RegisteredUpdater registered_updater(const InstallScope& scope) {
    const auto publisher = scope.programs / L"Długosz";
    const auto directory = publisher / L"Updater";
    const auto executable = directory / L"Updater.exe";
    const auto registered = fs::path(registry_string(scope.registry, updater_key, L"ExecutablePath"));
    const auto version = registry_string(scope.registry, updater_key, L"Version");
    if (!registered.is_absolute() || !equal_ordinal(registered.lexically_normal().native(), executable.native()) ||
            version.empty() || !normal_path(publisher, true) ||
            !normal_path(directory, true) || !normal_path(executable, false)) {
        return {};
    }
    return {executable, version_at_least(version, {1, 0, 2, 0})};
}

bool elevated_or_unknown() noexcept {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) { return true; }
    const Handle owner(token, CloseHandle);
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    return !GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes) || elevation.TokenIsElevated != 0;
}

DWORD about_process_id(HWND owner) noexcept {
    if (!owner || !IsWindowVisible(owner)) { return 0; }
    DWORD process_id = 0;
    GetWindowThreadProcessId(owner, &process_id);
    return process_id == GetCurrentProcessId() ? process_id : 0;
}

bool start_updater(const RegisteredUpdater& updater, const InstallScope& scope, bool manual, HWND owner = nullptr) {
    if (updater.executable.empty() || (!manual && !updater.supports_update_check_options)) { return false; }
    auto arguments = std::wstring(manual ? L"Launcher --check --scope " : L"Launcher --scope ") + std::wstring(scope.argument);
    const auto process_id = manual ? about_process_id(owner) : 0;
    if (process_id && updater.supports_update_check_options) {
        arguments += L" --anchor " + std::to_wstring(reinterpret_cast<UINT_PTR>(owner)) + L" " + std::to_wstring(process_id);
    }
    return start_process(updater.executable, arguments, 0, nullptr, process_id && GetForegroundWindow() == owner) != nullptr;
}

bool stage_bootstrap(StagedBootstrap& staged) {
    const HRSRC source = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_UPDATER_BOOTSTRAP), RT_RCDATA);
    if (!source) { return false; }
    const DWORD size = SizeofResource(nullptr, source);
    const HGLOBAL loaded = LoadResource(nullptr, source);
    const auto* content = loaded ? LockResource(loaded) : nullptr;
    if (!size || !content) { return false; }

    wchar_t temporary[32768];
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
    GUID unique{};
    wchar_t identifier[39];
    constexpr int identifier_size = static_cast<int>(std::size(identifier));
    if (!length || length >= std::size(temporary) || FAILED(CoCreateGuid(&unique)) ||
            StringFromGUID2(unique, identifier, identifier_size) != identifier_size) {
        return false;
    }
    auto directory = fs::path(temporary) / (L"Dlugosz-Updater-" + std::wstring(identifier + 1, 36));
    auto script = directory / bootstrap_name;
    auto installer = directory / L"SharedUpdaterClient-Setup.exe";
    if (!CreateDirectoryW(directory.c_str(), nullptr)) { return false; }
    staged.directory = std::move(directory);
    staged.script = std::move(script);
    staged.installer = std::move(installer);

    const HANDLE file = CreateFileW(staged.script.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { return false; }
    const Handle storage(file, CloseHandle);
    DWORD written = 0;
    return WriteFile(file, content, size, &written, nullptr) && written == size;
}

std::wstring bootstrap_environment(const fs::path& script, const fs::path& modules) {
    const std::unique_ptr<wchar_t, decltype(&FreeEnvironmentStringsW)> inherited(GetEnvironmentStringsW(), FreeEnvironmentStringsW);
    if (!inherited) { return {}; }
    std::vector<std::wstring> values;
    for (const wchar_t* entry = inherited.get(); *entry;) {
        const std::wstring_view value(entry);
        entry += value.size() + 1;
        const auto name = value.substr(0, value.find(L'=', 1));
        if (!equal_ordinal(name, L"LAUNCHER_UPDATER_SCRIPT") && !equal_ordinal(name, L"PSModulePath")) {
            values.emplace_back(value);
        }
    }
    values.emplace_back(L"LAUNCHER_UPDATER_SCRIPT=" + script.native());
    values.emplace_back(L"PSModulePath=" + modules.native());
    std::ranges::sort(values, [](const std::wstring& left, const std::wstring& right) {
        return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::wstring block;
    for (const auto& value : values) {
        block += value;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

Handle start_bootstrap(const fs::path& script) {
    wchar_t system[32768];
    const UINT length = GetSystemDirectoryW(system, static_cast<UINT>(std::size(system)));
    if (length == 0 || length >= std::size(system)) { return Handle(nullptr, CloseHandle); }
    const auto inbox = fs::path(system) / L"WindowsPowerShell" / L"v1.0";
    auto environment = bootstrap_environment(script, inbox / L"Modules");
    if (environment.empty()) { return Handle(nullptr, CloseHandle); }
    constexpr std::wstring_view arguments = L"-NoLogo -NoProfile -NonInteractive -ExecutionPolicy RemoteSigned "
        L"-Command \"$ErrorActionPreference = 'Stop'; try { & $env:LAUNCHER_UPDATER_SCRIPT; "
        L"exit $LASTEXITCODE } catch [System.Management.Automation.PSSecurityException] { exit 12 } catch { exit 1 }\"";
    return start_process(inbox / L"powershell.exe", arguments, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data());
}

void CALLBACK finish_bootstrap(PTP_CALLBACK_INSTANCE, void* context) noexcept {
    const struct PendingReset { ~PendingReset() { bootstrap_pending.clear(); } } pending;
    const std::unique_ptr<BootstrapRequest> request(static_cast<BootstrapRequest*>(context));
    try {
        StagedBootstrap staged;
        const auto process = stage_bootstrap(staged) ? start_bootstrap(staged.script) : Handle(nullptr, CloseHandle);
        DWORD status = 1;
        if (process && WaitForSingleObject(process.get(), INFINITE) == WAIT_OBJECT_0 && GetExitCodeProcess(process.get(), &status)) {
            if (status == bootstrap_cancelled ||
                    (status == 0 && start_updater(registered_updater(request->scope), request->scope, true, request->anchor()))) {
                return;
            }
        }
        if (!open_page(nullptr, updater_information_page)) {
            MessageBoxW(nullptr, L"Windows could not open the Shared Updater Client information page.", L"Launcher", MB_OK | MB_ICONINFORMATION);
        }
    } catch (...) {
        MessageBoxW(nullptr, L"Windows could not complete the updater setup request.", L"Launcher", MB_OK | MB_ICONINFORMATION);
    }
}
#endif

ULONGLONG current_filetime() noexcept {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    return (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
}

ULONGLONG automatic_retry() noexcept {
    ULONGLONG retry = 0;
    DWORD bytes = sizeof(retry);
    constexpr DWORD flags = RRF_RT_REG_QWORD | RRF_SUBKEY_WOW6464KEY;
    return RegGetValueW(HKEY_CURRENT_USER, launcher_update_state, automatic_retry_after, flags, nullptr, &retry, &bytes) == ERROR_SUCCESS &&
        bytes == sizeof(retry) ? retry : 0;
}

ULONGLONG review_reminder(const wchar_t* name) noexcept try {
    if (!name) { return 0; }
    const auto value = registry_string(HKEY_CURRENT_USER, launcher_update_state, name);
    const auto separator = value.find(L'|');
    if (separator == value.npos || separator == 0 || separator + 1 == value.size()) { return 0; }
    ULONGLONG deadline = 0;
    for (const auto digit : std::wstring_view(value).substr(0, separator)) {
        if (digit < L'0' || digit > L'9') { return 0; }
        const auto number = static_cast<ULONGLONG>(digit - L'0');
        if (deadline > (std::numeric_limits<ULONGLONG>::max() - number) / 10) { return 0; }
        deadline = deadline * 10 + number;
    }
    return deadline;
} catch (...) {
    return 0;
}

constexpr UINT retry_delay(ULONGLONG deadline, ULONGLONG now) noexcept {
    if (deadline <= now || deadline - now > ticks_per_day) { return 0; }
    return static_cast<UINT>((deadline - now + ticks_per_millisecond - 1) / ticks_per_millisecond);
}

constexpr ULONGLONG next_update_wake(ULONGLONG feed, ULONGLONG review, ULONGLONG review_retry, ULONGLONG now) noexcept {
    if (!review || (review > now && review - now > ticks_per_day)) { return feed; }
    return std::min(feed, std::max(review, review_retry));
}

constexpr ULONGLONG retry_deadline(ULONGLONG retry, ULONGLONG now, bool newly_recorded) noexcept {
    if (!retry || (retry > now && !retry_delay(retry, now))) { return 0; }
    // Older Updaters recorded a 15–30 minute delay. A newly observed failure
    // still gets the full hour. A deadline already due can run on the next tick.
    return newly_recorded && retry > now ? std::max(retry, now + ticks_per_hour) : retry;
}

static_assert(retry_delay(retry_deadline(ticks_per_hour / 2 + 1, 1, true), 1) == milliseconds_per_hour);
static_assert(retry_delay(retry_deadline(2 * ticks_per_hour + 1, 1, true), 1) == 2 * milliseconds_per_hour);
static_assert(retry_delay(retry_deadline(ticks_per_hour / 2 + 1, 1, false), 1) == milliseconds_per_hour / 2);
static_assert(retry_deadline(ticks_per_day + 2, 1, true) == 0 && retry_deadline(0, 1, true) == 0);
static_assert(retry_delay(2, 1) == 1);
static_assert(next_update_wake(100, 90, 95, 10) == 95);
static_assert(next_update_wake(100, 90, 120, 10) == 100);
static_assert(next_update_wake(100, 0, 0, 10) == 100);

} // namespace

struct AutomaticUpdateSchedule::Watch {
    Watch(HWND window, UINT message) noexcept : window(window), message(message) {}

    ~Watch() {
        if (wait) {
            SetThreadpoolWait(wait, nullptr, nullptr);
            WaitForThreadpoolWaitCallbacks(wait, TRUE);
            CloseThreadpoolWait(wait);
        }
        if (key) { RegCloseKey(key); }
    }

    bool start() noexcept {
        changed.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!changed) { return false; }
        wait = CreateThreadpoolWait(notify, this, nullptr);
        return wait && open() && arm();
    }

    bool arm() noexcept {
        constexpr DWORD filter = REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC;
        auto result = RegNotifyChangeKeyValue(key, FALSE, filter, changed.get(), TRUE);
        if (result == ERROR_KEY_DELETED) {
            RegCloseKey(std::exchange(key, nullptr));
            ResetEvent(changed.get());
            if (!open()) { return false; }
            result = RegNotifyChangeKeyValue(key, FALSE, filter, changed.get(), TRUE);
        }
        if (result != ERROR_SUCCESS) { return false; }
        SetThreadpoolWait(wait, changed.get(), nullptr);
        return true;
    }

private:
    bool open() noexcept {
        // A fresh installation has no per-user result until its first check.
        // Create only the empty key so that result can be observed immediately.
        return RegCreateKeyExW(HKEY_CURRENT_USER, launcher_update_state, 0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_NOTIFY | KEY_WOW64_64KEY, nullptr, &key, nullptr) == ERROR_SUCCESS;
    }

    static void CALLBACK notify(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT) noexcept {
        const auto& watch = *static_cast<const Watch*>(context);
        PostMessageW(watch.window, watch.message, 0, 0);
    }

    const HWND window;
    const UINT message;
    Handle changed{nullptr, CloseHandle};
    HKEY key = nullptr;
    PTP_WAIT wait = nullptr;
};

AutomaticUpdateSchedule::AutomaticUpdateSchedule() noexcept = default;
AutomaticUpdateSchedule::~AutomaticUpdateSchedule() = default;

bool AutomaticUpdateSchedule::refresh_retry(ULONGLONG now) noexcept {
    const auto retry = automatic_retry();
    if (retry_initialized_ && retry == observed_retry_) { return false; }
    retry_deadline_ = retry_deadline(retry, now, retry_initialized_);
    observed_retry_ = retry;
    retry_initialized_ = true;
    return true;
}

void AutomaticUpdateSchedule::schedule_next(HWND window, UINT_PTR timer, ULONGLONG now) const noexcept {
    const auto wake = next_update_wake(feed_wake_, review_reminder(review_reminder_value_), review_retry_after_, now);
    const auto delay = wake <= now ? 1 : retry_delay(wake, now);
    SetTimer(window, timer, delay ? delay : normal_check_delay, nullptr);
}

void AutomaticUpdateSchedule::check(HWND window, UINT_PTR timer, UINT retry_changed_message, const FileLookup& files) noexcept try {
    SetTimer(window, timer, normal_check_delay, nullptr);
    review_reminder_value_ = nullptr;
    if (files.portable()) { return; }
#if defined(LAUNCHER_USE_JMD_MODULES) && !defined(__INTELLISENSE__)
    const auto updater = jmd::find_automatic_updater(files.executable(), updater_product.install_directory);
    if (updater.executable.empty()) { return; }
    const auto machine_scope = updater.machine_scope;
#else
    const auto scope = installed_scope(files.executable().parent_path());
    if (scope.programs.empty() || elevated_or_unknown()) { return; }
    const auto updater = registered_updater(scope);
    if (updater.executable.empty() || !updater.supports_update_check_options) { return; }
    const auto machine_scope = scope.registry == HKEY_LOCAL_MACHINE;
#endif

    if (supports_review_commands(machine_scope)) {
        review_reminder_value_ = machine_scope ? L"ReviewReminder.AllUsers" : L"ReviewReminder.JustMe";
    }
    if (!watch_) {
        auto watch = std::make_unique<Watch>(window, retry_changed_message);
        if (watch->start()) { watch_ = std::move(watch); }
    }
    const auto now = current_filetime();
    const bool first_check = !retry_initialized_;
    if (refresh_retry(now)) {
        if (retry_deadline_) { feed_wake_ = retry_deadline_; }
        else if (!first_check) { feed_wake_ = now + normal_check_ticks; }
    }
    if (feed_wake_ <= now || feed_wake_ - now > ticks_per_day) {
        feed_wake_ = now + normal_check_ticks;
        review_retry_after_ = now + review_retry_ticks;
#if defined(LAUNCHER_USE_JMD_MODULES) && !defined(__INTELLISENSE__)
        jmd::start_automatic_update_check(updater, updater_product.suite);
#else
        start_updater(updater, scope, false);
#endif
    } else if (review_reminder_value_ && review_retry_after_ <= now) {
        const auto reminder = review_reminder(review_reminder_value_);
        if (first_check || (reminder && reminder <= now)) {
            // Another Updater session can hold the suite lock. Give it time
            // to finish before retrying an unchanged reminder deadline.
            review_retry_after_ = now + review_retry_ticks;
            start_review_command(updater.executable, machine_scope, L"--review-reminder");
        }
    }
    schedule_next(window, timer, now);
} catch (...) {
}

void AutomaticUpdateSchedule::retry_changed(HWND window, UINT_PTR timer) noexcept {
    if (!watch_) { return; }
    // Register the next notification before reading the value to avoid missing a write.
    if (!watch_->arm()) { watch_.reset(); }
    const auto now = current_filetime();
    if (refresh_retry(now)) {
        feed_wake_ = retry_deadline_ ? retry_deadline_ : now + normal_check_ticks;
    }
    schedule_next(window, timer, now);
}

void AutomaticUpdateSchedule::stop() noexcept {
    watch_.reset();
}

bool check_for_updates(HWND owner, const FileLookup& files) noexcept try {
#if defined(LAUNCHER_USE_JMD_MODULES) && !defined(__INTELLISENSE__)
    return jmd::check_for_updates(updater_product, files.executable(), files.portable(), owner);
#else
    if (files.portable()) { return open_page(owner, launcher_portable_page); }
    const auto directory = files.executable().parent_path();
    auto scope = installed_scope(directory);
    if (scope.programs.empty()) { return open_page(owner, launcher_portable_page); }

    const bool elevated = elevated_or_unknown();
    if (elevated && scope.registry == HKEY_CURRENT_USER) { return open_page(owner, updater_information_page); }
    const auto updater = registered_updater(scope);
    if (!updater.executable.empty()) {
        return start_updater(updater, scope, true, owner) || open_page(owner, updater_information_page);
    }

    if (elevated || !LAUNCHER_UPDATER_BOOTSTRAP_ENABLED) { return open_page(owner, updater_information_page); }
    if (bootstrap_pending.test_and_set()) { return true; }
    const auto prompt = scope.registry == HKEY_LOCAL_MACHINE
        ? L"Download the Shared Updater Client and open its setup program? Choose All users in Setup."
        : L"Download the Shared Updater Client and open its setup program? Choose Just me in Setup.";
    if (MessageBoxW(owner, prompt, L"Launcher",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        bootstrap_pending.clear();
        return true;
    }
    try {
        auto* request = new BootstrapRequest{std::move(scope)};
        if (about_process_id(owner) && SetPropW(owner, bootstrap_anchor_property, request)) { request->owner = owner; }
        if (TrySubmitThreadpoolCallback(finish_bootstrap, request, nullptr)) { return true; }
        delete request;
    } catch (...) {
        bootstrap_pending.clear();
        throw;
    }
    bootstrap_pending.clear();
    return open_page(owner, updater_information_page);
#endif
} catch (...) {
    return false;
}

HANDLE start_review_status_query(const FileLookup& files) noexcept try {
    if (files.portable()) { return nullptr; }
#if defined(LAUNCHER_USE_JMD_MODULES) && !defined(__INTELLISENSE__)
    const auto updater = jmd::find_automatic_updater(files.executable(), updater_product.install_directory);
    const auto machine_scope = updater.machine_scope;
#else
    const auto scope = installed_scope(files.executable().parent_path());
    if (scope.programs.empty() || elevated_or_unknown()) { return nullptr; }
    const auto updater = registered_updater(scope);
    const auto machine_scope = scope.registry == HKEY_LOCAL_MACHINE;
#endif
    if (updater.executable.empty() || !supports_review_commands(machine_scope)) { return nullptr; }
    return start_review_command(updater.executable, machine_scope, L"--review-status").release();
} catch (...) {
    return nullptr;
}

std::wstring last_automatic_check_text() {
    constexpr DWORD flags = RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY;
    DWORD checked = 0;
    DWORD bytes = sizeof(checked);
    if (RegGetValueW(HKEY_CURRENT_USER, launcher_update_state, automatic_check_day, flags, nullptr, &checked, &bytes) != ERROR_SUCCESS ||
            bytes != sizeof(checked) || checked == 0) {
        return L"Last checked for updates: never";
    }
    const auto today = current_filetime() / ticks_per_day;
    if (checked > today) { return L"Last checked for updates: date unavailable"; }
    const auto age = today - checked;
    if (age == 0) { return L"Last checked for updates: today"; }
    if (age == 1) { return L"Last checked for updates: 1 day ago"; }
    return L"Last checked for updates: " + std::to_wstring(age) + L" days ago";
}

} // namespace launcher
