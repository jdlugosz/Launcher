# 1. The architecture in one pass

## Four objects with different jobs

The simplest useful model separates discovery identity, an attempt, a trusted catalog and a displayed presentation. Treating any two as interchangeable causes subtle failures.

| Object | Meaning |
| --- | --- |
| Search key | The canonical provider and locations that determine which files to discover. Several icons may share it. |
| Latest attempt | What the most recent admitted check observed, including coverage status, diagnostics or a start/scanner failure. |
| Last complete catalog | The most recent authoritative result. It can remain usable while a newer attempt runs or fails. |
| Icon presentation | One coherent choice of concrete action, catalog, label, capabilities and pixels that the UI currently displays. |

A search is not an action configuration. Two Blender icons can share a pattern while using different launch arguments. Adobe icons can share a catalog while selecting different products or beta policies. Generic icons currently choose the newest candidate automatically; older files are selected through the version menu. An unsuccessful refresh is not an empty installation set. A ready catalog is not permission to replace the target of a mouse press already underway. These distinctions account for much of the code beyond the filesystem loop.

@figure architecture | The normal request path separates UI policy from blocking work and separates catalog collection from presentation adoption.

## One request, end to end

Suppose the current tab contains a wildcard Blender icon. Revealing the expanded panel begins a shown-tab visit. The UI adapter supplies the distinct search keys on that tab to `VersionDiscoveryService`. The service either joins work already running for the same key or queues a new attempt, subject to its capacity and attention policy.

The native executor starts an owned work closure on a transient thread. The scanner traverses the configured path, reads optional metadata, orders the candidates and returns a structured observation. A job records that observation and publishes readiness with a release store. The native wrapper then destroys the work closure's captures and signals the shared completion Event.

The UI's ordinary message wait observes that Event alongside Windows input. Collection reads ready jobs with acquire semantics. A complete result may replace the immutable catalog. An incomplete or failed result updates the attempt record while preserving the previous complete catalog.

The UI then selects an exact candidate for each binding. A presentation worker prepares launch information, a label and icon pixels in its own COM apartment. When the desired generation finishes and no interaction holds adoption, the UI adopts the complete presentation together. A user action copies this concrete state, rather than asking the filesystem to resolve “latest” again.

@source src/main.cpp | void sync_version_activation()
@source src/version_discovery.cpp | std::size_t collect() noexcept
@source src/main.cpp | void bind_item_presentation(

## Responsibilities and dependencies

| Component | Responsibility |
| --- | --- |
| `target_versions` and `TargetVersionSource` | Generic path traversal, native probes/cursors, metadata, ordering and structured coverage errors. |
| `adobe_cc` | Adobe's provider-specific catalog layouts, product recognition and deduplication, feeding the same result policy. |
| `VersionDiscoveryService` | Owner-thread records, activation visits, admission, pending-action pins and complete-catalog retention. |
| Native executor | Thread ownership, completion signaling, native exit observation and the final process boundary. |
| `ItemPresentation` and Window integration | Blocking launch/image preparation, generation checks, coherent UI adoption and bounded caches. |
| Version menu session | Frozen version rows and exact-file actions through parent/child native menu tracking. |
| Error journal and `AppNotification` | Worker-side external-error records and UI-side notification policy/activation. |

The service has no Windows window, timer or drawing dependency. Its executor and scanner are injected interfaces. Conversely, the scanner knows nothing about selected tabs, menu gestures or cached Direct2D bitmaps. The Window adapter connects these concerns without moving its mutable state into workers.

## Why the legacy cache is not the main coordinator

The repository still contains `TargetVersionCache`, which represents the earlier background-refresh abstraction. Its compatibility tests and implementation can help explain the history, but current page coordination uses `VersionDiscoveryService`. A search through a file name alone can therefore lead a maintainer to a valid but inactive abstraction.

Start at the Window's service construction and `version_search()` adapter when tracing live behavior. Then follow the injected scanner into the generic or Adobe provider. The source map at the end of the book provides the shorter route through the current call graph.

@source src/main.cpp | std::make_unique<launcher::VersionDiscoveryService>
@source src/target_version_cache.cpp | bool TargetVersionCache::refresh(

## Core invariants

- Service records belong to one owner thread. Workers publish only through their own shared job state.
- A non-complete observation cannot silently replace the last complete catalog.
- One shown-tab visit consumes at most one attempt for a shared search, including a failed start or an already running attempt that the visit joins.
- A displayed action, label, catalog and image describe the same adopted generation.
- An accepted user intention retains its copied options and resolves against its captured attempt, rather than a later successful retry.
- Shared captures preserve object lifetime. Explicit runtime policy separately establishes safe process exit.

These invariants are more useful review criteria than a target line count. New functionality should identify which invariant it extends and which boundary owns it.
