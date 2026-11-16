# 5. Runtime ownership and publication

The scanner is synchronous. Given a search and a source of filesystem information, it traverses the requested paths and returns one detailed observation. Keeping that operation synchronous makes traversal, resource ownership, and testing straightforward. The application supplies concurrency around it, where it can also decide which work deserves a thread and when a completed observation may affect the screen.

Three components divide that responsibility. `VersionDiscoveryService` owns scheduling and catalog policy on the UI thread. `NativeVersionDiscoveryExecutor` owns native threads and their completion signal. A private shared job carries one attempt between those owners. The service does not know about Windows messages; the executor does not know about icons, tabs, or service records.

@figure lifetime | The service can disappear while accepted work retains its own inputs. Native thread ownership remains with the application runtime.

@source include/launcher/version_discovery.hpp | class VersionDiscoveryExecutor
@source include/launcher/version_discovery_runtime.hpp | class NativeVersionDiscoveryExecutor final

## One owner for mutable policy

A service record holds configuration membership, visibility, queued/running work, attempts and trusted catalogs. Every service method runs on one owner thread; a worker never edits that record. The scheduling rules in chapter 4 therefore remain an ordinary sequential state machine, even while providers block independently.

The native adapter creates a transient thread for each admitted job. The service's 16-job limit bounds discovery admission; it is neither a permanent thread per icon nor a fixed thread pool. The runtime separately owns accepted thread handles until their native exit can be observed.

@source src/version_discovery.cpp | struct Record final
@source src/version_discovery.cpp | void admit() noexcept

An activation token governs retries and queue retention, not thread lifetime. Hiding can discard speculative reservations, but already accepted work retains its inputs and can publish after its service record or Window has disappeared.

## The publication boundary

A worker owns a copy of the scanner callable and search key. Its shared `Job` owns the attempt being written. Standard and nonstandard scanner exceptions are contained there and become a failed attempt. On completion, the worker publishes the job with a release store. The owner reads readiness with acquire semantics before accessing the attempt.

@code src/version_discovery.cpp | // Everything read by the owner is immutable after publication. | 2 | The result becomes immutable before the release publication.

This atomic handoff establishes visibility of the result's C++ memory. Reference counting supplies lifetime, while release/acquire supplies the ordering that makes the completed contents safe to read. Neither replaces the other. The owner subsequently exposes a const attempt and can create an aliasing `shared_ptr` to the vector within it, keeping the whole attempt alive without copying its catalog during collection.

Only a successful Complete observation can replace the authoritative catalog. Incomplete and Failed observations still become the last attempt, but leave the prior complete snapshot intact. Complete with an empty vector is authoritative known-empty. An unchanged complete vector retains its existing snapshot identity and revision, avoiding an unnecessary icon rebuild.

@source src/version_discovery.cpp | std::size_t collect() noexcept

## A persistent doorbell, with no polling timer

After `Work` returns, the executor releases its arbitrary captures and signals a shared manual-reset Event. The Event announces that something may need collection; it does not contain the result. Several workers may coalesce into one signal. The application waits for that Event and Windows input together, using an infinite wait when idle. Dispatch batches contain at most 64 messages so sustained input cannot indefinitely postpone completion collection.

@figure publication | Result publication and the Windows completion signal have different jobs. Resetting the Event before inspection prevents a lost wakeup.

@source src/version_discovery_runtime.cpp | // Release arbitrary Work captures while the runtime is intact.
@source src/main.cpp | const DWORD ready = MsgWaitForMultipleObjectsEx(1, &completion, INFINITE

The owner resets the Event **before** inspecting ready jobs. Consider the two races. If publication preceded the reset, the following acquire inspection finds the result. If publication follows an inspection that saw no result, the worker's subsequent `SetEvent` causes another wake. Publication between reset and inspection is also safe: collection finds it, and a later signal merely causes a redundant drain. Resetting after collection would instead risk erasing a signal from work completed after its ready flag was inspected.

@code src/main.cpp | void collect_background() | 11 | Completion is reset first, then the application drains its independent result owners.

The signal carries no HWND or message-queue pointer, avoiding recycled-window races. During a native modal menu loop, the outer wait may pause; the Event remains signaled until collection resumes. Native thread exit is a separate observation, explained in chapter 9.
