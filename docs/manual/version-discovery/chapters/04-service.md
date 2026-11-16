# 4. Scheduling checks and preserving the last good catalog

`VersionDiscoveryService` coordinates searches. Its public methods run on the owner/UI thread; private job publication crosses threads. Windows handles, drawing resources and notification transport belong to other layers, keeping scheduling tests deterministic and native worker ownership explicit.

@source include/launcher/version_discovery.hpp | class VersionDiscoveryService final {

## Search identity describes discovery, not launching

`VersionSearchKey` contains a provider kind, owned location strings and a semantic revision. The generic binding adapter supplies one expanded, absolute, normalized, case-insensitive pattern. Adobe's current page adapter supplies one shared default-catalog key, represented by `adobe_catalog` with an empty location list. The type can represent roots, but readers should not infer that production currently builds a separate key per Adobe product.

@code include/launcher/version_discovery.hpp | struct VersionSearchKey { | 7 | A search key owns the values that determine discovery equivalence.

Arguments, working directory, show mode, product selector, icon identity and activation token are absent from the key. Two Blender icons pointing at the same wildcard can share one directory/metadata scan even when one launches a particular document or uses different options. Their presentation and concrete actions remain separate. Conversely, changing the provider or semantic revision changes discovery identity even if a location string looks identical.

The service owns a map of records keyed by these values. Each record tracks configuration membership, visibility, queue state, priority/order, the queued and attempted activation, pending-intent references, an optional running job, the last attempt and the last trusted snapshot. The production scanner callable captures an owned pattern map. Its journal wrapper writes filesystem diagnostics before returning the observation for publication, so reporting does not depend on whether an icon is still visible.

@source src/main.cpp | auto patterns = std::make_shared<std::map<launcher::VersionSearchKey, std::filesystem::path>>();

## A visit is the refresh boundary

The UI calls `sync_version_activation` when the dock's shown state or active tab may have changed. A visit exists only while the dock is expanded and not hidden to the taskbar, transitioning, restarting or unable to lay out its display. Repeated synchronization for the same shown tab returns immediately. Revealing that tab again, or switching to another shown tab, begins another visit.

@source src/main.cpp | void sync_version_activation()

`begin_activation` validates the visible keys, updates visibility and creates a nonzero increasing token. Every distinct visible search joins an existing job or queues a new check. A job that started during an earlier visit is still useful: joining it marks the new visit as attempted. The service does not start a second worker merely because a reveal occurred while the first worker was blocked.

Attention passes the current token to `ensure_checked`. Stale tokens and keys outside the current visible tab are ignored. A queued icon is promoted before completion collection can admit replacement speculative work. Running work is joined. A consumed attempt returns `already_checked`, including after failure, so hovering cannot create a retry storm. The next visit supplies the next retry opportunity.

@source src/version_discovery.cpp | VersionRequestResult VersionDiscoveryService::ensure_checked(const VersionSearchKey& key, Activation activation, VersionRequestReason reason) {

Crucially, **queuing does not consume an attempt**. Starting a job or joining one does. A never-started reservation can be discarded, then requested again during the same visit. This distinction lets visibility changes remove obsolete work without falsely claiming that a search was checked.

## Capacity protects attention

The default limits are 16 active discovery jobs in total, with four slots reserved for attention. They are not 16 speculative jobs plus four more. Activation admission requires total active count below 12; attention can use all 16 slots. Active attention jobs count toward both thresholds.

@figure scheduler | Activation fills up to twelve active slots; attention has priority and can use the remaining capacity up to sixteen total.

The admission loop chooses attention before activation, then FIFO order within a priority. Hover or explicit action brings an icon forward, but cannot displace a running synchronous call. If all sixteen jobs stall, another request remains queued.

@source src/version_discovery.cpp | void admit() noexcept {

The executor is externally owned. Its `start` contract either accepts a work item exactly once or throws before accepting it; inline execution is permitted. The service reserves capacity and records the attempt before calling the executor, so a test executor that completes immediately follows the same rules as a real thread. Jobs copy the scanner and key, owning everything they use. These are transient per-admitted-search jobs, not permanent threads attached to every icon.

## Results publish once and remain immutable

A job owns its `VersionDiscoveryAttempt`, including the scan observation and any start/scanner failure information. The worker catches scanner exceptions, preserving a failed attempt rather than allowing the exception to escape its boundary. It then stores `ready = true` with release semantics. Collection loads that flag with acquire semantics before reading the attempt. Everything read after publication is immutable.

@source src/version_discovery.cpp | VersionDiscoveryExecutor::Work work = [job, scan = scanner, search = key]() noexcept {

The attempt and catalog answer different questions: “what happened during the latest check?” and “which entries are safe to use?” A failed refresh updates the former while preserving the latter.

| Latest observation | Trusted snapshot after collection |
| --- | --- |
| Complete, changed entries | Replace snapshot; advance catalog revision. |
| Complete, identical entries | Keep snapshot and revision. |
| Complete, empty | Publish a known-empty catalog. |
| Incomplete or Failed | Keep the previous complete snapshot. |
| Worker exception or rejected start | Keep the previous complete snapshot and record failure. |

`nullptr` means no successful catalog has ever been published. A nonnull empty vector means the search completed and found nothing. Consumers must not collapse those states. Otherwise, a cold click could incorrectly report “Not installed” while a first check was still pending.

Collection uses an aliasing `shared_ptr` to the version vector inside the immutable attempt. It does not copy every version or allocate a replacement catalog during collection. Equality comparison suppresses revision changes when a scan confirms the same entries. Existing snapshot handles remain valid across later results and configuration removal.

@source src/version_discovery.cpp | record.snapshot = {attempt, &attempt->observation.versions};

Collection releases the service's job reference and active slot, then can admit queued work. An inline executor may complete newly admitted work immediately; `collect_ready` drains that publication too without waiting for asynchronous work. The runtime separately observes actual thread exit. A published result is not proof that native thread teardown has finished.

## Visibility, pins and tombstones prevent subtle races

When a tab hides, never-started unpinned queues are dropped. Running work remains owned and can publish later. An explicit cold user action may retain pending work with `retain_pending`; each successful retain needs a matching release. A pinned queue gains attention priority and survives tab hiding because the user already requested its eventual action. Retaining an idle or terminal search does not manufacture another attempt.

@source src/version_discovery.cpp | bool VersionDiscoveryService::retain_pending(const VersionSearchKey& key) {

Configuration removal cancels its reservations and pins. A running removed record remains as a private tombstone until its job is collected. Suppose configuration removes a Blender binding while its network scan is blocked, then readds the same key. Retaining that record allows the new binding to join the original job. Erasing it immediately would permit duplicate scans and lose the coordinator's accounting of accepted work.

There is a second race at a visibility transition. A worker may publish between the UI capturing terminal intents and beginning the next visit. Visibility must change before queued work is admitted, or an obsolete tab could consume a newly free slot. The UI first captures results without admitting old queues, then begins activation without an intervening publication drain. A newly published job remains attached long enough for the new visit to join it and for the original intent to terminate against the correct attempt. This preserves user intent without letting it follow some replacement check indefinitely.

@source src/version_discovery.cpp | VersionDiscoveryService::Activation VersionDiscoveryService::begin_activation(std::span<const VersionSearchKey> visible_keys, bool collect_published) {

These rules separate visibility, admission, execution, publication, trust and explicit intent. Native completion wakes the owner when work changes; there is no periodic discovery timer, hover-driven rescan loop or readiness polling task. The surrounding application has other event sources, but an unchanged catalog creates no scheduled refresh work.
