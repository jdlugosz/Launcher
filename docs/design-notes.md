# Agreed design direction

These notes retain earlier milestone decisions and later design directions.

The stacked-tab milestone followed release 1.0.0. The version-discovery
feature has its own [detailed design](version-discovery-design.md). The
milestone sections below record decisions from earlier implementations.

- Windows 10 and 11; native x64; Microsoft's supported C++23 mode.
- One dock initially, attached to a configured edge of a chosen display.
- Float over ordinary windows and suppress the dock during full-screen use
  on its display. Collapsed state shows the tabs, with a short dwell revealing
  the panel. Hovering must not steal keyboard focus.
- Preserve the supplied reference's pastel colors and sloped tab appearance.
  Draw shapes and text at the current monitor DPI rather than stretching one
  background image.
- Rows run parallel to the screen edge and tabs. The top layout stays
  unchanged; the right edge rotates the full layout 90 degrees clockwise and
  the left edge rotates it 90 degrees counterclockwise. Tab shapes, label text,
  and cell positions rotate together, while icon bitmaps remain upright within
  those cells. The bottom edge keeps upright, horizontal tab labels.
- Rows and columns are global to all tabs, with automatic counting available.
  Automatic dimensions normally use the largest tab. Per-tab sizing may be
  added as an option.
- Items are sequential. A spacer consumes one cell. A break advances to the
  next row in row-major flow or next column in column-major flow. A break at
  an existing boundary is a no-op; a trailing break does not grow the grid.
- Show an item's configured label after the pointer dwells over its icon while
  the panel is showing. Use `appearance.tooltip_delay_ms`, default 700, as an
  integer from 0 through 5000 milliseconds. Every new icon starts a fresh delay.
  Keep native tooltips upright on every edge, wrap long labels, and display
  Unicode and ampersands literally. Preserve foreground focus. Dismiss on
  pointer departure, collapse, full-screen suppression, menus, adjustments, or
  dragging; empty cells and spacers have no tooltip. Changing the selected tab
  dismisses the old page's tooltip and registers the new page's icons.
- Left-click a tab to select its icon page. The active tab uses its full name;
  inactive tabs use their configured short name when present. A tab click does
  not start an along-edge drag or launch an icon. Selection is session-only:
  startup begins on the first configured tab, with no selection key or write
  to the base/runtime settings files. Keep the shared grid dimensions.
- Tabs may wrap into multiple rows parallel to the edge. An inactive tab may
  show a short name or icon; the active tab shows its full name. In the showing
  state, hovering a compact tab should explain its full name with a tooltip.
  Expansion should not cause unstable wrapping under the pointer.
  Move the whole selected row next to the panel, preserving the relative order
  of the other rows. Keep each sloped head and label at the compact row height,
  with a straight-sided body reaching the panel. Paint outer rows first and
  give clicks to the visible foremost tab, including exposed outer bodies.
- Use the same logical tab widths and wrapping for all edges. Rotating a
  side-edge layout retains the same collapsed strip depth, derived from the
  current font metrics and tab padding, with label text following the edge.
- Keep nominal text and icon sizes in the base configuration. Sample desktop
  icon size once at startup unless explicitly configured. Offer separate text
  and icon/panel factors and an integer physical-pixel spacing control in a
  native adjustment dialog with live preview and Apply/OK/Cancel/Reset. Save
  runtime settings in a sibling TOML file whose comments explain merging them
  into the base configuration.
- Use one `grid.icon_spacing_px` integer from 0 through 1024, default 8, for
  both inter-icon canvas gaps and outer icon-canvas-to-panel margins. Zero is
  flush. Preserve this physical pixel count across DPI, text-scale, and
  icon-scale changes; icon scaling changes the panel through icon size only.
  Runtime spacing is an absolute override copied into the base as-is, while
  text and icon factors continue to multiply. Resolve legacy DIP padding and
  base/runtime spacing percentages once at the actual display DPI, then save
  the absolute pixel form. Legacy panel padding no longer determines margins.
  Reset sizes restores the base gap and identity runtime text/icon factors,
  preserving the position and base factors.
  Fitting oversized content must preserve the physical-pixel gap; reject a
  layout if the fixed gaps alone cannot fit. An invalid live preview restores
  the previous valid sizes. Use the same fitting policy in native smoke tests
  and verify inter-icon gaps plus all four outer margins.
