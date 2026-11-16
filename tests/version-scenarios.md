# Isolated version-discovery scenarios

The optional `LAUNCHER_VERSION_TEST_HOOKS=ON` build supplies
`LauncherVersionTest.exe`; ordinary builds exclude the controller, scenario
parsing and control endpoint. `BUILD_TESTING=ON` alone does not enable them.
The instrumented build requires an explicit absolute scenario path and an
explicit absolute fixture configuration. Its provenance identifies
`version-scenarios`, and it must never be accepted as the normal installation.

Use the separate version-test Release preset. The native driver creates its own
fixtures and leaves evidence under the requested output directory:

```powershell
& .\tests\version-scenarios-native.ps1 `
    -Launcher '.\build\windows-x64-version-tests\Release\LauncherVersionTest.exe' `
    -OutputDirectory '.\build\verification\version-scenarios'
```

The driver verifies normal close, a UI-observed exception, restart, retained
complete catalogs after partial/failed refreshes, and cold launch success/failure
across a page switch. An independent local search
and UI checkpoint must complete while another search owns a native directory
cursor and is held at its first `next` call. Close/error/restart tests leave that
worker held through the actual application cleanup and process-exit boundary.
The retained-catalog case adds a newer fixture version, injects partial/failure
outcomes, and captures the exact launch target and arguments to prove that the
previous complete catalog remains usable. The driver executes no configured
program and requests no elevation.

The cold cases click before the first catalog exists, show another page, then
release the worker. Success must capture the original action exactly once;
failure must capture nothing, including after a later successful activation.
Checks use logged service outcomes and explicit UI checkpoint barriers rather
than assuming a fixed delay made a race happen.

A gate is a controlled delay, not evidence of a naturally stalled network. To
exercise an explicitly owned SMB fixture directory as well, pass its UNC output
path and `-RealSMB`. Evidence records actual SMB coverage separately from the
simulated delay; the option requires a UNC path.

For a manual demonstration, create a dedicated directory containing:

- `LauncherVersionScenario.fixture`, with the first line
  `Launcher version scenario fixture v1` (LF and CRLF are accepted).
- An ordinary Launcher configuration and harmless versioned files beneath that
  directory. Use stock icons to keep the demonstration focused on discovery.
- A schema-1 scenario, such as [version-scenario.example.toml](fixtures/version-scenario.example.toml),
  replacing every absolute path with its owned fixture location.

Start it with `--version-scenario <absolute-scenario> --config <absolute-config>`.
The scenario's declared configuration must match that explicit configuration.
Searches, scenario/config files and evidence stay inside the marker's directory;
reparse-point fixtures are rejected. Unknown searches and attempts beyond the
explicit plan fail rather than reading an ambient installation.

For an explicitly owned removable-drive test, a search may set an absolute
`fixture_directory` outside the internal scenario directory. That existing
directory must have its own ownership marker. Volume/share roots, device paths,
wildcard roots, parent traversal and reparse ancestors are rejected. The search
must stay beneath that marked directory; configuration and output still remain
inside the original internal fixture. Captured actions are restricted to the
configured search fixtures. This allows the removable source to disappear while
logs and evidence remain available internally.

Real native notifications are disabled for scenarios by default. Set the root
boolean `notifications = true` only for an interactive notification demonstration.
The manifest records this choice. A `metadata` gate pauses before the native
file open, after directory cursors have been destroyed, so an owned removable
device can be safely ejected before the read resumes. Keep `outcome = 'native'`
and omit injected failure settings when claiming real device-removal coverage.
Use only the precisely identified non-system USB device and respect Windows'
safe-removal veto; never force a dismount or disable a hub/controller.

Each search has 1..32 attempt plans. `outcome` is `native`, `incomplete`, `failed`,
or `throw`. `incomplete` retains the observed native catalog as diagnostic
evidence; `failed` returns no catalog, and `throw` exercises the scanner exception
boundary. `fail_after_entries` injects a cursor error after that many returned
native entries across the attempt. `error` is the nonzero Win32 code (default
53). `gate=true` holds until its release Event is signaled; `gate_stage` selects
`before`, `cursor`, or `metadata`. Cursor gates own a real native Find handle.
Forced `failed`/`throw` outcomes use the `before` gate.

The controller writes `evidence/process-<PID>/manifest.json` (also TOML),
`trace.jsonl`, and an isolated application log. Every entered/release Event name
includes the fresh instance, process, search and attempt. Events have a protected
ACL granting access only to the current user and reject pre-existing names. Open
an attempt's `entered` Event to wait for its boundary, then set `release` when
ready. There is no production polling timer or controller thread.

The manifest's registered control message targets that process's verified
Launcher window. Put the command in the low 16 bits of `WPARAM`, an optional
page/item index in its high 16 bits, and the manifest PID in `LPARAM`:

| Command | Action |
| --- | --- |
| 1 | Close normally. |
| 2 | Throw at the outer UI loop, exercising normal owner unwinding. |
| 3 | Request the normal staged restart, preserving scenario arguments. |
| 4 | Select a page. |
| 5 | Flush `ui-checkpoint` evidence. |
| 6 | Activate an item through the normal captured action path. |
| 7 | Open an item's real native context menu for external input tests. |

The optional native menu driver uses actual mouse and keyboard input against
that instrumented window:

```powershell
& .\tests\version-menus-native.ps1 `
    -Launcher '.\build\windows-x64-version-tests\Release\LauncherVersionTest.exe' `
    -OutputDirectory '.\build\verification\version-menus' `
    -Probe '.\build\windows-x64\Release\LauncherLaunchGuiProbe.exe'
```

Run this from Windows PowerShell 5.1 on the interactive desktop. It guards input by the owned
process/window and foreground menu, and restores pointer/focus after cleanup.
Coverage includes Apps/Shift+F10, recursive Shell submenus and cancellation,
pagination, and frozen older-file actions after deleting the selected file and
adding a newer one. That file-race case does not force a new catalog publication
inside the open popup; service/adoption tests cover publication holds separately.
Admin routing is captured without requesting UAC. Properties dispatch is also
captured here; the separate `LauncherExactFileUiProbe` verifies an actual native
Properties dialog and exact Explorer selection on its own files.

`-Mode Default` exercises **Set as default** and **Always use latest**, including
ordinary icon activation, restart persistence, unavailable saved installations,
and restoring automatic selection. Preference commands must never launch a
target. This mode is also registered as `Launcher.VersionScenario.DefaultVersion`
in the isolated test build; it requires an interactive desktop with input access.
The driver verifies fixture executable paths across 32-bit and 64-bit hosts.

`owners-cleaned` is written after Window, COM and logger cleanup and before the
runtime's final worker-exit decision. A deliberately held worker can require the
documented self-termination boundary; this marker proves explicit cleanup ran
first. It does not claim that ordinary CRT/static teardown ran afterward.
