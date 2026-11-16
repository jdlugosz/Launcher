# 10. Testing time, failure and lifetime

## Tests need control of the boundary

The interesting failures in this feature depend on order. A worker can finish just before a click, a menu can retain an older file while a new catalog arrives, or the Window can disappear while a provider still owns a directory cursor. A test that merely sleeps for a convenient interval does not establish which event occurred first.

The implementation supplies narrow injection seams at the source, executor and full-application controller. Each seam controls a different part of the argument. Scripted sources make filesystem outcomes deterministic. Manual executors choose execution order and start failure. Real threads and native Events establish publication and lifetime behavior. The optional instrumented application exercises actual Window teardown and restart without replacing personal configuration.

@figure testing | Layered tests establish distinct contracts. A gate controls ordering while native adapters still perform real I/O.

## Scanner tests

An injected `TargetVersionSource` can report a missing candidate, unreadable metadata, a cursor error after some entries, or an exhausted resource budget. Its cursor uses the same end/error distinction as the native adapter. Tests can verify that a partial observation remains Incomplete even when it contains plausible versions.

Important scanner cases include multiple wildcard levels, literal targets, extended prefixes, wildcard/parent normalization, final-file reprobes, optional metadata absence, numeric ordering, path deduplication and distinguishing a missing descendant from a disconnected root. Adobe tests separately cover its recognized layouts, skipped links and repeated-installation identity.

@source tests/target_version_scan_tests.cpp | TEST_CASE(
@source tests/target_versions_tests.cpp | TEST_CASE(
@source tests/adobe_cc_tests.cpp | TEST_CASE(

## Service and runtime tests

A manual executor can run accepted work inline. The service must reserve capacity and establish its job before entering that executor, rather than assume that `start()` returns before completion. Another executor rejects start before acceptance. This verifies that failed starts consume the current visit and that capacity is restored correctly.

Service tests cover joining work across visits, hidden queues, attention promotion, complete-empty replacement, retained catalogs, pending-action pins and removed-running tombstones. Real-thread tests additionally check independent progress and destroying the service before a worker finishes. Those tests exercise owned publication state rather than trying to prove lifetime safety through a final boolean alone.

The native runtime tests separate a published result from the thread's actual exit. They hold work captures or the native tail long enough to show that a ready flag does not make the thread safe to join. Event-reset interleavings exercise the wakeup protocol. Child-process tests establish the blocked-worker final boundary and failure handling without terminating the test runner.

@source tests/version_discovery_tests.cpp | TEST_CASE(
@source tests/version_discovery_runtime_tests.cpp | TEST_CASE(

## The instrumented application

`LAUNCHER_VERSION_TEST_HOOKS=ON` builds `LauncherVersionTest.exe` through the separate version-test preset. `BUILD_TESTING=ON` alone does not enable the controller. Ordinary applications exclude scenario parsing, injected outcomes and the control endpoint. The instrumented binary declares its provenance and must not become the accepted normal installation.

Every scenario supplies explicit absolute configuration and scenario paths and an owned fixture marker. Evidence stays under that fixture. A search can explicitly name a second marked fixture on a removable drive while the configuration and logs remain internal. Device paths, volume/share roots, wildcard roots, parent traversal and reparse ancestors are rejected for those external fixtures.

Attempt plans can pause before scanning, at a real cursor, or just before metadata I/O. They can inject an error after a known number of entries or force an incomplete, failed or throwing outcome. Fresh named Events include the instance, PID, search and attempt. Protected ACLs restrict them to the current user, and pre-existing names fail rather than attach to someone else's object.

Captured dispatch receipts record the exact target, arguments, working directory, show mode and verb without executing configured programs or requesting elevation. A timeout is a watchdog. The entered/released Events and UI checkpoints establish the order that a test actually exercised.

@source tests/version-scenarios.md | The optional
@source src/version_scenarios.cpp | std::shared_ptr<VersionScenarios> VersionScenarios::load(

## What the retained evidence establishes

| Evidence layer | Established behavior |
| --- | --- |
| Scanner, Adobe and service tests | Native semantics, structured coverage and service-state contracts. The completion record reports 94 cases and 1,316 assertions for this selected group. |
| Presentation and overlay tests | Nine presentation cases / 110 assertions, two overlay cases / 1,051 assertions, plus native attention/completion image comparisons. |
| Exact target and native menus | Exact-file preparation and Properties API checks, separate real Properties/Explorer selection, keyboard context input, recursive cancellation, pagination and captured older-file actions. |
| Full application scenarios | Close, Cold, ColdFailure, Fatal, Restart and Retained modes. Independent work progresses while another worker remains held through actual owner cleanup. |
| Controlled real SMB | Close, Retained and Cold modes use actual SMB I/O with a controlled provider gate. They do not establish a naturally stalled or failing NAS. |
| USB and notification follow-up | Genuine safe-removal error, real sharing errors, retained catalogs and internal journals. Native notification history and direct warm/cold activation routes work. Actual replacement-toast clicking remains unverified. |

These counts describe retained focused runs, not a claim that every test has been rerun for this documentation edition. The final completion review and delivery record supersede earlier checkpoint files that still say instrumentation was pending. A retained earlier failed SMB run also remains in the evidence directory; the final successful run is the one identified in the source/evidence map.

## Gaps remain explicit

Administrative routing was captured without a real UAC interaction. The native menu race removed an older file and added a newer one but did not force a catalog publication inside the open popup; separate service/adoption tests cover publication holds. Visual checks cover selected sizes and DPI settings, rather than an exhaustive high-contrast and monitor matrix. Provider delays were controlled, and finite capacity can still fill with permanently blocked calls.

The acceptance scripts hash personal TOML files before and after installed-build acceptance. They launch normal use through desktop Explorer, because a process created from the Codex package context can see redirected profile files. That is an environment-specific acceptance requirement, rather than part of the discovery algorithm.

For a subsequent code change, choose the tests that challenge the affected contract. A scanner comparator edit needs ordering cases. An Event sequencing edit needs real interleavings and runtime tests. A menu snapshot edit needs an exact-file race. Repeating unrelated combinations adds less confidence than establishing the relevant ordering once.
