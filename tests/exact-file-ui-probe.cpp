#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/launch.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

std::string utf8(std::wstring_view value) {
    if (value.empty()) { return {}; }
    const auto length = static_cast<int>(value.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, nullptr, 0, nullptr, nullptr);
    if (!size) { throw std::runtime_error("Cannot encode probe evidence"); }
    std::string result(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, result.data(), size, nullptr, nullptr)) {
        throw std::runtime_error("Cannot encode probe evidence");
    }
    return result;
}

std::string json(std::string_view value) {
    std::string result = "\"";
    for (const unsigned char unit : value) {
        if (unit == '\\' || unit == '"') { result += '\\'; result += static_cast<char>(unit); }
        else if (unit == '\n') { result += "\\n"; }
        else if (unit == '\r') { result += "\\r"; }
        else if (unit == '\t') { result += "\\t"; }
        else if (unit < 32) { result += '?'; }
        else { result += static_cast<char>(unit); }
    }
    return result + '"';
}

std::string json(std::wstring_view value) { return json(utf8(value)); }
const char* boolean(bool value) { return value ? "true" : "false"; }

bool same_path(std::wstring_view left, std::wstring_view right) {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

void pump(DWORD milliseconds = 25) {
    MsgWaitForMultipleObjectsEx(0, nullptr, milliseconds, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    MSG message{};
    for (unsigned count = 0; count < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

std::wstring window_text(HWND window) {
    DWORD_PTR length = 0;
    if (!SendMessageTimeoutW(window, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &length) || length > 32'768) { return {}; }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    DWORD_PTR copied = 0;
    if (!SendMessageTimeoutW(window, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()),
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &copied)) { return {}; }
    text.resize((std::min)(static_cast<std::size_t>(copied), text.size() - 1));
    return text;
}

bool responsive(HWND window) {
    DWORD_PTR unused = 0;
    return IsWindow(window) && SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &unused) != 0;
}

struct OwnedDialog final {
    std::wstring token;
    std::wstring directory;
    HWND window = nullptr;
    std::wstring title;
    std::vector<std::wstring> text;
};

BOOL CALLBACK child_text(HWND window, LPARAM parameter) {
    auto& text = *reinterpret_cast<std::vector<std::wstring>*>(parameter);
    if (auto value = window_text(window); !value.empty()) { text.push_back(std::move(value)); }
    return text.size() < 128;
}

BOOL CALLBACK find_dialog(HWND window, LPARAM parameter) {
    auto& match = *reinterpret_cast<OwnedDialog*>(parameter);
    wchar_t type[64]{};
    if (!IsWindowVisible(window) || !GetClassNameW(window, type, 64) || std::wstring_view(type) != L"#32770") { return TRUE; }
    auto title = window_text(window);
    if (title.find(match.token) == std::wstring::npos) { return TRUE; }
    std::vector<std::wstring> text;
    EnumChildWindows(window, child_text, reinterpret_cast<LPARAM>(&text));
    const bool directory_seen = std::ranges::any_of(text, [&](const auto& value) {
        return value.find(match.directory) != std::wstring::npos;
    });
    if (!directory_seen) { return TRUE; }
    match.window = window;
    match.title = std::move(title);
    match.text = std::move(text);
    return FALSE;
}

struct PropertiesEvidence final {
    bool passed = false;
    bool observed = false;
    bool usable = false;
    bool returned_while_open = false;
    bool exited_while_open = false;
    bool usable_after_exit = false;
    bool modal = false;
    bool closed = false;
    bool stuck_worker = false;
    DWORD window_process = 0;
    DWORD window_thread = 0;
    launcher::LaunchResult result;
    std::wstring title;
    std::vector<std::wstring> text;
    std::string error;
};

PropertiesEvidence properties(const fs::path& file, std::wstring_view token) {
    PropertiesEvidence evidence;
    struct Completion final { launcher::LaunchResult result; std::atomic<bool> ready = false; };
    const auto completion = std::make_shared<Completion>();
    launcher::NativeLaunchAction action;
    action.target = file.wstring();
    action.arguments = L"Configured program arguments must not reach Properties.";
    action.working_directory = file.root_path().wstring();
    action.show = launcher::LaunchShow::hidden;
    auto worker = std::make_unique<std::thread>([completion, action] {
        try { completion->result = launcher::show_target_properties(action); }
        catch (...) { completion->result = {false, ERROR_GEN_FAILURE, L"Properties worker threw."}; }
        completion->ready.store(true, std::memory_order_release);
    });
    OwnedDialog dialog{std::wstring(token), file.parent_path().filename().wstring()};
    const auto deadline = GetTickCount64() + 10'000;
    ULONGLONG first_seen = 0;
    while (GetTickCount64() < deadline) {
        if (!dialog.window) { EnumWindows(find_dialog, reinterpret_cast<LPARAM>(&dialog)); }
        if (dialog.window && responsive(dialog.window)) {
            evidence.observed = true;
            evidence.usable = true;
            if (!first_seen) { first_seen = GetTickCount64(); }
            evidence.returned_while_open = completion->ready.load(std::memory_order_acquire);
            evidence.exited_while_open = WaitForSingleObject(worker->native_handle(), 0) == WAIT_OBJECT_0;
            if (evidence.exited_while_open) {
                const auto stable = GetTickCount64() + 250;
                while (GetTickCount64() < stable) { pump(); }
                evidence.usable_after_exit = responsive(dialog.window);
                break;
            }
            if (GetTickCount64() - first_seen >= 500) { evidence.modal = !evidence.returned_while_open; break; }
        } else if (completion->ready.load(std::memory_order_acquire) && !completion->result.success) { break; }
        pump();
    }
    if (dialog.window) {
        evidence.title = dialog.title;
        evidence.text = dialog.text;
        evidence.window_thread = GetWindowThreadProcessId(dialog.window, &evidence.window_process);
        // Both the unique file token and the owned directory were observed.
        // Never close another dialog by class, process name, or generic title.
        if (IsWindow(dialog.window)) { PostMessageW(dialog.window, WM_CLOSE, 0, 0); }
    }
    const auto cleanup = GetTickCount64() + 5'000;
    while (GetTickCount64() < cleanup && (IsWindow(dialog.window) || WaitForSingleObject(worker->native_handle(), 0) != WAIT_OBJECT_0)) { pump(); }
    evidence.closed = dialog.window && !IsWindow(dialog.window);
    if (WaitForSingleObject(worker->native_handle(), 0) == WAIT_OBJECT_0) { worker->join(); }
    else { evidence.stuck_worker = true; worker.release(); }
    if (completion->ready.load(std::memory_order_acquire)) { evidence.result = completion->result; }
    evidence.passed = evidence.observed && evidence.usable && evidence.closed && evidence.result.success &&
        !evidence.stuck_worker && (!evidence.exited_while_open || evidence.usable_after_exit);
    if (!evidence.passed) {
        evidence.error = !evidence.observed ? "No usable Properties dialog with the exact owned file/directory appeared." :
            !evidence.closed ? "The exact owned Properties dialog did not close." :
            evidence.stuck_worker ? "Properties worker did not exit after its owned dialog closed." :
            evidence.exited_while_open && !evidence.usable_after_exit ? "Properties became unusable after its worker exited." :
            "Properties API reported failure.";
    }
    return evidence;
}

struct ExplorerSelection final {
    HWND window = nullptr;
    std::wstring path;
};

std::vector<HWND> explorer_windows(IShellWindows* windows) {
    std::vector<HWND> result;
    long count = 0;
    if (FAILED(windows->get_Count(&count))) { return result; }
    for (long index = 0; index < count; ++index) {
        VARIANT item{}; item.vt = VT_I4; item.lVal = index;
        ComPtr<IDispatch> dispatch;
        ComPtr<IWebBrowserApp> browser;
        SHANDLE_PTR window = 0;
        if (SUCCEEDED(windows->Item(item, dispatch.GetAddressOf())) && dispatch && SUCCEEDED(dispatch.As(&browser)) &&
                SUCCEEDED(browser->get_HWND(&window))) { result.push_back(reinterpret_cast<HWND>(window)); }
    }
    return result;
}

ExplorerSelection selected_file(IShellWindows* windows, const fs::path& expected) {
    long count = 0;
    if (FAILED(windows->get_Count(&count))) { return {}; }
    for (long index = 0; index < count; ++index) {
        VARIANT item{}; item.vt = VT_I4; item.lVal = index;
        ComPtr<IDispatch> dispatch;
        ComPtr<IServiceProvider> provider;
        ComPtr<IShellBrowser> browser;
        ComPtr<IShellView> view;
        ComPtr<IFolderView2> folder;
        ComPtr<IShellItemArray> selection;
        if (FAILED(windows->Item(item, dispatch.GetAddressOf())) || !dispatch || FAILED(dispatch.As(&provider)) ||
                FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(browser.GetAddressOf()))) ||
                FAILED(browser->QueryActiveShellView(view.GetAddressOf())) || FAILED(view.As(&folder)) ||
                FAILED(folder->Items(SVGIO_SELECTION, IID_PPV_ARGS(selection.GetAddressOf())))) { continue; }
        DWORD selected_count = 0;
        if (FAILED(selection->GetCount(&selected_count)) || selected_count != 1) { continue; }
        ComPtr<IShellItem> selected;
        PWSTR path = nullptr;
        if (FAILED(selection->GetItemAt(0, selected.GetAddressOf())) || FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &path))) { continue; }
        const std::wstring actual(path);
        CoTaskMemFree(path);
        if (!same_path(actual, expected.native())) { continue; }
        HWND window = nullptr;
        if (SUCCEEDED(browser->GetWindow(&window))) { return {window, actual}; }
    }
    return {};
}

