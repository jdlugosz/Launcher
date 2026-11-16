# Launcher setup

Extract every file from the download ZIP. Its top level contains a versioned
`Launcher-R<version>-windows-x64` folder, `Install.cmd`, and `Portable.txt`.
Open `Launcher Setup.exe` in the versioned folder to install or upgrade.
The outer `Install.cmd` opens the same Setup and remains the entry point for
Shared Updater Client. For portable use, read
`Portable.txt`, then open the versioned folder and run `Launcher-R<version>.exe`.
Keep the outer wrapper beside the folder so it can find that exact version.
Windows PowerShell 5.1,
included with Windows 10 and 11, runs the wizard; no developer tools are needed.

Setup also installs the included `LauncherDrop.exe` and
`LauncherSvgExtractor.exe` companions and their guides. SVG Extractor is used
only when you run it; see `docs\SvgExtractor.html`. It requires .NET Framework
4.6.2 or later. Close an extractor running from the selected installation or
download before updating; setup leaves unrelated copies alone.

Setup checks both standard application folders and displays FOUND / NOT FOUND,
full paths, executable identities, and running copies. It recognizes the product
from its version resources without executing discovered files. This is product
recognition, not an authenticity or signature check. Unreadable, legacy, or
conflicting contents require review rather than being treated as absent.
Running copies are enumerated through native Windows process APIs, without
requiring CIM/WMI access. An unreadable candidate remains visible for review;
it is not treated as an absent process.

For a fresh installation, choose **All users** (Program Files, recommended) or
**Just me** (Local Programs). Launcher Setup requests administrator approval for
program files and Windows uninstall registration, even if the application folder
is already writable. If approval is declined or unavailable,
setup returns to review with the personal folder selected. Select Install again
to accept that destination. Other failures, such as invalid files or a full disk,
report their actual error.

An existing installation is upgraded in place. If both standard installations
exist, setup upgrades the personal copy and leaves the all-users copy alone.
An all-users upgrade that cannot obtain permission stops; it never creates a
second personal installation as a fallback.

The wizard has six pages:

1. Discovery summary, Refresh, and expandable details.
2. Installation scope or existing upgrade, with the full application folder.
3. The current account's sign-in Startup option.
4. Review, followed by Install or Upgrade.
5. Actual installation progress.
6. Completion, with an optional **Start Launcher now** checkbox.

The wizard remains in the initiating user's context. Its separate worker can
request elevation for program-file writes and machine-wide uninstall registration;
it does not change another account's
settings, shortcuts, or startup choices. Finish launches the installed copy from
the original wizard.

On a fresh installation, setup copies a portable trial's root TOML files
(configuration, matching runtime settings and palettes), except the bundled
`Launcher.default.toml` and `Launcher.installed.default.toml`, into the initiating
user's AppData Launcher folder.
It also copies files beneath `Icon` and `Icons`, preserving their relative
subdirectories. Other directory names are not searched. Absolute references
remain external dependencies. Once these files have transferred, the extracted
download can be discarded unless the configuration still refers to it by an
absolute path or `$EXE`.

The review page lists the files to transfer. Setup preserves an existing
AppData profile: if any scoped file conflicts or exists only in AppData, the
whole transfer is skipped with a warning. Identical files are retained. An
**Upgrade** never imports trial files, even when the installed profile is
missing a file. If trial files differ or were added, review and completion warn
that they are not copied and list their paths. Profile transfer uses the
initiating user's account after affected copies have saved their adjustments;
administrator approval applies only to the program installation.

After handling a portable profile, setup creates
`%LOCALAPPDATA%\Długosz\Launcher\Launcher.toml` from the installed
`Launcher.installed.default.toml` if that personal file is missing. This also
covers an upgrade with no default configuration. Existing configuration,
including a transferred portable profile, keeps its custom text; only exact
stock guide/demo entries are updated with a backup. This step runs in the initiating
user's account, separately from the elevated program-file worker. If it fails,
completion explains the problem and leaves **Start Launcher now** unchecked.

When setup closes several Launcher instances, Finish can restart all of them
with their individual configuration and launch options. Setup first asks every
affected instance for its restart arguments; if any cannot reply, none are
closed. Older versions without this support must be closed manually. A fresh
installation starts one default instance. Clearing the Start checkbox leaves
Launcher stopped.

If installation fails after closing instances, setup attempts to restart them
from their original paths after checking that the original executable bytes are
present. If recovery also fails, the error identifies where the saved restart
information was retained.

## Files and running applications

Setup validates a fixed payload and its hashes, stages the files, and checks
write access before closing affected Launcher copies. It closes only verified
Launcher processes in the source package or selected application folder, in the
initiating user's session, and waits for actual exit. Other installations and
portable copies stay running. A LauncherDrop copy blocking either folder must be
closed by its user, so uncopied text is not lost. Setup never force-terminates it.

