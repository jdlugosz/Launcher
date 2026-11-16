# 3. Scanning paths and establishing trustworthy results

The scanner answers a narrow question: which regular files are reachable through this configured pattern, and what version information can be read from them? Scheduling, catalog retention and icon actions belong to its consumers. That separation makes filesystem behavior independently testable.

The production entry point is `scan_target_versions_detailed`. It returns a `TargetVersionScanResult` containing a coverage status, observed `TargetVersion` records, bounded diagnostics and any exhausted limit. The older `scan_target_versions` convenience API remains available, but accepts only Complete results and throws for other outcomes. Production pages use the detailed API through the discovery service.

@source src/target_versions.cpp | TargetVersionScanResult scan_target_versions_detailed(std::filesystem::path expanded_pattern, TargetVersionSource& source,

## Follow Blender through the directory tree

Consider the configured target `$P\Blender Foundation\Blender*\blender.exe`. After Launcher expands the configured path markers, the scanner receives an absolute filesystem pattern such as `C:\Program Files\Blender Foundation\Blender*\blender.exe`. Its wildcard component can match `Blender 5.2`, `blender-5.1.0-windows-x64` and `blender-5.3.0-beta+v53.3db45062d058-windows.amd64-release`. Spaces and dashes require no special parsing. The names are input to Windows' own filename matcher.

Only `*` and `?` introduce wildcards. This is Windows filename syntax, not a regular expression or a recursive glob language. The question mark in the extended-path prefix `\\?\` is recognized as part of the prefix. A shell namespace target or URI never enters generic discovery merely because it contains punctuation. Relative filesystem patterns are anchored beside the configuration file; a bare wildcard executable name does not invoke PATH lookup.

@source src/launch.cpp | std::optional<fs::path> target_pattern(std::wstring_view target, const fs::path& base) {

Normalization produces a stable absolute pattern without resolving directory links. Ordinary lexical components can collapse, but a wildcard followed by `..` must survive: the wildcard directory must actually exist before traversal follows its parent. Collapsing that pair early would change the search's meaning. Drive roots, UNC shares and extended paths are split deliberately rather than relying on a decomposition that could lose their real filesystem anchor.

The search then advances through one configured component at a time. It probes literal components and enumerates wildcard components. At each intermediate level, only directories become parents for the next level. At the final level, only regular files become candidates. For Blender, the three matching directories produce three independent probes for `blender.exe`; an unrelated matching folder without that file contributes no version. The scanner does not explore Blender's assets or plugins.

@figure traversal | A Blender wildcard expands one directory level, then probes the same final executable in each matching directory.

Generic configured paths may deliberately cross directory links. Traversal still has a finite depth determined by the supplied components. Enumeration attributes are advisory: a final non-directory match is probed again so that a vanished file or a link to something other than a file does not silently become a launch target. Matching wildcard components also build a readable `path_label`, joining several matched components with ` / `.

## The native adapter owns every native operation

`TargetVersionSource` separates the traversal algorithm from Windows I/O. It exposes `probe`, `probe_no_follow`, `enumerate` and `read_metadata`. Tests can substitute a source that returns a particular directory tree, delays an operation, or fails a cursor after several entries. The native implementation remains a synchronous adapter used inside a worker; the interface does not imply overlapped I/O or coroutines.

@source include/launcher/target_version_scan.hpp | class TargetVersionSource {

An enumeration returns an owned `TargetDirectoryCursor`. Its `next` result distinguishes an entry, normal end and failure. The native cursor keeps the first `WIN32_FIND_DATAW` result from `FindFirstFileW`, then advances with `FindNextFileW`. `ERROR_NO_MORE_FILES` is normal end; another error means incomplete enumeration. A small RAII owner closes the search handle on normal end, failure or stack unwinding. Generic traversal finishes its directory levels before entering the metadata loop, so enumeration handles do not remain open throughout metadata reads.

@source src/target_version_source.cpp | class NativeDirectoryCursor final : public TargetDirectoryCursor {

Native probing starts with `GetFileAttributesW`. Ordinary directories can be accepted directly. Files and links are followed with a query-only handle and checked with native handle APIs, preserving Windows error distinctions. UNC share roots need a handle-based fallback because an attribute query cannot reliably query the share itself. The implementation avoids a standard-library status conversion that can erase a network failure into a generic missing-file result.

## Optional metadata is different from unreadable metadata

The metadata method returns `std::expected<std::optional<TargetVersion>, std::error_code>`. The two wrappers express three distinct states:

| Return | Meaning | Generic target behavior |
| --- | --- | --- |
| Present `TargetVersion` | A usable Windows version resource was read. | Retain numeric and text metadata. |
| Empty optional | Version resources are absent or invalid. | Keep the file with a path-derived version label. |
| Unexpected error | The file or resource could not be read. | Mark coverage Incomplete. |

Before querying version resources, the native reader retains a `GENERIC_READ` handle sharing reads, writes and deletes. This exposes an access or sharing failure that the version APIs can otherwise report as missing resource data. Allowing those sharing modes accommodates installers; the retained handle does not create an atomic snapshot of the file.

@source src/target_version_source.cpp | std::expected<std::optional<TargetVersion>, std::error_code> read_native_metadata(const std::filesystem::path& path,

`GetFileVersionInfoSizeW`, `GetFileVersionInfoW` and `VerQueryValueW` supply fixed numeric product/file versions and translated `ProductName`, `ProductVersion` and `FileVersion` strings. The reader checks buffer extents, signatures, field lengths and translation counts. It copies fixed structures and translation records rather than assuming malformed bytes provide aligned typed objects. Missing text can fall back to numeric text. A missing product name does not disqualify a generic document or executable.

Generic metadata-bearing files sort ahead of files using path fallback. Product-version numbers take precedence, followed by numeric file-version values, then natural text/path ties and deterministic executable-path ordering. Digit-run comparison avoids integer overflow and gives the useful result `5.10 > 5.9`. Generic discovery does not infer a universal stable/beta classification from directory spelling. In the Blender example, executable metadata supplies the important ordering even when a folder contains extra build identifiers.

@source src/target_versions.cpp | bool newest_first(const TargetVersion& left, const TargetVersion& right) {

## Coverage is the trust boundary

Complete means the search completed according to its rules, even when it found nothing. Failed means it could not establish the search, as with normalization or an unavailable initial anchor. A later enumeration or metadata error produces Incomplete, even if no candidate survived. Budget exhaustion also produces Incomplete. Partial records remain diagnostic evidence; they cannot silently redefine “Latest.”

@figure trust | Only a Complete observation replaces the trusted catalog; incomplete evidence and failed attempts retain the last Complete catalog.

An ordinary absent descendant is not automatically a filesystem fault. If one Blender installation lacks `blender.exe` while an ancestor directory still answers, that branch can simply be absent. However, native errors 2 and 3 can also occur after an entire USB drive disappears. The scanner reprobes ancestors, stopping at the drive/share anchor. It must find an answering directory before treating the descendant as ordinary absence. It does not climb outside an unavailable mount and use some unrelated parent to justify an empty catalog. Other native network/access errors retain their original domain and code.

@source src/target_versions.cpp | bool expected_absence(TargetVersionSource& source, const std::filesystem::path& path, const std::filesystem::path& root,

Adobe uses the same source contract and result policy but a different traversal strategy. Its scanner checks known installation layouts, skips links, and requires recognizable product metadata. A remembered executable identity prevents duplicate layouts from reading the same candidate twice. Generic discovery supports arbitrary regular files and uses path labels when version resources are optional; sharing a result type does not erase these provider-specific rules.

## Finite work, honest limits

`TargetScanLimits` bounds each search and can be replaced with small values in deterministic tests. Actual target paths are rejected when too long, never shortened into a different executable identity.

| Default bound | Value |
| --- | --- |
| Supplied path components | 256 |
| Provider operations / entries / branches | 100,000 each |
| Distinct targets | 4,096 |
| One version-resource buffer | 16 MiB |
| Accounted storage | 64 MiB |
| Metadata field / translations | 4,096 UTF-16 units / 128 |
| Diagnostic records / path length | 64 / 32,767 UTF-16 units |

@source include/launcher/target_version_scan.hpp | struct TargetScanLimits {

Storage accounting includes retained text, container capacity and bounded resource data. Allocator bookkeeping and additional bounded scratch remain outside that account; it is not a hard process-memory ceiling. Results retain an exhausted limit even when no diagnostic record fits, and count omitted records. Work budgets cannot make a synchronous network or device call return promptly. The service and runtime must contain that latency without blocking the UI.