- Express nominal tab text as `appearance.text_size_pt`, defaulting to 9
  points. Request the generic Segoe UI Variable typographic family at weight
  600, enable automatic optical sizing, and fall back to Segoe UI when the
  requested family is unavailable. Convert points to DirectWrite DIPs once
  using 96/72; the monitor-DPI transform then supplies physical scaling. Keep
  nominal icon size in DIPs and icon spacing in physical pixels. Preserve
  legacy `text_size_dip` by converting it to points, and reject configurations
  specifying both text-size fields.
  Base and runtime text factors continue to multiply the nominal size.
- The archived desktop-scale Stardock image informed tab sizing.
  At 120 DPI, 9 points gives a 15-pixel em and a tab about
  23 pixels high. Derive tab dimensions from the effective font em, with total
  vertical padding of 0.2 em added to the measured line height and pixel-snapped
  height. Text adjustments and explicit nominal point sizes must affect the
  tab geometry consistently. Icon spacing remains an independent physical-pixel
  grid setting. The slightly different active/inactive tab heights remain
  deferred detail work; stacked connections and active-row ordering are in the
  current unreleased milestone.
- Drag blank panel space only along the selected edge. Save a fraction of the
  available travel, preserving placement across display or size changes. An
  explicit drag may activate the launcher for native capture and Escape; hover
  continues to preserve foreground focus.
- Icons normally come from the launched program or shortcut, retaining a
  shortcut's customized icon. Allow an explicit image or resource override.
- Give an item an optional launch action through flat `target`, `arguments`,
  `working_directory`, `show`, and `handler` fields. `target` is required for
  an action; existing icon-only entries remain placeholders. Default `handler`
  to `shell`, with executables, console applications, shortcuts, documents,
  folders, and protocols dispatched through ShellExecuteEx. Default `show` to
  normal, retaining visible consoles; permit hidden and minimized requests.
  Pass arguments as a raw Windows string, with no implicit CMD or PowerShell
  wrapper. Preserve unknown handlers in the model but report them as
  unsupported at dispatch until a provider exists.
- Adobe entries use `kind = "adobeCC"` and a product-family `target`. Discover
  locations below HKLM / Software / Adobe and Adobe uninstall registrations in
  both registry views, including custom drives, with native Program Files /
  Adobe as a fallback. Registered families can identify main executables beyond
  the legacy filename list. Read their
  version resources, normalize calendar-year suffixes, and distinguish similarly
  named products. Ordinary clicks choose the newest numeric Product Version
  (then File Version), including Beta by default. `include_beta = false` limits
  that choice to stable versions; explicit `(Beta)` targets select Beta only.
  Right-click lists every installed version, with Beta marked, and carries the
  configured arguments, working
  directory and visibility into that launch. Icons and tooltip versions follow
  the ordinary-click choice. Activating an already-running older version first
  remains a future enhancement; current dispatch chooses the newest executable.
  A successful catalog with unmatched configured Adobe targets opens a temporary
  commented, pasteable Adobe tab in Notepad once per unmatched target/session.
  `--adobe-cc-list` opens that report on demand; `--adobe-cc-list-output <file>`
  exports it without opening an editor or loading a configuration.
  `root_path` on an Adobe item accepts a directory or list of directories, adding
  per-product search hints to the automatic roots. Cache keys distinguish these
  normalized root sets; saved active-version identities retain the authored
  roots. Extra trees are scanned without following links and pruned once a main
  executable is found. Executable resources establish product identity/version.
  An Adobe `target` ending in `.exe` selects files by filename (with `*` and
  `?` supported) across those same roots, independent of ProductName. Explicit
  filename scans have distinct cache identities and accept missing metadata.
