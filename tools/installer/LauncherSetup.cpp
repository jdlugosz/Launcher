#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>

namespace {

constexpr DWORD capacity = 32768;
constexpr wchar_t package_name[] = LAUNCHER_PACKAGE_NAME;
constexpr struct {
    const wchar_t* name;
    const wchar_t* digest;
} payload[] = {
#include "setup_hashes.h"
};

struct Setup {
    wchar_t package[capacity];
    wchar_t path[capacity];
    wchar_t powershell[capacity];
    wchar_t command[capacity];
    BYTE buffer[16384];
    HANDLE files[ARRAYSIZE(payload)];
    HANDLE directories[capacity / 2];
    DWORD directory_count;
    decltype(&BCryptCreateHash) create_hash;
    decltype(&BCryptHashData) hash_data;
    decltype(&BCryptFinishHash) finish_hash;
    decltype(&BCryptDestroyHash) destroy_hash;
};

bool append(wchar_t* target, const wchar_t* suffix)
{
    const int length = lstrlenW(target);
    if (length + lstrlenW(suffix) >= capacity) return false;
    lstrcpyW(target + length, suffix);
    return true;
}

bool file_path(Setup& setup, const wchar_t* name)
{
    lstrcpyW(setup.path, setup.package);
    return append(setup.path, L"\\") && append(setup.path, name);
}

// Keep the directory entries and the verified files stable until the worker exits.
bool lock_directories(Setup& setup, const wchar_t* directory)
{
    const int length = lstrlenW(directory);
    if (length < 3 || length >= capacity || directory[1] != L':' || directory[2] != L'\\') return false;
    if (directory != setup.path) lstrcpyW(setup.path, directory);
    for (auto* end = setup.path + 3;; ++end) {
        if (*end != L'\\' && *end != L'\0') continue;
        const wchar_t separator = *end;
        *end = L'\0';
        const HANDLE handle = CreateFileW(setup.path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        *end = separator;
        if (handle == INVALID_HANDLE_VALUE) return false;
        if (setup.directory_count == ARRAYSIZE(setup.directories)) { CloseHandle(handle); return false; }
        setup.directories[setup.directory_count++] = handle;
        FILE_ATTRIBUTE_TAG_INFO info;
        if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof info) ||
            (info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != FILE_ATTRIBUTE_DIRECTORY)
            return false;
        if (!separator) return true;
    }
}

bool lock_request(Setup& setup, const wchar_t* request, const wchar_t* result)
{
    const int length = lstrlenW(request);
    const int result_length = lstrlenW(result);
    if (length >= capacity || length < 3 || result_length >= capacity) return false;
    lstrcpyW(setup.path, request);
    auto* name = setup.path + length;
    while (name > setup.path && name[-1] != L'\\') --name;
    if (name < setup.path + 4 || lstrcmpiW(name, L"request.json")) return false;
    *name = L'\0';
    const int prefix = lstrlenW(setup.path);
    if (result_length <= prefix || CompareStringOrdinal(setup.path, prefix, result, prefix, TRUE) != CSTR_EQUAL ||
        lstrcmpiW(result + prefix, L"result.json")) return false;
    name[-1] = L'\0';
    if (!lock_directories(setup, setup.path)) return false;
    const HANDLE file = CreateFileW(request, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    // This handle lives until the worker exits and the OS closes Setup's handles.
    FILE_ATTRIBUTE_TAG_INFO info;
    return GetFileInformationByHandleEx(file, FileAttributeTagInfo, &info, sizeof info) &&
        !(info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

bool check_file(Setup& setup, DWORD index, BCRYPT_ALG_HANDLE algorithm)
{
    if (!file_path(setup, payload[index].name)) return false;
    const HANDLE file = CreateFileW(setup.path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    setup.files[index] = file;
    FILE_ATTRIBUTE_TAG_INFO info;
    if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &info, sizeof info) ||
        (info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (setup.create_hash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) return false;
    BYTE digest[32];
    bool ok = false;
    DWORD size;
    while (ReadFile(file, setup.buffer, sizeof setup.buffer, &size, nullptr)) {
        if (!size) {
            ok = setup.finish_hash(hash, digest, sizeof digest, 0) >= 0;
            break;
        }
        if (setup.hash_data(hash, setup.buffer, size, 0) < 0) break;
    }
    setup.destroy_hash(hash);
    if (!ok) return false;
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (DWORD i = 0; i < sizeof digest; ++i)
        if (payload[index].digest[2 * i] != hex[digest[i] >> 4] ||
            payload[index].digest[2 * i + 1] != hex[digest[i] & 15]) return false;
    return true;
}

bool check_payload(Setup& setup)
{
    const HMODULE bcrypt = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!bcrypt) return false;
    const auto open_algorithm = reinterpret_cast<decltype(&BCryptOpenAlgorithmProvider)>(GetProcAddress(bcrypt, "BCryptOpenAlgorithmProvider"));
    const auto close_algorithm = reinterpret_cast<decltype(&BCryptCloseAlgorithmProvider)>(GetProcAddress(bcrypt, "BCryptCloseAlgorithmProvider"));
    setup.create_hash = reinterpret_cast<decltype(setup.create_hash)>(GetProcAddress(bcrypt, "BCryptCreateHash"));
    setup.hash_data = reinterpret_cast<decltype(setup.hash_data)>(GetProcAddress(bcrypt, "BCryptHashData"));
    setup.finish_hash = reinterpret_cast<decltype(setup.finish_hash)>(GetProcAddress(bcrypt, "BCryptFinishHash"));
    setup.destroy_hash = reinterpret_cast<decltype(setup.destroy_hash)>(GetProcAddress(bcrypt, "BCryptDestroyHash"));
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!open_algorithm || !close_algorithm || !setup.create_hash || !setup.hash_data || !setup.finish_hash || !setup.destroy_hash ||
        !lock_directories(setup, setup.package) || open_algorithm(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return false;
    bool ok = true;
    for (DWORD i = 0; i < ARRAYSIZE(payload) && ok; ++i)
        ok = check_file(setup, i, algorithm);
    close_algorithm(algorithm, 0);
    FreeLibrary(bcrypt);
    return ok;
}

bool quoted(wchar_t* command, const wchar_t* value)
{
    for (const auto* p = value; *p; ++p)
        if (*p == L'"') return false;
    const int length = lstrlenW(value);
    if (length && value[length - 1] == L'\\') return false;
    return append(command, *command ? L" \"" : L"\"") && append(command, value) && append(command, L"\"");
}

bool begin_command(Setup& setup, const wchar_t* script)
{
    if (!file_path(setup, script) || GetFileAttributesW(setup.path) == INVALID_FILE_ATTRIBUTES) return false;
    lstrcpyW(setup.command, L"");
    return quoted(setup.command, setup.powershell) &&
        append(setup.command, L" -NoLogo -NoProfile -STA -ExecutionPolicy Bypass -File") &&
        quoted(setup.command, setup.path);
}

bool append_number(wchar_t* command, ULONG_PTR value)
{
    wchar_t buffer[21];
    auto* digits = buffer + ARRAYSIZE(buffer) - 1;
    *digits = L'\0';
    do {
        *--digits = static_cast<wchar_t>(L'0' + value % 10);
        value /= 10;
    } while (value);
    return append(command, digits);
}

DWORD launch(Setup& setup, bool wait)
{
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(setup.powershell, setup.command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process)) return 2;
    CloseHandle(process.hThread);
    DWORD result = 2;
    if (!wait) result = 0;
    else if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0)
        GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hProcess);
    return result;
}

bool restrict_powershell(Setup& setup)
{
    lstrcpyW(setup.path, setup.powershell);
    auto* end = setup.path + lstrlenW(setup.path);
    while (end > setup.path && end[-1] != L'\\') --end;
    end[-1] = L'\0';
    return SetCurrentDirectoryW(setup.path) && append(setup.path, L"\\Modules") &&
        SetEnvironmentVariableW(L"PSModulePath", setup.path);
}

bool installed_for_all_users(Setup& setup)
{
    wchar_t expected[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, SHGFP_TYPE_CURRENT, expected))) return false;
    constexpr wchar_t suffix[] = L"\\D\u0142ugosz\\Launcher";
    if (lstrlenW(expected) + ARRAYSIZE(suffix) > MAX_PATH) return false;
    lstrcatW(expected, suffix);
    return !lstrcmpiW(expected, setup.package);
}