Setup installs the versioned EXE and creates `Launcher.exe` as a hard link to it,
falling back to a verified separate copy if hard links are unavailable. Setup
removes the download marker from the verified Launcher executable staged for
installation. The extracted package keeps its marker. Personal
installation needs no symbolic-link privileges or Developer Mode. The active
alias entry is replaced without writing through the old link. Prerelease/beta
and special updates retain the previous executable. After a final release
activates, Setup removes verified older Launcher executables except for one
numbered executable or recognized backup in the installation folder whose numeric
version and SHA-256 match the previous build in the scoped update receipt.
A matching same-byte repair keeps that fallback. Accepting the review records
the decision and leaves the fallback in place for later cleanup.
A file-only install removes all verified older executables.
Equal or newer versions, personal settings, and unrelated files are preserved.
For example, when R1.3.0.5 is active, registered Setup for R1.4 keeps that build
and removes R1.2 and R1.3.0.4. Files with unrecognized identity or
mismatched filename/version metadata are left for manual review. A locked or
changed old file produces a cleanup warning. A versioned filename with different
existing contents is never replaced;
use the next package revision. Support files are staged and rollback is attempted
on a reported failure; the alias is published last. This is not a crash-atomic
transaction across every file.

Existing `Launcher.toml`, runtime settings, and edited palettes are preserved.
Missing bundled examples are seeded, while existing examples are retained. The
first release-to-development transition snapshots an existing personal
configuration once, before replacing the release. A later update does not refresh
that snapshot or invent one if no configuration existed at the transition.

## Portable first run

Open the versioned payload folder, keep its files together and double-click the
versioned main EXE. With
no configuration argument, it looks for `Launcher.toml` beside itself, then in
your AppData Launcher folder. If neither exists, a portable copy creates the
adjacent file from `Launcher.default.toml`. Setup normally prepares the installed
copy's AppData configuration before Finish. If an installed copy later cannot
find it, it offers to create the configuration in AppData or exit.

For an independent portable trial when AppData already has your normal profile,
copy `Launcher.default.toml` to `Launcher.toml` in the extracted folder first.
Edit that adjacent file and choose **Restart** to reload it. Runtime adjustments
use its sibling `Launcher.runtime.toml`. Put relative artwork under `Icon` or
`Icons` if you want fresh setup to transfer it with the profile. The `examples`
folder contains browsable palettes and is not searched automatically.

The starter configuration has **Launcher**, **Windows**, and **Examples** tabs.
It opens the included companions, guides, standard Windows apps and sample color
files. Its `$EXE` references follow the running program's directory in either
portable or installed use. Open `docs\Launcher.html` in a browser for the
offline guide and its configuration, launch targets, and appearance references.
Open the editable TOML examples in Notepad.

For a NOT FOUND destination, **Start Launcher when I sign in** starts checked.
For an existing installation it reflects the matching Startup shortcut.
Navigation and refresh preserve an explicit checkbox choice.

Setup seeds the color demo at
`%LOCALAPPDATA%\Długosz\Launcher\Demos\demo_many_colors.toml` before offering
to launch. The installed starter uses that AppData path for both running and
editing the demo. Runnable demo files and the CMD wrapper are omitted from the
installed folder; the EXE embeds defaults for later Windows accounts. The ZIP
retains portable examples. Existing demo
edits and saved runtime settings are preserved. `Launcher.installed.default.toml`
is the installed starter; portable use keeps `Launcher.default.toml`.

When upgrading an existing main configuration, setup updates only exact stock
Markdown guide links and demo entries. It keeps a backup before changing those
entries and preserves custom targets, unrelated text, comments and everything
after `%STOP`.

The original configuration is saved under the profile's `Backups` directory as
`Launcher.before-setup-<date>-<unique-id>.toml`. These backups are inactive.
The generated swatch icon is seeded in `Icons\many-colors.svg`, preserving an
existing edited icon. Older `Demos\ManyColors` data is copied to the flat demo
and runtime filenames; originals remain for custom configurations and recovery.

## Retained versions, repair and removal

Close Launcher and LauncherDrop from the selected installation before running
maintenance. Run these commands from its folder, or specify the full script path.
Every changing action requires an explicit `-InstallDirectory`:

```powershell
.\Maintenance.ps1 -Action List -InstallDirectory 'C:\Program Files\Długosz\Launcher'
.\Maintenance.ps1 -Action Rollback -InstallDirectory 'C:\Program Files\Długosz\Launcher' -ExecutableName 'Launcher-R1.0.exe'
.\Maintenance.ps1 -Action Repair -InstallDirectory 'C:\Program Files\Długosz\Launcher' -SourceDirectory 'D:\Downloads\Launcher-R1.1.0.8-windows-x64'
```

For `-SourceDirectory`, choose the versioned payload folder containing the EXE
and `Install.ps1`, rather than its parent containing `Install.cmd` and `Portable.txt`.