- Scan Adobe installations asynchronously at startup, cache the catalog in
  memory, and refresh it every 60 seconds and on Adobe page/click/menu demand.
  Coalesce demand while a scan is pending; clicks, menus and page rebuilding
  otherwise read the current cache without scanning on the UI thread. Queue a
  click received during the initial scan with its original item until results
  are available. Tooltips distinguish discovery in progress from not installed.
  Defer changed-catalog adoption while a menu, mouse press, drag, other interaction
  or taskbar transition is active. Unchanged results must not recreate icons or
  disturb tooltips. No persisted cache or new settings are required.
- Begin with a native Adobe resolver. Future external providers can use a
  versioned protocol if concrete callback needs arise; a public COM extension
  surface is not part of the first milestone.
- Keep rendering event-driven, cache icon extraction, and put slow discovery
  work away from the UI thread. Provide bounded logs and deterministic test
  hooks.
- CMake plus Ninja with vcpkg manifest dependencies. Preserve explicit Debug,
  optimized-with-symbols, Release, and future PGO-generation/use workflows.
- Use a private Windows SDK C++ PCH for each native GUI/probe target and
  configuration. CMake force-includes its generated wrapper, and each native
  source explicitly includes the SDK PCH header first. Keep project headers,
  explicit standard-library includes, toml++, and Catch2 outside it; local
  header edits should rebuild consumers without invalidating the SDK PCH.
  Keep stable macro definitions in the shared build flags. Clean and reconfigure
  when changing the SDK/toolset; header-list and compiler-flag changes rebuild it. Keep the core
  and Catch2 tests on ordinary includes. A later `import std` experiment must
  place imports outside the PCH.
- Keep editable settings separate from the bundled `Launcher.default.toml`.
  The shared [file-location policy](file-locations.md) covers explicit profiles,
  portable EXE-adjacent settings, AppData and final embedded resources. Runtime
  settings stay beside the selected profile. Builds and updates can change the
  bundled starter but must preserve existing user settings; an explicitly named
  missing configuration is never bootstrapped.

## Installer and portable distribution

Agreed direction, updated 2026-10-23. `LauncherStage` and `LauncherZip` build a
clean extracted-download folder and its ZIP from the explicit install payload.
Installer source is under `tools/installer/`; its six-page GUI performs read-only
installation/process discovery and uses recognized existing installations to
select an upgrade from the second page onward. It displays the findings for
validation. Its worker stages and verifies the fixed application payload before
gracefully closing affected Launcher copies and replacing program files. The
wizard handles permission fallback, profile snapshots, Startup shortcuts and
final launch in the initiating user's context. The large artwork is read from Launcher's embedded icon; no
loose icon is shipped. Native DPI awareness is set before creating the window,
separately from WinForms control scaling. `LauncherShippingCheck` verifies the
payload, archive, discovery decisions, isolated installation operations and wizard pages. The
published 1.0.0 ZIP remains an extract-and-run package.

### Portable placement

The user chooses the directory holding the extracted program files. Locate
bundled files relative to the running executable, regardless of that directory's
name or the process's current working directory. No particular Program Files or
AppData Programs location is required to run Launcher.

Launcher and LauncherDrop use the same search context. An explicit selected
configuration directory comes first, followed by the portable executable
directory when applicable, then AppData, then applicable embedded resources.
Portable copies honor real standard and custom palette files beside the
executable before embedded resources. Installed executable directories are not
automatic roots for editable configuration, palette or icon files; use AppData
or an explicitly selected profile directory. The bundled `Launcher.default.toml`
is an immutable program file read by its exact executable-adjacent path when
seeding a new configuration.
Deduplicate directory roots by filesystem identity, keeping the last confirmed
occurrence. An uncertain or missing directory is conservatively retained.

