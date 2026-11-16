#include "launcher/windows_pch.hpp"
#include <sddl.h>

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/version_scenarios.hpp"
#include "launcher/adobe_cc.hpp"
#include "launcher/config_text.hpp"
#include "launcher/file_lookup.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace launcher {
namespace {

namespace fs = std::filesystem;
constexpr std::size_t maximum_scenario_bytes = 64 * 1024;

std::string utf8(std::wstring_view source) {
    if (source.empty()) { return {}; }
    if (source.size() > 32'767) { throw std::runtime_error("Scenario text is too long."); }
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source.data(), static_cast<int>(source.size()), nullptr, 0, nullptr, nullptr);
    if (!count) { throw std::system_error(GetLastError(), std::system_category(), "Scenario UTF-16"); }
    std::string result(count, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source.data(), static_cast<int>(source.size()), result.data(), count, nullptr, nullptr)) {
        throw std::system_error(GetLastError(), std::system_category(), "Scenario UTF-16");
    }
    return result;
}

std::wstring wide(std::string_view source) {
    if (source.empty()) { return {}; }
    if (source.size() > maximum_scenario_bytes || source.find('\0') != source.npos) { throw std::runtime_error("Invalid scenario text."); }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source.data(), static_cast<int>(source.size()), nullptr, 0);
    if (!count) { throw std::system_error(GetLastError(), std::system_category(), "Scenario UTF-8"); }
    std::wstring result(count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source.data(), static_cast<int>(source.size()), result.data(), count)) {
        throw std::system_error(GetLastError(), std::system_category(), "Scenario UTF-8");
    }
    return result;
}

std::string json(std::string_view text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char byte : text) {
        if (byte == '"' || byte == '\\') { result += '\\'; result += static_cast<char>(byte); }
        else if (byte < 32) { result += "\\u00"; result += digits[byte >> 4]; result += digits[byte & 15]; }
        else { result += static_cast<char>(byte); }
    }
    return result + '"';
}

bool identifier(std::string_view text, std::size_t maximum = 48) {
    return !text.empty() && text.size() <= maximum && std::ranges::all_of(text, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    });
}

void check_keys(const toml::table& table, std::initializer_list<std::string_view> allowed) {
    for (const auto& [key, ignored] : table) {
        (void)ignored;
        if (std::ranges::find(allowed, key.str()) == allowed.end()) { throw std::runtime_error("Unknown scenario field: " + std::string(key.str())); }
    }
}

std::string text(const toml::table& table, std::string_view key, std::string fallback = {}) {
    const auto* node = table.get(key);
    if (!node) { return fallback; }
    const auto value = node->value<std::string>();
    if (!value || value->find('\0') != value->npos) { throw std::runtime_error("Invalid scenario string: " + std::string(key)); }
    return *value;
}

std::uint32_t number(const toml::table& table, std::string_view key, std::uint32_t fallback, std::uint32_t maximum) {
    const auto* node = table.get(key);
    if (!node) { return fallback; }
    const auto value = node->value<std::int64_t>();
    if (!value || *value < 0 || *value > maximum) { throw std::runtime_error("Invalid scenario integer: " + std::string(key)); }
    return static_cast<std::uint32_t>(*value);
}

bool flag(const toml::table& table, std::string_view key) {
    const auto* node = table.get(key);
    if (!node) { return false; }
    const auto value = node->value<bool>();
    if (!value) { throw std::runtime_error("Invalid scenario boolean: " + std::string(key)); }
    return *value;
}

