# About this guide

This is a maintainer's guide to Launcher's background version discovery. It explains the delivered C++ implementation, including the reasons for its boundaries and the conditions under which those boundaries matter. It is intended for a reader who knows C++ and ordinary Windows programming but has not followed the feature's development.

The feature begins with a familiar request: replace the changing part of a file path with a wildcard and offer the installed versions. Its implementation also has to keep the window responsive, preserve known results after a filesystem error, protect an in-progress click from a newly discovered target, and finish process shutdown while a provider may still be blocked. Those are separate responsibilities. Reading them as separate responsibilities makes the additional code easier to understand.

## What the book describes

This guide describes the implementation as of 20 October 2026, including the discovery service, presentation workers, exact-file version menus, external-error journal and persistent Windows app notifications.

The earlier `version-discovery-design.md` remains useful design history. It contains an implementation record followed by original proposals and checkpoint notes. Some later headings still describe work as proposed even though the delivered implementation and its completion evidence supersede those passages. This guide gives the current source precedence. In particular, the process-exit boundary and native version-row menus are implemented, rather than outstanding prototypes.

The chapter directives name source files and excerpts from that edition. The configuration and pseudocode examples are marked as illustrations. Check the current source for later changes.

## A reading route

Read chapters 1 and 2 for the model and vocabulary. Chapters 3 through 5 cover the scanner, service and native execution boundary. Chapters 6 and 7 explain why a completed scan does not immediately retarget every visible icon. Chapters 8 and 9 cover diagnostics and shutdown. Chapters 10 through 12 provide testing, maintenance and reference material for subsequent code changes.

For a focused code review, begin with the four-object distinction in chapter 1, then trace one request using the source map in chapter 12. For a bug investigation, use chapter 11's symptom table to identify the responsible boundary before adding logging or changing timing.

This is implementation documentation, rather than a record of every development experiment. The evidence chapter distinguishes a controlled gate from a naturally stalled device, a captured administrative action from a real UAC interaction, and a native activation callback test from an actual click on a Windows notification.

## Scope and terminology

An installation can be an executable, a shortcut, or a versioned document. The `TargetVersion` member named `executable` also stores concrete non-executable file paths. A catalog is a collection of these discovered candidates. A binding is the association between an icon's configuration and a shared search. A visit is the interval represented by one active, shown-tab activation token.

The book uses “owner” for the UI thread that calls the discovery service. The native executor also performs owned background launch and presentation work. A “worker” therefore describes an execution context, while a “discovery job” specifically describes one admitted search attempt.

The current product runs synchronous native filesystem operations on transient threads. It does not implement overlapped directory enumeration, coroutines, a permanent thread attached to every icon, or periodic version refreshes. Its finite limits constrain admitted work and accounted data. They do not impose a deadline on a blocking provider call.