Installed mode means the executable is in the recognized
`%ProgramFiles%\Długosz\Launcher` or
`%LOCALAPPDATA%\Programs\Długosz\Launcher` location; anywhere else is portable.
Do not infer mode from writability or configuration location. A portable copy
with no discoverable default configuration seeds `Launcher.toml` beside its
executable. An installed copy instead offers Create in AppData or Exit.
Explicit missing files are fatal and never seeded. Runtime files always remain
beside the selected configuration, with the matching basename.
The complete lookup, source logging and error rules are maintained in
[Configuration and file locations](file-locations.md). Future includes will use
the same context, but includes are not implemented. Portable first-run and
maintenance instructions are included in the download README and installer guide.
The published 1.0.0 keeps its original
`%LOCALAPPDATA%\Launcher` behavior. Preserve user files during program updates.

Fresh installation adopts root TOML files (except `Launcher.default.toml`) and
relative assets beneath `Icon` or `Icons` into the initiating user's AppData
profile, preserving subdirectories. Conflicting or destination-only profile
files prevent the whole transfer. Upgrades preserve the installed profile and
never fill its gaps from the extracted trial folder; review and completion warn
about differing or added trial files that will not be copied. See
[Configuration and file locations](file-locations.md) for the transfer scope.

### Installation scope and upgrades

Offer both installation scopes, retaining the vendor/application namespace:

- **All users (recommended):** `%ProgramFiles%\Długosz\Launcher`. Select this by
  default for a fresh installation. Request administrator approval for program
  writes when needed; ordinary Launcher use remains unelevated.
- **Current user:** `%LOCALAPPDATA%\Programs\Długosz\Launcher`. Always offer this
  choice for a fresh installation; it should work without administrator approval.

Before choosing a destination, detect existing Launcher installations in both
locations, independently of whether Launcher is running. If one exists, select
an upgrade of that installation, retaining its directory and scope. If both
exist, target the current user's Local Programs installation and leave the
Program Files installation alone. The machine-wide copy may be an older,
corporately managed installation; a personal update must not replace it.
Show both locations and their dispositions. Do not offer a new installation as
the normal alternative to an upgrade. If neither exists, offer the two scopes
with Program Files selected.
An explicitly selected existing directory can identify an installation placed
elsewhere; moving an existing installation is a separate operation.

For a fresh installation, switch to the original user's Local Programs location
when elevation is prohibited or cannot be obtained, or when permissions prevent
creating/writing the Program Files destination. Explain the changed destination
on the review page before proceeding. Cancellation of the wizard still cancels
setup. Other failures, such as a full disk or invalid payload, must report the
actual error rather than trigger this permissions fallback. If the destination
is an existing Program Files installation, inability to upgrade it stops setup;
it must not silently create a second installation under Local Programs.

PowerShell directory creation does not request elevation automatically. Keep
discovery and the wizard in the ordinary user's context, and explicitly start a
limited elevated installation helper when needed. Capture the initiating user's
SID, profile paths and startup preference before elevation, so administrator
credentials for another account do not redirect personal settings or shortcuts.

### Running copies during setup

Collect running `Launcher*.exe` candidates, assuming the user retained the
program's name. Use native Windows process enumeration and inspection so setup
does not require CIM/WMI access. Quietly omit any candidate whose executable filename contains
`backup`, case-insensitively, from both directory and process discovery. A folder
name containing `backup` does not disqualify the executable. Record the full
executable path, process identity and user/session,
then classify each copy as the selected installation, the extracted source
package, another known installation, or elsewhere. Confirm the version resource's
`ProductName` is exactly `Długosz Launcher` and its `LegalCopyright` identifies
John Długosz; the copyright year may change between versions. Both shipped
executables carry this product identity. Use `InternalName` and `OriginalFilename`
to distinguish Launcher from LauncherDrop, and retain the path and build-role
checks: development/test executables can also embed the product's resources.
These fields exclude accidental name matches; they are not proof of authenticity.
Older Launcher builds lack the new identity fields, so missing metadata is legacy
or unknown evidence rather than proof that no Launcher exists. A process whose
identity or path cannot be inspected is likewise unknown, rather than absent.

Prefix discovery hits with a symbol and readable disposition: ignored,
targeted for upgrade, or portable and running now. An unverified hit requiring
review has its own question-mark marker. Refresh the markers when the selected
destination changes. Preserved release executables are retained, not targets
for replacement; a running copy at an untargeted known installation is ignored,
not classified as portable.

