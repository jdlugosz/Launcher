# Windows restart arguments and window text replies

`include/win32/window_text_reply.hpp` and `src/window_text_reply.cpp` provide a
small Windows transport independent of Launcher configuration, window classes,
process discovery, or shutdown policy. They depend only on the Windows SDK and
the C++ standard library. `WindowTextReply` is configured with a registered
message name and an owned, immutable response string. Other applications can use
the same code with their own response. `tools/installer/WindowTextQuery.cs` is an
independent managed client for the same protocol.

## Request and response

The requesting thread creates a temporary message-only receiver window and
sends the registered request to a particular application window:

* `wParam`: the receiver HWND.
* `lParam`: a nonzero correlation cookie.

The application sends a second, synchronous message back to that receiver:

* Message: `WM_COPYDATA`.
* `wParam`: the responding application HWND.
* `COPYDATASTRUCT.dwData`: the request's cookie, echoed unchanged.
* `COPYDATASTRUCT.cbData`: the UTF-16 string's byte count, including its NUL.
* `COPYDATASTRUCT.lpData`: the string, without embedded NUL characters.

The maximum payload is 32,767 UTF-16 code units including the terminating NUL.
An empty response is valid. The receiver matches the window, process and cookie,
validates the byte count and terminator, and copies the text before returning
`TRUE`. The application returns `1` from the original request after an accepted
reply, or `-1` if the reply failed. An unsupported request normally returns `0`.

These cross-process sends use timeouts. Windows does not enforce the timeout
when a window shares the caller's input queue. The query deliberately omits
`SMTO_BLOCK`: the requesting thread must dispatch the reverse sent message while waiting inside
its original send. No text or pointer is returned through `LRESULT`, and the
receiver never retains the system-owned `WM_COPYDATA` buffer.

Query and response run between the ordinary application and ordinary setup
wizard. The elevated file worker does not receive replies or launch programs.
The window/cookie checks associate a reply with its outstanding query; they are
not a security boundary against other processes running as the same user.

## Application policy

The shared request name for restart arguments is
`Dlugosz.RestartArguments.v1`. Its response is the complete synthesized argument
string, **without the executable name**. The updater chooses the replacement
executable. The transport does not parse or alter this string.

Launcher configures its response after successful initialization. It includes
the resolved absolute `--config` path when the configuration was explicitly
named, plus persistent test/scenario options when applicable. Implicit default
configuration selection remains implicit. Relative application paths must be
resolved when synthesizing arguments so the response can be used from a new
working directory. Logging follows the selected configuration's `[logging].file`.

The Restart menu uses exactly the same configured string. Its process handoff
adds fresh private `--restart-wait`, `--restart-ready`, `--restart-cancel` and
`--restart-error` inherited handles. Those temporary handles are omitted from
the configured response, as are one-shot commands such as stop, preview and
smoke test. Restart closes only its own instance after its replacement is ready.

## Installer lifecycle

Before starting its file worker, the wizard pins and verifies every affected
process, then collects all argument strings. A failed or unsupported query
stops the operation before any instance is closed. Older versions that do not
implement this request must be closed manually before upgrading.

Immediately before committing files, the worker rechecks the process identity
against the captured PID, executable path, owner, session and creation time.
A new or changed affected instance blocks shutdown before any close. Processes
that have already exited are skipped. The worker reports which processes it
actually stopped, including completed closes preceding a later failure.

If installation fails after a close, the ordinary wizard attempts to restore
those instances from their original paths. It first checks each executable
against the SHA-256 recorded before shutdown, so an incomplete rollback cannot
silently launch different bytes. Other recovery attempts continue if one fails.
Unresolved recovery leaves the saved request and result files in place and the
error dialog identifies their directory.

On successful installation, Finish requests one replacement for each stopped
instance, using the saved argument string unchanged and the installed executable.
It never merges instances just because they use the same EXE. If no instances
were stopped, the checked Start option starts one default instance. Clearing
that option leaves them stopped. When Setup launches directly, each successful
launch is removed from the pending list so retrying Finish after another launch
fails cannot duplicate it.

Source and selected-destination processes remain the only affected instances;
other installations and portable copies are not closed. `Launcher.exe --stop`
continues to close every matching instance from its own executable path and
does not select instances by configuration.

## First launch after an Updater install

Updater can opt in to the Finish handoff by writing
`.launcher-updater-watch-startup` next to the staged `Install.cmd`, with the
same transaction ID as `.launcher-updater-transaction`. After a version change
commits under that ID, the ordinary wizard publishes a UTF-8 JSON file named
`.launcher-updater-startup-plan.json` in the same directory. Schema version 1
carries the transaction, selected scope, and exact ordered restart arguments.
An empty array leaves Launcher stopped. An array containing one empty string
requests a default launch. Updater derives the executable path from the verified
installation receipt. Standalone Setup and older Updaters launch directly.

Updater passes a private ready event and diagnostic pipe as
`--update-ready` and `--update-error`. Launcher signals ready when its main
message pump handles the startup notification after initializing and rendering
the dock. This works with a hidden drawer. A fatal settings error sends a
`settings` line followed by the diagnostic over the pipe in UTF-16LE. When the
loader identifies an on-disk source file, it can insert a
`Settings file: <absolute path>` line and a blank line before the diagnostic.
If the parser identifies a source position in that file, a
`Settings location: <line>:<column>` line and a blank line follow the file
header. Both coordinates are one-based. Updater uses those fields for its
**Edit** action. Other handled startup failures
send a `startup` line. Launcher closes both handles after reporting the outcome.
**Start again** repeats the watchdog for the failed launches of the same
verified update. Ordinary launches continue to report their own errors.