struct RevealEvidence final {
    bool passed = false;
    bool selected = false;
    bool new_window = false;
    bool closed_new_window = false;
    bool retained_reused_window = false;
    std::wstring path;
    launcher::LaunchResult result;
    std::string error;
};

RevealEvidence reveal(const fs::path& file) {
    RevealEvidence evidence;
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(windows.GetAddressOf())))) {
        evidence.error = "Cannot inspect Explorer selection through Shell automation.";
        return evidence;
    }
    const auto before = explorer_windows(windows.Get());
    launcher::NativeLaunchAction action;
    action.target = file.wstring();
    evidence.result = launcher::reveal_launch_target(action);
    ExplorerSelection selection;
    const auto deadline = GetTickCount64() + 10'000;
    while (evidence.result.success && GetTickCount64() < deadline) {
        selection = selected_file(windows.Get(), file);
        if (selection.window && responsive(selection.window)) { break; }
        pump(50);
    }
    evidence.selected = selection.window != nullptr;
    evidence.path = selection.path;
    if (selection.window) {
        evidence.new_window = std::ranges::find(before, selection.window) == before.end();
        if (evidence.new_window) {
            // A new Explorer window currently selects the exact owned file.
            // Existing windows, including a reused tabbed frame, stay open.
            PostMessageW(selection.window, WM_CLOSE, 0, 0);
            const auto cleanup = GetTickCount64() + 3'000;
            while (IsWindow(selection.window) && GetTickCount64() < cleanup) { pump(); }
            evidence.closed_new_window = !IsWindow(selection.window);
        } else { evidence.retained_reused_window = true; }
    }
    evidence.passed = evidence.result.success && evidence.selected && (!evidence.new_window || evidence.closed_new_window);
    if (!evidence.passed) { evidence.error = evidence.selected ? "The new owned Explorer window did not close." : "Explorer did not select the exact owned file."; }
    return evidence;
}