DWORD run(Setup& setup, int count, wchar_t** args)
{
    const DWORD windows = GetSystemWindowsDirectoryW(setup.powershell, capacity);
    if (!windows || windows >= capacity ||
        !append(setup.powershell, L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe")) return 2;
    SetEnvironmentVariableW(L"PSModulePath", nullptr);

    const DWORD length = GetModuleFileNameW(nullptr, setup.package, capacity);
    if (!length || length >= capacity) return 2;
    auto* slash = setup.package + length;
    while (slash > setup.package && slash[-1] != L'\\') --slash;
    if (slash == setup.package) return 2;
    slash[-1] = L'\0';

    if (count == 4 && !lstrcmpW(args[1], L"--worker")) {
        if (!restrict_powershell(setup) || !check_payload(setup) || !lock_request(setup, args[2], args[3]) ||
            !file_path(setup, L"Launcher Setup.exe")) return 2;
        // Keep the installer copy that will be deployed from being swapped during the worker run.
        const HANDLE image = CreateFileW(setup.path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        FILE_ATTRIBUTE_TAG_INFO info;
        if (image == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(image, FileAttributeTagInfo, &info, sizeof info) ||
            (info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !begin_command(setup, L"Worker.ps1") ||
            !append(setup.command, L" -RequestPath") || !quoted(setup.command, args[2]) ||
            !append(setup.command, L" -ResultPath") || !quoted(setup.command, args[3])) return 2;
#ifndef LAUNCHER_SETUP_ARCHIVE
        if (!append(setup.command, L" -DisableLauncherArchiveSupport")) return 2;
#endif
        return launch(setup, true);
    }

    if (count == 3 && !lstrcmpW(args[1], L"--uninstall")) {
        const bool machine = !lstrcmpW(args[2], L"AllUsers");
        if (machine ? !installed_for_all_users(setup) : lstrcmpW(args[2], L"CurrentUser") != 0) return 2;
        if (!begin_command(setup, L"Maintenance.ps1") || !append(setup.command, L" -Action Uninstall -InstallDirectory") ||
            !quoted(setup.command, setup.package) || !append(setup.command, L" -Interactive")) return 2;
        if (!append(setup.command, machine ? L" -Scope AllUsers" : L" -Scope CurrentUser")) return 2;
        // The script waits for this image to exit before deleting it.
        if (!append(setup.command, L" -SetupProcessId ") ||
            !append_number(setup.command, GetCurrentProcessId())) return 2;
        return launch(setup, false);
    }

    if (count != 1) return 2;
    if (!begin_command(setup, L"Install.ps1")) {
        if (!append(setup.package, L"\\") || !append(setup.package, package_name) ||
            !begin_command(setup, L"Install.ps1")) return 2;
    }
    return launch(setup, true);
}

} // namespace

extern "C" void launcher_setup_entry()
{
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto* setup = static_cast<Setup*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Setup)));
    int count;
    auto** args = CommandLineToArgvW(GetCommandLineW(), &count);
    const DWORD result = setup && args ? run(*setup, count, args) : 2;
    if (result == 2 && !(args && count > 1 && !lstrcmpW(args[1], L"--worker")))
        MessageBoxW(nullptr, L"Launcher Setup could not start. Extract the complete download again, or repair the installation before retrying.",
            L"Launcher Setup", MB_OK | MB_ICONERROR);
    ExitProcess(result);
}
