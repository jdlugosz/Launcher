#pragma once

#include "launcher/target_version_scan.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

struct LaunchAction;
struct VersionSearchKey;

using AdobeCCInstallation = TargetVersion;

// Product names omit the changing calendar year and Beta suffix. Matching also
// accepts names without Adobe and the Premiere Pro executable-family name.
std::wstring canonical_adobe_cc_name(std::wstring_view product);

// An explicit (Beta) product name restricts its catalog and automatic selection
// to Beta editions, even when the general include_beta option is false.
bool adobe_cc_is_beta_product(std::wstring_view product);
// An .exe target is a filename pattern, not a product-family name.
bool adobe_cc_is_executable_target(std::wstring_view target);

struct AdobeCCRegistration {
    std::wstring product;
    std::filesystem::path location; // Search directory; legacy executable hints are converted to their parent.
    bool recursive = false; // Authored root_path may use an arbitrary hierarchy; registry hints use Adobe layouts.
};

// Enumerates product path values below HKLM\Software\Adobe and Adobe uninstall
// entries in both registry views. No product-name allowlist is used.
std::vector<AdobeCCRegistration> adobe_cc_registrations(const TargetScanLimits& limits = {});

// Registry records provide directory hints only; executable metadata establishes
// each discovered installation. Configured roots supplement all automatic roots.
VersionSearchKey adobe_cc_search_key(const LaunchAction& action, const std::filesystem::path& configuration_directory);
TargetVersionScanResult scan_adobe_cc_catalog(const VersionSearchKey& key, const TargetScanLimits& limits = {});
std::vector<AdobeCCInstallation> scan_adobe_cc_for_action(const LaunchAction& action, const std::filesystem::path& configuration_directory);

// The native empty-root overload includes registry locations and Program Files /
// Adobe. Explicit roots and injected sources stay isolated from the real registry.
// The default root uses Adobe's fixed layouts; additional roots scan directory
// trees, pruning assets below main executables. Links are skipped. Product-name
// discovery requires metadata; explicitly matched filenames can lack it.
// Partial/unreadable listings and metadata are reported as unreliable catalogs,
// allowing the discovery service to retain its last complete snapshot.
TargetVersionScanResult scan_adobe_cc_detailed(std::filesystem::path root, TargetVersionSource& source);
TargetVersionScanResult scan_adobe_cc_detailed(std::filesystem::path root, TargetVersionSource& source,
    const TargetScanLimits& limits);
TargetVersionScanResult scan_adobe_cc_detailed(std::filesystem::path root, TargetVersionSource& source,
    const TargetScanLimits& limits, std::span<const AdobeCCRegistration> registrations);
TargetVersionScanResult scan_adobe_cc_detailed(std::filesystem::path root, TargetVersionSource& source,
    const TargetScanLimits& limits, std::span<const AdobeCCRegistration> registrations, std::wstring_view executable_pattern);
TargetVersionScanResult scan_adobe_cc_detailed(std::filesystem::path root = {});
TargetVersionScanResult scan_adobe_cc_detailed(std::filesystem::path root, const TargetScanLimits& limits);

// Complete-only compatibility API; throws filesystem_error for an incomplete
// or failed observation. An accessible hierarchy with no installation is empty.
std::vector<AdobeCCInstallation> scan_adobe_cc(std::filesystem::path root = {});

// Menus include every installed version, newest first. A target ending in .exe
// matches executable filenames with * and ?; other targets match product names.
// An explicit (Beta)
// product selects only Beta installations. Duplicate executable paths collapse.
std::vector<AdobeCCInstallation> adobe_cc_versions(
    std::span<const AdobeCCInstallation> installations, std::wstring_view product);

// Ordinary launch selects the newest installed edition, including Beta. Passing
// include_beta=false selects stable only; an explicit (Beta) product is Beta-only.
std::optional<AdobeCCInstallation> adobe_cc_latest(
    std::span<const AdobeCCInstallation> installations, std::wstring_view product,
    bool include_beta = true);

// UTF-8 TOML tab, with explanatory comments and one entry per discovered family
// (plus a Beta-only entry when present). Empty targets produces a utility report.
std::string adobe_cc_tab_report(std::span<const AdobeCCInstallation> installations,
    std::span<const std::wstring> unmatched_targets = {}, std::span<const std::wstring> configured_roots = {});

} // namespace launcher