bool beneath(const fs::path& path, const fs::path& directory) {
    const auto value = path.lexically_normal().native();
    auto prefix = directory.lexically_normal().native();
    if (!prefix.ends_with(L'\\')) { prefix += L'\\'; }
    return value.size() > prefix.size() && CompareStringOrdinal(value.data(), static_cast<int>(prefix.size()),
        prefix.data(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

bool same_path(const fs::path& left, const fs::path& right) {
    const auto identity = [](const fs::path& value) {
        const auto normalized = value.lexically_normal();
        auto text = normalized.native();
        const auto root_size = normalized.root_path().native().size();
        while (text.size() > root_size && text.ends_with(L'\\')) { text.pop_back(); }
        return text;
    };
    const auto first = identity(left);
    const auto second = identity(right);
    return first.size() == second.size() && CompareStringOrdinal(first.data(), static_cast<int>(first.size()),
        second.data(), static_cast<int>(second.size()), TRUE) == CSTR_EQUAL;
}

void fixture_path(const fs::path& path, const fs::path& directory) {
    if (!path.is_absolute() || path.native().size() > 32'767 || path.native().find(L'\0') != path.native().npos || !beneath(path, directory)) {
        throw std::runtime_error("Scenario paths must stay in their explicit fixture directory.");
    }
    auto ancestor = path;
    while (beneath(ancestor, directory) || same_path(ancestor, directory)) {
        const DWORD attributes = GetFileAttributesW(ancestor.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            throw std::runtime_error("Scenario fixtures must not traverse reparse points.");
        }
        const auto parent = ancestor.parent_path();
        if (same_path(parent, ancestor)) { break; }
        ancestor = parent;
    }
}

bool volume_or_share_root(const fs::path& path) {
    if (!path.has_relative_path()) { return true; }
    if (!path.root_name().native().starts_with(L"\\\\")) { return false; }
    std::size_t components = 0;
    for (const auto& component : path.relative_path()) { if (!component.empty()) { ++components; } }
    return components <= 1;
}

void explicit_fixture_path(const fs::path& path) {
    auto preferred = path;
    preferred.make_preferred();
    if (!preferred.is_absolute() || preferred.native().size() > 32'767 || preferred.native().find(L'\0') != preferred.native().npos ||
            preferred.native().starts_with(L"\\\\?\\") || preferred.native().starts_with(L"\\\\.\\")) {
        throw std::runtime_error("Explicit scenario fixture paths require ordinary absolute Windows paths.");
    }
    for (const auto& component : preferred) {
        if (component == L"." || component == L"..") { throw std::runtime_error("Explicit scenario fixture paths cannot traverse parent directories."); }
    }
}

void require_fixture_marker(const fs::path& directory, const FileLookup& files = FileLookup({}, {})) {
    const auto path = directory / L"LauncherVersionScenario.fixture";
    fixture_path(path, directory);
    auto marker = files.open_exact(path, [](const auto& exact) { return std::ifstream(exact, std::ios::binary); });
    std::array<char, 64> line{};
    marker.getline(line.data(), static_cast<std::streamsize>(line.size()));
    std::string_view signature(line.data());
    if (signature.ends_with('\r')) { signature.remove_suffix(1); }
    if (marker.fail() || signature != "Launcher version scenario fixture v1") { throw std::runtime_error("Missing owned scenario fixture marker."); }
}

fs::path external_fixture_directory(fs::path directory) {
    explicit_fixture_path(directory);
    if (has_target_wildcards(directory.native())) { throw std::runtime_error("Scenario fixture directories cannot contain wildcards."); }
    directory = directory.lexically_normal().make_preferred();
    if (volume_or_share_root(directory)) { throw std::runtime_error("A volume or share root cannot be a scenario fixture."); }
    auto ancestor = directory;
    while (!volume_or_share_root(ancestor)) {
        const DWORD attributes = GetFileAttributesW(ancestor.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            throw std::system_error(GetLastError(), std::system_category(), "Scenario fixture directory");
        }
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            throw std::runtime_error("Explicit scenario fixture directories must be existing directories without reparse ancestors.");
        }
        ancestor = ancestor.parent_path();
    }
    directory = fs::canonical(directory);
    require_fixture_marker(directory);
    return directory;
}

class Handle final {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) { CloseHandle(value_); } }
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value_ && value_ != INVALID_HANDLE_VALUE) { CloseHandle(value_); }
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const noexcept { return value_; }
private:
    HANDLE value_;
};

struct LocalDelete { void operator()(void* memory) const noexcept { LocalFree(memory); } };
using LocalMemory = std::unique_ptr<void, LocalDelete>;

