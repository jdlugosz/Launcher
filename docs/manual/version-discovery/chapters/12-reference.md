# 12. Reference and source map

## Working vocabulary

| Term | Meaning in this implementation |
| --- | --- |
| Activation / visit | A monotonic token for the current genuinely shown tab. It bounds repeated checks for a search. |
| Attention | A request caused by interaction with a particular icon. It promotes queued work and can use reserved admission capacity. |
| Attempt | One accepted or failed-to-start check. It records diagnostics independently from the retained complete catalog. |
| Binding | Per-icon configuration, selector and original launch options associated with a shared search. |
| Complete | The scanner regards its coverage as authoritative, including a successful empty result. |
| Incomplete | The observation contains partial or unreliable coverage. Its candidate rows remain diagnostic evidence. |
| Failed | `TargetScanStatus::failed` means the scanner could not establish coverage. Separately, `VersionDiscoveryFailure::start` / `scanner` record rejected start or a thrown scanner exception. |
| Generation | The identity of a requested coherent presentation. It prevents an older prepared result from replacing newer intent. |
| Pin | An explicit pending action's reference that retains useful queued work across hiding. |
| Publication | Making an immutable job result visible through a release/acquire flag. |
| Snapshot | An immutable retained catalog or a copied coherent interaction state, according to context. |
| Tombstone | A removed-but-running private search record retained until collection. |

## Entry points for a code tour

The following anchors are deliberately small enough to begin a targeted read. Source links and line numbers resolve from the implementation baseline recorded in the front matter.

### Configuration and scanner

@source src/launch.cpp | std::optional<fs::path> version_target_pattern(
@source src/target_versions.cpp | std::filesystem::path normalize_target_version_pattern(
@source include/launcher/target_version_scan.hpp | class TargetVersionSource {
@source src/target_versions.cpp | TargetVersionScanResult scan_target_versions_detailed(
@source src/target_version_source.cpp | class NativeDirectoryCursor
@source src/adobe_cc.cpp | scan_adobe_cc_detailed(

### Service and runtime

@source include/launcher/version_discovery.hpp | class VersionDiscoveryService final
@source src/version_discovery.cpp | void start(const VersionSearchKey& key
@source src/version_discovery.cpp | void admit() noexcept
@source src/version_discovery.cpp | std::size_t collect() noexcept
@source src/version_discovery_runtime.cpp | void NativeVersionDiscoveryExecutor::start(
@source src/version_discovery_runtime.cpp | void NativeVersionDiscoveryExecutor::finish_process_if_busy(

### Window, presentation and interaction

@source src/main.cpp | void sync_version_activation()
@source src/main.cpp | void collect_background()
@source src/main.cpp | void queue_item_presentation(
@source src/item_presentation.cpp | ItemPresentation prepare_item_presentation(
@source src/main.cpp | void adopt_item_presentations()
@source src/main.cpp | void capture_version_results(
@source src/main.cpp | bool version_adoption_held() const
@source src/main.cpp | struct VersionMenuSession

### Diagnostics and instrumentation

@source src/filesystem_error_log.cpp | bool FilesystemErrorLog::append(
@source src/filesystem_error_notice.cpp | bool PerSearchNoticeState::observe(
@source src/app_notification.cpp | HRESULT AppNotification::show(
@source src/main.cpp | int activate_filesystem_notification()
@source include/launcher/version_scenarios.hpp | class VersionScenarios final

## Retained evidence map

The repository's ignored `build/verification` directories retain machine-specific test receipts, fixture outputs and logs. They support the implementation record but are not dependencies of this book or files to ship with Launcher.

| Location | Purpose |
| --- | --- |
| `2026-10-19-version-feature-completion/final-review.md` | Completed feature review and selected test scope. |
| `2026-10-19-version-feature-completion/delivery.json` | Final completion/installed-build handoff record. |
| `2026-10-19-version-feature-completion/controlled-network-253428774ddf4358b87b8e191919de71` | Successful controlled real-SMB run. Earlier failed runs remain separate evidence. |
| `2026-10-19-usb-notification` | Genuine USB removal and sharing-error journals, retained catalogs, persistent notification and acceptance receipts. |
| `2026-10-20-overlay-gallery/native-qa.json` | Native size/DPI/slat-count and overlay-coverage checks for the current artwork gallery. |
| `version-discovery-manual/source-validation.json` | Resolved source anchors, excerpt ranges and source hashes for this edition. |

## Native API references

Microsoft's API documentation supplies the platform contracts. The source files supply Launcher's chosen policy and error interpretation.

- [FindFirstFileW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstfilew): native wildcard enumeration, first result, error reporting and `FindClose` ownership. It does not sort matches.
- [MsgWaitForMultipleObjectsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjectsex): the UI's wait can observe synchronization handles and Windows input. Keep each waited handle alive throughout the wait.
- [TerminateProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminateprocess): the explicit whole-process boundary has different cleanup semantics from normal C++ teardown and does not provide a provider-latency guarantee.

## Suggested final review questions

When reviewing a change, identify the shared search, the attempt it changes, the complete catalog the UI may trust, and the presentation a user is acting on. Check what happens if a provider returns slowly, returns an incomplete observation, throws, or never returns. Then identify which owner releases each object and how the process can finish after that owner disappears.

Those questions describe the feature's essential structure. The code can evolve while keeping their answers explicit.