Keep installation presence separate from executable hits. First show Program
Files (all users) and Local Programs (current user) with full paths and neutral
FOUND / NOT FOUND indicators. Unknown or conflicting contents retain a check or
review status. Then list executable details and running copies, with each hit's
full path on the same logical line as its disposition symbol. The installation
choice and application folder belong on page 2; page 3 contains only the startup
option, and page 4 is the final review.

Finish destination selection, payload validation and any needed elevation before
closing applications. The first application-changing step is a graceful stop
of Launcher instances running from either the source or destination directory,
followed by a bounded wait for their actual exit. Recheck identities immediately
before file replacement. Warn about copies running elsewhere and leave them
running. LauncherDrop may contain uncopied text; if a running helper blocks an
update, let its user preserve the text and close it instead of terminating it.

Apply the versioned-file convention within the selected installation directory:
write `Launcher-R<version>.exe` and retain `Launcher.exe` as the permanent hard-link
entry point, with a verified separate-copy fallback. Replace the alias directory
entry without writing through it. Prerelease/special updates preserve the previous
active executable. Explicit final releases remove verified older Launcher
executables in the selected installation after successful activation. Registered
Setup protects one exact previous executable from the scoped update receipt,
including after a matching same-byte repair. It matches the previous SHA-256
against the regular numbered executables and recognized backups. Compare all four
numeric components, treating omitted components as zero; preserve equal/newer
versions. Snapshot cleanup candidates before activation and recheck their paths,
identity and hashes before deletion. Locked or changed files produce warnings.
Unrecognized files and mismatched filename/version metadata need manual review. Keep
personal configuration, runtime settings and edited palettes. The first
release-to-development transition still snapshots `Launcher.toml`; the installer
does not zip or place user data under revision control. An all-users program
installation still uses each user's own settings. Log-in startup remains opt-in;
its shortcut is a per-user Startup `.lnk` targeting the selected installation's
permanent `Launcher.exe`, with the installation directory as its working directory.
Never copy an executable into Startup. Manage only Launcher's own shortcut,
leaving `desktop.ini` and other entries untouched. On upgrade, initialize the
startup checkbox from the current user's existing shortcut when it targets this
installation. The checkbox expresses the desired final state: unchecking an
existing matching link plans its removal, explicitly shown on the review page.
An unchecked option leaves a link targeting another installation untouched;
checking it explicitly plans retargeting a recognized Launcher link, preserving
its arguments. Unrecognized or unreadable links require review before replacement.
Remove only the selected installation's shortcut when startup is deselected or
that installation is uninstalled. An all-users program install does not
automatically enable startup for every account.

### One ZIP, two entry points

Distribute one ZIP whose top level contains the versioned
`Launcher-R<version>-windows-x64` payload folder, `Install.cmd`, and `Portable.txt`.
The folder holds `Launcher-R<version>.exe`, the companions, documentation,
examples and all setup/maintenance scripts. No separate portable build or
installer download is needed.

The instructions begin with extracting all files from the ZIP, then offer:

- **Portable:** read `Portable.txt`, open the versioned folder and run `Launcher-R<version>.exe`.
- **Install or update:** run the outer `Install.cmd`, which invokes the nested `Install.ps1` to detect
  an existing installation or offer the installation choices above.

Packaging writes the outer wrapper with the exact versioned directory name.
The original inner wrapper still locates `Install.ps1` beside itself; the
PowerShell script resolves the payload and helpers relative to its own location.
Both wrappers must work regardless
of the current working directory. Start the wrapper unelevated and request
administrator approval only when required by the selected installation scope.
Installing a binary download must not require the source checkout or developer
build tools. Installed shortcuts target the installed copy, allowing the user to
discard the extracted download after installation.

The repository's release script requires an existing destination directory.
Both it and shipping setup create a hard-link alias with a verified copy fallback,
so first install does not depend on symbolic-link privileges or Developer Mode.
John's existing Program Files
development installation and its folder permissions are a separate local
workflow, not requirements imposed on every end user.

## Unreleased stacked-tab milestone