LocalMemory current_user_security() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) { throw std::system_error(GetLastError(), std::system_category(), "Scenario token"); }
    const Handle token(raw);
    DWORD bytes = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
    if (!bytes || bytes > 64 * 1024) { throw std::runtime_error("Invalid scenario token size."); }
    std::vector<std::byte> information(bytes);
    if (!GetTokenInformation(token.get(), TokenUser, information.data(), bytes, &bytes)) {
        throw std::system_error(GetLastError(), std::system_category(), "Scenario token user");
    }
    TOKEN_USER user{};
    if (information.size() < sizeof(user)) { throw std::runtime_error("Invalid scenario token user."); }
    std::memcpy(&user, information.data(), sizeof(user));
    LPWSTR raw_sid = nullptr;
    if (!ConvertSidToStringSidW(user.User.Sid, &raw_sid)) { throw std::system_error(GetLastError(), std::system_category(), "Scenario SID"); }
    const LocalMemory sid(raw_sid);
    const std::wstring descriptor = L"D:P(A;;GA;;;" + std::wstring(raw_sid) + L")";
    PSECURITY_DESCRIPTOR security = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(), SDDL_REVISION_1, &security, nullptr)) {
        throw std::system_error(GetLastError(), std::system_category(), "Scenario event security");
    }
    return LocalMemory(security);
}

Handle event(const std::wstring& name, void* security, bool signaled) {
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security, FALSE};
    SetLastError(ERROR_SUCCESS);
    Handle value(CreateEventW(&attributes, TRUE, signaled, name.c_str()));
    const DWORD error = GetLastError();
    if (!value.get()) { throw std::system_error(error, std::system_category(), "Scenario event"); }
    if (error == ERROR_ALREADY_EXISTS) { throw std::runtime_error("Scenario event name already exists."); }
    return value;
}

struct Attempt final {
    std::string outcome;
    std::string gate_stage;
    DWORD error = ERROR_BAD_NETPATH;
    std::optional<std::uint32_t> fail_after_entries;
    bool gated = false;
    std::wstring entered_name;
    std::wstring release_name;
    Handle entered;
    Handle release;
};

struct Search final {
    std::string id;
    VersionSearchKey key;
    fs::path path;
    fs::path fixture;
    bool explicit_fixture = false;
    std::vector<Attempt> attempts;
    std::size_t started = 0;
};

struct CursorFault final {
    std::optional<std::uint32_t> fail_after;
    std::uint32_t entries = 0;
    DWORD error;
    std::string gate_stage;
    std::function<void()> gate;
    bool gate_reached = false;
    void reach(std::string_view stage) {
        if (stage == gate_stage && !std::exchange(gate_reached, true)) { gate(); }
    }
};

class FaultCursor final : public TargetDirectoryCursor {
public:
    FaultCursor(std::unique_ptr<TargetDirectoryCursor> native, std::shared_ptr<CursorFault> fault)
        : native_(std::move(native)), fault_(std::move(fault)) {}
    std::expected<std::optional<TargetDirectoryEntry>, std::error_code> next() override {
        // The native cursor and Find handle already exist at this boundary.
        fault_->reach("cursor");
        if (fault_->fail_after && fault_->entries >= *fault_->fail_after) {
            return std::unexpected(std::error_code(static_cast<int>(fault_->error), std::system_category()));
        }
        auto next = native_->next();
        if (next && *next) {
            ++fault_->entries;
            if ((**next).reparse_point) { return std::unexpected(std::error_code(ERROR_CANT_ACCESS_FILE, std::system_category())); }
        }
        return next;
    }
private:
    std::unique_ptr<TargetDirectoryCursor> native_;
    std::shared_ptr<CursorFault> fault_;
};

