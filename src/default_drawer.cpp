#include "launcher/windows_pch.hpp"

#include <bcrypt.h>
#include <sddl.h>

#include "default_drawer.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace launcher {
namespace {

struct LocalDelete final {
    void operator()(void* value) const noexcept { LocalFree(value); }
};
using LocalMemory = std::unique_ptr<void, LocalDelete>;

[[noreturn]] void throw_last_error(const char* operation)
{
    throw std::system_error(GetLastError(), std::system_category(), operation);
}

std::wstring user_sid()
{
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) { throw_last_error("Read default drawer user"); }
    const std::unique_ptr<void, decltype(&CloseHandle)> token(raw_token, CloseHandle);
    DWORD bytes = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
    if (bytes < sizeof(TOKEN_USER)) { throw_last_error("Read default drawer user size"); }
    std::vector<std::byte> data(bytes);
    if (!GetTokenInformation(token.get(), TokenUser, data.data(), bytes, &bytes)) { throw_last_error("Read default drawer SID"); }
    TOKEN_USER user{};
    std::memcpy(&user, data.data(), sizeof(user));
    PWSTR sid_text = nullptr;
    if (!ConvertSidToStringSidW(user.User.Sid, &sid_text)) { throw_last_error("Format default drawer SID"); }
    const LocalMemory sid(sid_text);
    return sid_text;
}

std::wstring object_prefix(const std::filesystem::path& installation, const std::wstring& sid)
{
    // Canonicalize drive casing, short paths and junctions before deriving an
    // object name. Keep the name independent of the executable's build number.
    auto path = std::filesystem::canonical(installation).wstring();
    const int length = static_cast<int>(path.size());
    const int needed = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, path.data(), length,
        nullptr, 0, nullptr, nullptr, 0);
    if (!needed) { throw_last_error("Normalize default drawer installation directory"); }
    std::wstring normalized(static_cast<std::size_t>(needed), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, path.data(), length,
            normalized.data(), needed, nullptr, nullptr, 0)) {
        throw_last_error("Normalize default drawer installation directory");
    }
    std::array<unsigned char, 32> hash{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(normalized.data()),
            static_cast<ULONG>(normalized.size() * sizeof(wchar_t)), hash.data(), static_cast<ULONG>(hash.size())) < 0) {
        throw std::runtime_error("Cannot identify the default drawer installation directory.");
    }
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring name = L"Local\\Dlugosz.Launcher.DefaultDrawer." + sid + L".";
    for (const auto byte : hash) {
        name += hex[byte >> 4];
        name += hex[byte & 15];
    }
    return name;
}

bool acquire(HANDLE mutex, DWORD timeout)
{
    switch (WaitForSingleObject(mutex, timeout)) {
    case WAIT_OBJECT_0:
    case WAIT_ABANDONED: return true;
    case WAIT_TIMEOUT: return false;
    default: throw_last_error("Wait for the default Launcher drawer");
    }
}

} // namespace

DefaultDrawer::DefaultDrawer(const std::filesystem::path& installation)
{
    const auto sid = user_sid();
    const auto prefix = object_prefix(installation, sid);
    // Limit access to this user. Windows also applies the token's normal
    // integrity boundary to the synchronization objects.
    const std::wstring permissions = L"D:P(A;;GA;;;" + sid + L")";
    PSECURITY_DESCRIPTOR raw_security = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(permissions.c_str(), SDDL_REVISION_1, &raw_security, nullptr)) {
        throw_last_error("Create default drawer security descriptor");
    }
    const LocalMemory security(raw_security);
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), security.get(), FALSE};
    owner_.reset(CreateMutexExW(&attributes, (prefix + L".Owner").c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE));
    if (!owner_) { throw_last_error("Open default drawer ownership"); }
    transition_.reset(CreateMutexExW(&attributes, (prefix + L".Transition").c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE));
    if (!transition_) { throw_last_error("Open default drawer startup transition"); }
    reveal_.reset(CreateEventExW(&attributes, (prefix + L".Reveal").c_str(), 0, SYNCHRONIZE | EVENT_MODIFY_STATE));
    if (!reveal_) { throw_last_error("Open default drawer activation event"); }
}

DefaultDrawer::~DefaultDrawer()
{
    if (owns_drawer_) { ReleaseMutex(owner_.get()); }
    if (owns_transition_) { ReleaseMutex(transition_.get()); }
}

bool DefaultDrawer::start(bool forward_reveal)
{
    reserve_restart();
    owns_drawer_ = acquire(owner_.get(), 0);
    if (!owns_drawer_ && forward_reveal) {
        if (!SetEvent(reveal_.get())) { throw_last_error("Reveal the existing Launcher drawer"); }
        // The previous owner may have quit between the check and the signal.
        // Keep the request and become its recipient if the owner is now gone.
        owns_drawer_ = acquire(owner_.get(), 0);
    }
    ReleaseMutex(transition_.get());
    owns_transition_ = false;
    if (owns_drawer_) { return true; }
    if (!forward_reveal) { throw std::runtime_error("The default Launcher drawer is already running. Close it before retrying setup startup."); }
    return false;
}

void DefaultDrawer::reserve_restart()
{
    // A ready Restart child keeps contenders out until the old process has
    // exited and the child can take over the default drawer's ownership.
    owns_transition_ = acquire(transition_.get(), 15000);
    if (!owns_transition_) { throw std::runtime_error("The default Launcher drawer is still restarting."); }
}

void DefaultDrawer::finish_restart()
{
    owns_drawer_ = acquire(owner_.get(), 0);
    ReleaseMutex(transition_.get());
    owns_transition_ = false;
    if (!owns_drawer_) { throw std::runtime_error("The restarted Launcher could not take ownership of the default drawer."); }
}

} // namespace launcher