The `topic/stacked-tabs` working tree brings the entire selected row against
the panel and leaves the other rows in their original relative order. Packing
still uses the current full/short label widths. Every tab has a compact sloped
head followed by straight sides reaching the panel, so partial rows and
dog-eared corners expose the real tab bodies behind them without stretching
the labels or head height.

Draw from outermost row to innermost row, retaining left-to-right overlap and
placing the active tab last. Hit testing follows the reverse paint order, and
the native window region uses the same complete head/body outline. Preserve
that order and geometry when an adjustment or page update rolls back. This
logical behavior follows the existing edge rotations and DPI fitting.

`Launcher.TabStackingSmoke` uses seven inert tabs with unequal widths and four
partly filled rows. Native selection checks exercise every page expanded and
collapsed, active-row adjacency, exposed bodies, overlapping ownership, and
native regions. A native screen capture verified the connected bodies and
active row beside the panel; interactive pointer clicks remain unchecked
because the Windows app controller omits Launcher's tool window. The released 1.0.0
application and its evaluation kit retain the earlier tab geometry.

## First milestone

Completed: create the project, acquire its parser and test dependencies, and
draw a sample tab strip and icon panel with a native Quit menu.

## Edge reveal milestone

Attach to any of the four edges of a configured display, including a shared
edge. Center along the work area and keep the tabs on the selected display.
The native window shrinks to the tabs when collapsed and expands inward on
reveal. Its region follows the tab shapes, so the panel cannot leak onto an
adjacent monitor. The drawing canvas is placed for the chosen edge. Side-edge
layouts rotate the top layout clockwise on the right or counterclockwise on
the left, including tab labels and cell positions. Icon bitmaps remain upright.
The top appearance stays unchanged, and bottom labels remain horizontal and
upright. Logical tab widths, wrapping, and collapsed strip depth are shared
across all orientations.

Keep the entire native window rectangle inside the selected monitor in both
states. Windows chooses a window's monitor from its
[bounding rectangle](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-monitorfromwindow);
clipping a large window that extends onto a differently scaled neighbor would
not reliably keep the intended DPI.

Use cancellable hover and leave delays, pause hiding during the native menu,
and preserve keyboard focus during pointer-driven transitions. Suppress the
launcher during full-screen use on its display. Keep geometry and hover-state
logic independent of the native window so transitions and negative-coordinate
multi-monitor placements can be tested.

Classify transparent overlays from native attributes, without naming particular
applications. Skip layered windows with known color-key transparency, partial
global alpha, or click-through behavior, then continue the Z-order search for
the main window beneath. Unknown transparency alone is insufficient to exclude
a window; opaque layered windows remain eligible. Keep process IDs in log
records and trace the first skipped overlay only when its identity or native
transparency attributes change.

Use a self-closing color-key overlay fixture alongside the opaque full-screen
probe. Keep these visible integration helpers outside CTest. A read-only native
inspection mode reports foreground, taskbar, and ruler-overlay classes,
executable basenames, style and geometry metadata, and transparency properties
without window titles or private UI content. It performs no taskbar or foreign
window mutations.

Adobe discovery, tab-name tooltips, and tab-face refinements
remain later milestones.

## Configuration storage milestone

The project retains `Launcher.toml` as its editable sample source; build and
install outputs name that sample `Launcher.default.toml`. The original storage
milestone placed the default editable configuration and runtime in AppData.
The current [file-location policy](file-locations.md) adds portable and explicit
profiles while keeping runtime beside its selected configuration. First-run
initialization creates only a missing file and preserves any concurrently
created user configuration.

The native context menu opens the current configuration for editing or opens
its containing folder. Edits apply after restart for now. Explicit configuration
paths support tests and portable experiments without changing the normal user
file. Storage tests use temporary directories, and the drawing smoke test
selects the project sample explicitly.

## SDK precompiled-header milestone

The native Launcher GUI and full-screen probe each compile their own private
Windows SDK PCH for the selected build configuration. Do not share one PCH
between targets or between Debug and Release: the compilation environment must
match its consumers. CMake's generated wrapper supplies the compiler's forced
include, while an explicit first include in each native source preserves clear
source ordering.