class FaultSource final : public TargetVersionSource {
public:
    FaultSource(fs::path fixture, const Attempt& attempt, std::function<void()> gate) : fixture_(std::move(fixture)), native_(make_native_target_version_source()),
        fault_(std::make_shared<CursorFault>(attempt.fail_after_entries, 0, attempt.error, attempt.gate_stage, std::move(gate))) {}
    std::expected<TargetFileKind, std::error_code> probe(const fs::path& path) override {
        // The generic scanner probes the drive/share anchor before descending.
        // Ancestors are query-only; enumerations/metadata stay inside the fixture.
        if (beneath(path, fixture_)) { fixture_path(path, fixture_); }
        else if (!beneath(fixture_, path) && !same_path(path, fixture_)) {
            throw std::runtime_error("Scenario probe escaped fixture ancestors: path=" + utf8(path.native()) +
                "; fixture=" + utf8(fixture_.native()) + ".");
        }
        return native_->probe(path);
    }
    std::expected<TargetFileKind, std::error_code> probe_no_follow(const fs::path& path) override {
        fixture_path(path, fixture_);
        return native_->probe_no_follow(path);
    }
    std::expected<std::unique_ptr<TargetDirectoryCursor>, std::error_code> enumerate(const fs::path& path) override {
        fixture_path(path, fixture_);
        auto native = native_->enumerate(path);
        if (!native) { return std::unexpected(native.error()); }
        return std::make_unique<FaultCursor>(std::move(*native), fault_);
    }
    std::expected<std::optional<TargetVersion>, std::error_code> read_metadata(const fs::path& path) override {
        fixture_path(path, fixture_);
        fault_->reach("metadata");
        return native_->read_metadata(path);
    }
    std::expected<std::optional<TargetVersion>, std::error_code> read_metadata(const fs::path& path, const TargetScanLimits& limits) override {
        fixture_path(path, fixture_);
        fault_->reach("metadata");
        return native_->read_metadata(path, limits);
    }
private:
    fs::path fixture_;
    std::unique_ptr<TargetVersionSource> native_;
    std::shared_ptr<CursorFault> fault_;
};

} // namespace

struct VersionScenarios::Impl final {
    FileLookup files = FileLookup({}, {});
    fs::path fixture;
    fs::path configuration;
    fs::path output;
    std::string instance;
    std::map<VersionSearchKey, Search> searches;
    std::mutex mutex;
    std::ofstream trace;
    UINT control = 0;
    bool notifications = false;

    void record(std::string_view stage, std::string_view details = {}) {
        const std::lock_guard lock(mutex);
        trace << "{\"stage\":" << json(stage) << ",\"pid\":" << GetCurrentProcessId();
        if (!details.empty()) { trace << ',' << details; }
        trace << "}\n";
        trace.flush();
        if (!trace) { throw std::runtime_error("Cannot flush scenario evidence."); }
    }

    TargetVersionScanResult scan(const VersionSearchKey& key) {
        Search* search = nullptr;
        Attempt* attempt = nullptr;
        std::size_t ordinal = 0;
        {
            const std::lock_guard lock(mutex);
            const auto found = searches.find(key);
            if (found == searches.end()) { throw std::runtime_error("The scenario has no explicit fixture for this search."); }
            search = &found->second;
            ordinal = ++search->started;
            if (ordinal > search->attempts.size()) { throw std::runtime_error("The scenario exhausted its explicit attempt plan."); }
            attempt = &search->attempts[ordinal - 1];
        }
        const auto identity = "\"search\":" + json(search->id) + ",\"attempt\":" + std::to_string(ordinal);
        record("started", identity);
        const auto gate = [this, attempt, identity] {
            record("entered", identity + ",\"gate_stage\":" + json(attempt->gate_stage));
            if (!SetEvent(attempt->entered.get())) { throw std::system_error(GetLastError(), std::system_category(), "Scenario entered signal"); }
            if (WaitForSingleObject(attempt->release.get(), INFINITE) != WAIT_OBJECT_0) {
                throw std::system_error(GetLastError(), std::system_category(), "Scenario release wait");
            }
            record("released", identity);
        };
        if (attempt->gate_stage == "before") { gate(); }
        if (attempt->outcome == "throw") { throw std::runtime_error("Injected scenario scanner exception."); }
        TargetVersionScanResult result;
        if (attempt->outcome == "failed") {
            result.status = TargetScanStatus::failed;
        } else {
            FaultSource source(search->fixture, *attempt, gate);
            result = key.kind == VersionSearchKind::adobe_catalog ? scan_adobe_cc_detailed(search->path, source) :
                scan_target_versions_detailed(search->path, source);
            if (attempt->outcome == "incomplete") { result.status = TargetScanStatus::incomplete; }
        }
        if (attempt->outcome == "failed" || attempt->outcome == "incomplete") {
            if (result.diagnostics.size() < 64) {
                result.diagnostics.push_back({TargetScanOperation::enumerate, search->path,
                    {static_cast<int>(attempt->error), std::system_category()}});
            } else { ++result.omitted_diagnostics; }
        }
        const auto status = result.status == TargetScanStatus::complete ? "complete" :
            result.status == TargetScanStatus::incomplete ? "incomplete" : "failed";
        record("finished", identity + ",\"status\":" + json(status) + ",\"observed\":" + std::to_string(result.versions.size()));
        return result;
    }
};