Rollback changes only the executable alias and retains the current one for
recovery. It leaves your profile intact. If the old executable needs its earlier
settings, supply `-ConfigurationPath` with the full path to a compatible saved
TOML, then use the displayed launch guidance. Runtime settings stay beside that
chosen TOML using its basename. The first release-to-development transition
saves `Launcher-R<release>.toml` and its matching sibling runtime when present.
The old 1.0 release needs an explicit configuration path because its historical
default AppData location differs.

Repair reinstalls from an extracted package through the same validated file
transaction. It preserves profiles and editable examples. To uninstall,
double-click `Uninstall.cmd` in the installation folder, or specify the exact
installation from a command prompt:

```text
Uninstall.cmd "C:\Program Files\Długosz\Launcher"
```

Setup also registers **Długosz Launcher** in **Control Panel > Programs and
Features** and Windows **Installed apps**. Choose **Uninstall** there to run the
installed `Launcher Setup.exe` for that exact installation. The existing
maintenance script handles removal, including administrator approval if needed
for an all-users uninstall. A **Just me** installation is
registered for the initiating account; **All users** is registered machine-wide.
Portable use creates no uninstall entry. Updates and rollback refresh the listed
active version; an update does not create a separate entry for every retained EXE.

Registered Setup adds **Launcher** and **Launcher Update History and Recovery**
under **Długosz** in the Start menu for the chosen scope. The **Launcher** link
opens `Launcher.exe` whenever you need it. The Start on sign-in option controls
the separate Startup shortcut.

The recovery link opens the same-scope Shared Updater Client even when Launcher
cannot start. If the client is absent, install it in that scope to use the
shortcut. The icon comes from the verified retained build if it provides recovery
artwork, or from the installed Launcher build. An upgrade moves a matching
recovery link from the earlier **Długosz Launcher** folder after verifying the new
link. Uninstall removes matching Launcher links and preserves the other entries
in the publisher's folder.

Both entry points show completion or explain why removal could not proceed.
Uninstall closes this installation's Launcher instances normally and waits for
them to finish saving and exit before removing files. Other installations stay
running. If Launcher cannot be closed safely, uninstall stops with an explanation.
Close Launcher Drop or SVG Extractor yourself first to preserve uncopied text
and unfinished scans. Personal profiles and edited or unrelated files are retained.

Uninstall checks the installation's ownership manifest and file hashes, removes
verified Launcher executables and unedited owned support files, and leaves
changed or unrelated files in place. It keeps all AppData profiles. The initiating
process removes only its account's matching Startup shortcut, including when it
is already elevated. Program-file removal can request elevation without
accessing the administrator account's settings.
Automatic shutdown runs in the initiating user's account before elevation; an
elevated file worker checks again that the installation is no longer running.
The matching Windows uninstall entry is removed after program-file removal
succeeds. An all-users uninstall requests administrator approval even when the
folder itself is writable.
The elevated file-only worker leaves Startup cleanup to that initiating process.
An absent shortcut requires no cleanup, and a shortcut to another installation
is preserved.

Startup is optional and applies only to the initiating account, including an
all-users program installation. Setup manages only `Startup\Launcher.lnk`.
An upgrade initializes its checkbox from the matching existing link. Clearing it
removes that installation's link; an unchecked option leaves another
installation's link alone. Explicit retargeting preserves arguments. Unknown or
changed shortcuts are not overwritten. If a shortcut operation fails after the
application files are installed, the completion page reports that separately;
the installed files are retained.

## Read-only checks and layout

`Install.cmd -DiscoveryOnly` prints real discovery as JSON without opening a
window or changing anything. `Install.cmd -ValidateUi` constructs all six pages
with synthetic findings, checks the choices and DPI layout, and disposes the
controls without displaying a window or installing files.

`-RenderDirectory <absolute-folder>` exports six PNG pages and `dpi-layout.json`.
`-RenderDetails` expands discovery. `-RenderDpi 96|120|144|192` exercises scaled
layout; the report distinguishes simulated layout DPI from actual window DPI.
Only the explicit render option writes those validation artifacts.

`Discovery.ps1`, `Deployment.ps1`, `Lifecycle.ps1`, and `Worker.ps1` keep discovery,
file replacement, application/shortcut lifetime, and worker coordination separate.
`Registration.ps1` manages the Windows uninstall entry and its installation scope.
`Processes.ps1` shares native process inspection between discovery and shutdown.
`WindowTextQuery.cs` is the reusable window-message client that reads each
application's synthesized argument string through a `WM_COPYDATA` reply.
`Presentation.ps1` reads the embedded Launcher icon and sets per-monitor DPI
awareness before constructing the window. `LauncherShippingCheck` tests the
actual staged scripts, archive contents, isolated file operations, process and
shortcut behavior, worker decisions, and wizard pages. It does not request UAC
or modify the normal installed application or personal profile.