The PCH contains SDK declarations, including COM and graphics headers; changing
application headers must not invalidate it. Leave project, standard-library,
parser, and test-framework includes outside, and keep stable Windows macro
definitions in the shared compiler options. Clean and reconfigure after an
SDK/toolset update; a changed header list or relevant compiler flags rebuilds
the PCH. Standard-library imports now follow this boundary, as described below.

## Compact tab sizing milestone

The user supplied a new Stardock screenshot scaled to match the desktop icons,
then authorized applying its compact text and tab height. Keep the desktop-icon
nominal size and shared pixel gap unchanged. The current default is 9-point
Segoe UI Variable at weight 600, with tab dimensions based on the effective
font em rather than the icon gap. Preserve explicit point sizes and both text
adjustment factors.

The design archive holds the original reference with its measurements and
SHA-256 checksum. It documents the single-row appearance. The user's
requested active-row reordering and the existing stacked-tab connection item
remain in the backlog, including later treatment of the active face's extra
height.

## Icon-label tooltip milestone

The panel's icons now explain their configured labels after a dwell. The delay
is separate from edge reveal and defaults to 700 milliseconds. Moving directly
to another icon starts the full delay again, so incidental pointer travel does
not immediately show a different label. The native tooltip stays upright even
when the launcher's canvas is rotated, wraps long labels, and treats Unicode
and ampersands as literal label text.

Only visible icon bounds participate. Leaving those bounds, collapsing or
suppressing the launcher, opening its menu or adjustments, and beginning a drag
clear the pending or visible tooltip. Tooltips do not activate the launcher or
take keyboard focus. Compact-tab tooltips remain later work.

## Tab selection milestone

Left-clicking a tab now selects its page of configured icons and icon-label
tooltips. Use the selected tab's full name; other tabs use
their short names when configured. Selection does not launch an icon or begin
the blank-panel along-edge drag. A page change dismisses the previous icon
tooltip and rebuilds the tools for the selected page.

Keep selection in memory for the current session. Each startup begins with
the first tab; do not add a configuration setting or persist selection in the
runtime file. Global grid dimensions remain shared across the pages. Connected
stacked faces, active-row adjacency/reordering, active/inactive face heights,
and tab-name tooltips remain separate milestones, as does Adobe resolution.

## Ordinary launch-action milestone

An item's optional launch action uses flat TOML fields: required `target`,
optional raw Windows `arguments`, optional `working_directory`, `show` defaulting
to `normal`, and `handler` defaulting to `shell`. Hidden and minimized show
requests are supported; console applications are visible by default. An unknown
handler is retained during parsing and reports unsupported dispatch, leaving
room for future providers. The current Adobe provider resolves its product name
to a concrete executable before using the same Shell launch path.

Prepare filesystem targets relative to the selected configuration directory,
expanding environment variables. Pin filesystem matches for bare executables
from the configured working directory, Windows directories and PATH, so launch
and icon extraction agree. Keep App Paths matches bare for Shell's registered
per-application environment. Shell/protocol targets retain their native meaning. An explicit working
directory may be absolute, config-relative, or contain environment variables.
Without one, an executable filesystem path uses its parent directory; bare
executables and protocols use the configuration directory. Shortcuts receive
no directory override so their saved Start in setting remains effective.

An explicit icon always wins. Otherwise extract the prepared target's Shell
icon, honoring a shortcut's custom icon; unavailable icons use the generic
application fallback. Existing icon-only items continue to draw and show their
labels without becoming launch actions.

Arm a launch on icon press and dispatch once only after release over the same
icon. Leaving the window, release outside, dialog entry, and view changes cancel the
press. Do not start the blank-panel drag from an icon. Use a short-lived STA
worker for ShellExecuteEx so launch work does not block drawing, and do not wait
for target process exit. Let the launched program and Windows handle foreground
activation. Preserve edge reveal, auto-hide, and full-screen suppression.
Interpreter programs are explicit targets; there is no automatic CMD or
PowerShell wrapper.