std::shared_ptr<VersionScenarios> VersionScenarios::load(const fs::path& scenario, const fs::path& explicit_configuration) {
    if (!scenario.is_absolute() || !explicit_configuration.is_absolute() || has_target_wildcards(scenario.native())) {
        throw std::runtime_error("Version scenarios require explicit absolute scenario and configuration paths.");
    }
    if (fs::file_size(scenario) > maximum_scenario_bytes) { throw std::runtime_error("Version scenario exceeds 64 KiB."); }
    auto impl = std::make_unique<Impl>();
    impl->fixture = fs::canonical(scenario.parent_path());
    fixture_path(scenario, impl->fixture);
    fixture_path(explicit_configuration, impl->fixture);
    require_fixture_marker(impl->fixture, impl->files);
    const auto table = config_text_builder(scenario, impl->files).parse();
    check_keys(table, {"schema_version", "instance", "configuration", "output_directory", "dispatch", "notifications", "searches"});
    if (number(table, "schema_version", 0, 1) != 1 || text(table, "dispatch") != "capture-only") {
        throw std::runtime_error("Scenario schema 1 and capture-only dispatch are required.");
    }
    impl->instance = text(table, "instance");
    impl->notifications = flag(table, "notifications");
    if (!identifier(impl->instance)) { throw std::runtime_error("Invalid scenario instance."); }
    impl->configuration = fs::path(wide(text(table, "configuration"))).lexically_normal();
    fixture_path(impl->configuration, impl->fixture);
    if (fs::canonical(impl->configuration) != fs::canonical(explicit_configuration)) {
        throw std::runtime_error("The explicit configuration does not match the scenario fixture.");
    }
    const auto output_root = fs::path(wide(text(table, "output_directory"))).lexically_normal();
    fixture_path(output_root, impl->fixture);
    fs::create_directories(output_root);
    impl->output = output_root / (L"process-" + std::to_wstring(GetCurrentProcessId()));
    if (!fs::create_directory(impl->output)) { throw std::runtime_error("Scenario process evidence already exists."); }
    impl->trace = impl->files.open_output(impl->output / L"trace.jsonl");
    if (!impl->trace) { throw std::runtime_error("Cannot create scenario evidence."); }
    const auto control_name = L"Dlugosz.Launcher.VersionScenario." + wide(impl->instance) + L"." + std::to_wstring(GetCurrentProcessId());
    impl->control = RegisterWindowMessageW(control_name.c_str());
    if (!impl->control) { throw std::system_error(GetLastError(), std::system_category(), "Scenario control message"); }
    const auto* searches = table.get_as<toml::array>("searches");
    if (!searches || searches->empty() || searches->size() > 128) { throw std::runtime_error("A scenario requires 1..128 searches."); }
    const auto security = current_user_security();
    toml::array manifest_searches;
    for (const auto& entry : *searches) {
        const auto* description = entry.as_table();
        if (!description) { throw std::runtime_error("A scenario search must be a table."); }
        check_keys(*description, {"id", "provider", "path", "fixture_directory", "attempts"});
        Search search;
        search.id = text(*description, "id");
        if (!identifier(search.id)) { throw std::runtime_error("Invalid scenario search ID."); }
        search.path = fs::path(wide(text(*description, "path")));
        if (!search.path.is_absolute()) { throw std::runtime_error("Scenario search paths must be absolute."); }
        search.explicit_fixture = description->contains("fixture_directory");
        search.fixture = search.explicit_fixture ? external_fixture_directory(fs::path(wide(text(*description, "fixture_directory")))) : impl->fixture;
        if (search.explicit_fixture) { explicit_fixture_path(search.path); }
        search.path = normalize_target_version_pattern(std::move(search.path));
        fixture_path(search.path, search.fixture);
        const auto provider = text(*description, "provider", "target");
        if (provider == "adobe") { search.key.kind = VersionSearchKind::adobe_catalog; }
        else if (provider == "target") {
            auto key = search.path.native();
            CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
            search.key.locations.push_back(std::move(key));
        } else { throw std::runtime_error("Scenario provider must be target or adobe."); }
        const auto* attempts = description->get_as<toml::array>("attempts");
        if (!attempts || attempts->empty() || attempts->size() > 32) { throw std::runtime_error("A scenario search requires 1..32 attempts."); }
        toml::array manifest_attempts;
        for (const auto& plan : *attempts) {
            const auto* detail = plan.as_table();
            if (!detail) { throw std::runtime_error("A scenario attempt must be a table."); }
            check_keys(*detail, {"outcome", "gate", "gate_stage", "error", "fail_after_entries"});
            Attempt attempt;
            attempt.outcome = text(*detail, "outcome", "native");
            if (attempt.outcome != "native" && attempt.outcome != "failed" && attempt.outcome != "incomplete" && attempt.outcome != "throw") {
                throw std::runtime_error("Unknown scenario outcome.");
            }
            attempt.gate_stage = text(*detail, "gate_stage", "before");
            if (attempt.gate_stage != "before" && attempt.gate_stage != "cursor" && attempt.gate_stage != "metadata") {
                throw std::runtime_error("Scenario gate_stage must be before, cursor, or metadata.");
            }
            if (attempt.gate_stage != "before" && (attempt.outcome == "failed" || attempt.outcome == "throw")) {
                throw std::runtime_error("Forced scenario failures require a before gate.");
            }
            attempt.gated = flag(*detail, "gate");
            attempt.error = number(*detail, "error", ERROR_BAD_NETPATH, 1'000'000);
            if (!attempt.error) { throw std::runtime_error("A scenario Win32 error must be nonzero."); }
            if (detail->contains("fail_after_entries")) { attempt.fail_after_entries = number(*detail, "fail_after_entries", 0, 100'000); }
            const auto name = L"Local\\" + control_name + L"." + wide(search.id) + L"." + std::to_wstring(search.attempts.size() + 1);
            attempt.entered_name = name + L".entered";
            attempt.release_name = name + L".release";
            attempt.entered = event(attempt.entered_name, security.get(), false);
            attempt.release = event(attempt.release_name, security.get(), !attempt.gated);
            manifest_attempts.push_back(toml::table{{"entered", utf8(attempt.entered_name)}, {"release", utf8(attempt.release_name)},
                {"gated", attempt.gated}, {"gate_stage", attempt.gate_stage}});
            search.attempts.push_back(std::move(attempt));
        }
        manifest_searches.push_back(toml::table{{"id", search.id}, {"fixture_directory", utf8(search.fixture.native())},
            {"attempts", std::move(manifest_attempts)}});
        if (!impl->searches.emplace(search.key, std::move(search)).second) { throw std::runtime_error("Duplicate scenario search identity."); }
    }
    const toml::table manifest{{"schema_version", 1}, {"pid", static_cast<std::int64_t>(GetCurrentProcessId())},
        {"control_message", static_cast<std::int64_t>(impl->control)}, {"capture_only", true}, {"notifications", impl->notifications},
        {"configuration", utf8(impl->configuration.native())}, {"searches", std::move(manifest_searches)}};
    auto output = impl->files.open_output(impl->output / L"manifest.toml");
    output << manifest;
    output.close();
    if (!output) { throw std::runtime_error("Cannot write scenario control manifest."); }
    auto json_output = impl->files.open_output(impl->output / L"manifest.json");
    json_output << "{\"schema_version\":1,\"pid\":" << GetCurrentProcessId() << ",\"control_message\":" << impl->control
        << ",\"capture_only\":true,\"notifications\":" << (impl->notifications ? "true" : "false") << ",\"searches\":[";
    bool first_search = true;
    for (const auto& [key, search] : impl->searches) {
        (void)key;
        if (!std::exchange(first_search, false)) { json_output << ','; }
        json_output << "{\"id\":" << json(search.id) << ",\"fixture_directory\":" << json(utf8(search.fixture.native())) << ",\"attempts\":[";
        bool first_attempt = true;
        for (const auto& attempt : search.attempts) {
            if (!std::exchange(first_attempt, false)) { json_output << ','; }
            json_output << "{\"entered\":" << json(utf8(attempt.entered_name)) << ",\"release\":" << json(utf8(attempt.release_name))
                << ",\"gated\":" << (attempt.gated ? "true" : "false") << ",\"gate_stage\":" << json(attempt.gate_stage) << '}';
        }
        json_output << "]}";
    }
    json_output << "]}\n";
    json_output.close();
    if (!json_output) { throw std::runtime_error("Cannot write scenario JSON control manifest."); }
    impl->record("loaded");
    return std::shared_ptr<VersionScenarios>(new VersionScenarios(std::move(impl)));
}

VersionScenarios::VersionScenarios(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VersionScenarios::~VersionScenarios() = default;

VersionDiscoveryService::Scanner VersionScenarios::scanner() {
    return [owner = shared_from_this()](const VersionSearchKey& key) { return owner->impl_->scan(key); };
}

void VersionScenarios::capture_action(const PreparedLaunch& launch, std::string_view verb, bool reveal) {
    const auto& action = launch.action;
    std::ostringstream details;
    details << "\"verb\":" << json(verb) << ",\"reveal\":" << (reveal ? "true" : "false") << ",\"prepared\":" << (action ? "true" : "false");
    if (action) {
        const auto target = fs::path(action->target);
        const auto owner = std::ranges::find_if(impl_->searches, [&](const auto& entry) { return beneath(target, entry.second.fixture); });
        if (owner == impl_->searches.end()) { throw std::runtime_error("Scenario action escaped its configured fixture directories."); }
        if (owner->second.explicit_fixture) { explicit_fixture_path(target); }
        fixture_path(target, owner->second.fixture);
        details << ",\"target\":" << json(utf8(action->target)) << ",\"arguments\":" << json(utf8(action->arguments))
            << ",\"working_directory\":" << json(utf8(action->working_directory)) << ",\"show\":" << static_cast<unsigned>(action->show);
    }
    const auto value = details.str();
    if (value.size() > maximum_scenario_bytes) { throw std::runtime_error("Scenario action receipt exceeds 64 KiB."); }
    impl_->record("action-captured", value);
}

void VersionScenarios::checkpoint(std::string_view stage) {
    if (!identifier(stage, 64)) { throw std::runtime_error("Invalid scenario checkpoint."); }
    impl_->record(stage);
}

std::optional<VersionScenarioCommand> VersionScenarios::command(std::uint32_t message, std::uintptr_t parameter,
        std::intptr_t process) const noexcept {
    if (message != impl_->control || process != static_cast<std::intptr_t>(GetCurrentProcessId()) || parameter > 0xffff'ffff) { return std::nullopt; }
    const auto kind = static_cast<VersionScenarioCommandKind>(parameter & 0xffff);
    if (kind < VersionScenarioCommandKind::close || kind > VersionScenarioCommandKind::open_icon_menu) { return std::nullopt; }
    return VersionScenarioCommand{kind, static_cast<std::uint16_t>(parameter >> 16)};
}

const fs::path& VersionScenarios::configuration_path() const noexcept { return impl_->configuration; }
const fs::path& VersionScenarios::output_directory() const noexcept { return impl_->output; }
bool VersionScenarios::notifications_enabled() const noexcept { return impl_->notifications; }

} // namespace launcher