std::string report_json(const fs::path& file, const PropertiesEvidence& property, const RevealEvidence& explorer) {
    std::string text = "{\n  \"target\": " + json(file.native()) + ",\n  \"properties\": {\"passed\": " + boolean(property.passed) +
        ", \"observed\": " + boolean(property.observed) + ", \"responsive\": " + boolean(property.usable) +
        ", \"api_returned_while_open\": " + boolean(property.returned_while_open) + ", \"worker_exited_while_open\": " + boolean(property.exited_while_open) +
        ", \"responsive_after_worker_exit\": " + boolean(property.usable_after_exit) + ", \"modal_worker_while_open\": " + boolean(property.modal) +
        ", \"closed\": " + boolean(property.closed) + ", \"worker_stuck\": " + boolean(property.stuck_worker) +
        ", \"window_process\": " + std::to_string(property.window_process) + ", \"window_thread\": " + std::to_string(property.window_thread) +
        ", \"api_success\": " + boolean(property.result.success) + ", \"api_error\": " + std::to_string(property.result.error_code) +
        ", \"title\": " + json(property.title) + ", \"error\": " + json(property.error) + ", \"child_text\": [";
    for (std::size_t index = 0; index < property.text.size(); ++index) { if (index) { text += ", "; } text += json(property.text[index]); }
    text += "]},\n  \"reveal\": {\"passed\": " + std::string(boolean(explorer.passed)) + ", \"selected_exact_file\": " + boolean(explorer.selected) +
        ", \"api_success\": " + boolean(explorer.result.success) + ", \"api_error\": " + std::to_string(explorer.result.error_code) +
        ", \"selected_path\": " + json(explorer.path) + ", \"new_window\": " + boolean(explorer.new_window) +
        ", \"closed_new_window\": " + boolean(explorer.closed_new_window) + ", \"retained_reused_window\": " + boolean(explorer.retained_reused_window) +
        ", \"error\": " + json(explorer.error) + "}\n}\n";
    return text;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 3 || std::wstring_view(argv[1]) != L"--output") { throw std::runtime_error("Usage: LauncherExactFileUiProbe --output <existing absolute evidence directory>"); }
        const fs::path output = argv[2];
        if (!output.is_absolute() || !fs::is_directory(output)) { throw std::runtime_error("Evidence directory must already exist and be absolute"); }
        const std::wstring token = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        const auto directory = output / (L"ExactFileUi-" + token);
        if (!fs::create_directory(directory)) { throw std::runtime_error("Cannot create a unique owned fixture directory"); }
        const auto file = directory / (L"Selected version 资料 " + token + L".txt");
        { std::ofstream fixture(file, std::ios::binary); fixture << "Owned harmless Launcher exact-target UI fixture.\n"; if (!fixture) { throw std::runtime_error("Cannot write owned fixture"); } }
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(apartment)) { throw std::runtime_error("Probe requires a private COM STA"); }
        const auto property = properties(file, token);
        const auto explorer = property.stuck_worker ? RevealEvidence{} : reveal(file);
        const auto evidence = report_json(file, property, explorer);
        { std::ofstream report(directory / L"result.json", std::ios::binary); report << evidence; if (!report) { throw std::runtime_error("Cannot save native UI evidence"); } }
        std::cout << evidence << std::flush;
        const int exit_code = property.passed && explorer.passed ? 0 : 1;
        if (property.stuck_worker) { TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exit_code)); }
        CoUninitialize();
        return exit_code;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