The starter sample adds known Shell, folder, and Settings targets while
preserving its layout and explicit stock-icon appearance. Builds must preserve
existing personal AppData settings; users add targets there rather than having
the bundled sample replace their file. Adobe resolution is now implemented in
the WIP; the separate Adobe example keeps its test tab out of personal settings.

## Icon image ownership

The window resolves item actions and explicit overrides, then requests a
physical-pixel image from `IconImages`. That component selects native bitmap
representations, uses WIC for decoding/resizing, and retains a bounded cache of
fully decoded PBGRA images. Source-file timestamps/sizes prevent stale direct
file entries. Repeated tab visits and returning to a previous size reuse the
cached image. Explicit raster files use WIC's decoder registry; target-derived
icons retain Shell semantics, with direct native resource extraction where
available. Explicit SVGs use a separate LunaSVG renderer behind the same image
request contract, rasterizing the original vector at each requested size.

Each visible `IconVisual` owns a reference to its cached WIC image and its own
render-target-dependent Direct2D bitmap. Losing or replacing the drawing
target releases only the Direct2D resources. Cache eviction never invalidates
references held by the current or rollback view. The component uses the UI
thread's existing COM apartment and imaging factory.

## Standard-library module milestone

The default build imports `std` in each project C++ translation unit. Native
sources keep their explicit SDK/PCH include first. Process real C wrappers and
`<version>` before import, then use the generated `seenthemall.hh` guards before
ordinary project and dependency headers. Keep imports outside the PCH and
public headers. The source-facing macro prelude supplies the interfaces still
used by textual source; macros already expanded inside the compiled standard
module do not need to be reconstructed for the caller.

Generate the adapter for the selected compiler and installed dependency set
into the build tree. Ordinary source/header edits trigger a small build-time
rescan, while unchanged generated contents retain their timestamps and avoid
rebuilding unrelated consumers. New/deleted files refresh CMake's inventory.
This is a tested MSVC adapter, not
a complete replacement for every standard header's macro interface. Current
sources do not need the public atomic macros; preserve another real header
before import if a future dependency demonstrates that need.

CMake manages standard-module discovery, matching per-configuration compiler
settings, import references, and object linkage. Share the relevant language
flags with its generated standard-module target, particularly UTF-8. Keep the
ordinary-header build available through `LAUNCHER_USE_STD_MODULE=OFF` and the
separate `windows-x64-headers` preset; both modes use the same vcpkg libraries.

## Configuration text preprocessing

`config_text_builder` is the common, schema-independent input layer for the
main configuration, palettes and runtime TOML. Its filename constructor reads
the file; `build()` returns owned preprocessed text and `parse()` returns a
generic `toml::table`. In-memory sources use `from_text()`. Domain-specific
readers validate those tables afterward. Main configuration loading reuses
the parsed table when it selects a palette and reads the tab definitions.

The current implementation only handles column-one `%STOP`, independent of
TOML syntax. The builder owns a vector containing one retained text piece,
totals the piece lengths, reserves the result capacity and appends the pieces.
Recursive includes, origin annotations and flattened error dumps are deferred;
the existing constructor, `build()` and `parse()` call sites need not change
when those are added.

For future `%INCLUDE`, first truncate each input at its own `%STOP`. Split the
retained input at include directives, append the preceding text as a piece,
process each named file recursively, and append the remaining original text.
This will allow one file per tab and reordering tabs through the main file's
include order. Includes are textual preprocessing, even inside TOML strings.
Source filenames and the original line number of the next piece should be
tracked so a flattened parse-error document in the application's logging
directory can be reconciled with the input files. Origin comments must not
become actual multiline-string content: retain mapping separately and annotate
the diagnostic copy where needed rather than changing the parsed values.

`OwnedStagingDirectory` is a separate first-startup storage guard. It owns a
newly created staging directory under the user's configuration directory while
startup validates a writable copy of the bundled default there. The guard
removes only its known temporary file and directory on scope exit. Publication refuses to
replace an existing user configuration. It is unrelated to preprocessing or
the eventual include-piece storage.
