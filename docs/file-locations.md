# Configuration and file locations

This is the shared file-location policy for current Launcher development and
Launcher Drop. The published 1.0.0 executable retains its historical behavior.

The implementation is `launcher::FileLookup` in
[`include/launcher/file_lookup.hpp`](../include/launcher/file_lookup.hpp). It owns
the resolved executable and user-data locations, the ordered directory roots,
and the separate portable/explicit-selection flags. Consumers pass this context
for file operations and choose either the shared search policy or an exact
filename. They do not build their own search lists. Text results carry their
actual source for diagnostics and logging.

## Read search order

Launcher builds one search context during initialization. Relative filenames
are tried in this order:

1. **The explicitly selected configuration file's directory**, when a
   configuration argument was supplied.
2. **The portable executable's directory**, in portable mode only.
3. **The user's AppData directory:** `%LOCALAPPDATA%\Długosz\Launcher`.
4. **Embedded resources**, where applicable. Standard palettes are embedded
   resources in the running executable.

AppData is the last user-data directory root; embedded resources follow it.
Portable copies try visible palette files beside the executable before reaching
embedded resources, including files with the standard palette names.
An installed executable directory is not an automatic search root for editable
configuration, custom palette or icon files; those files belong in AppData or an
explicitly selected profile directory.
The `examples` directory is a source for browsing and copying, not an additional
search root.

An absolute filename is used exactly as supplied. A relative filename keeps its
complete subtree at each root: `Icons\Bigicon.svg` can therefore resolve to
`%LOCALAPPDATA%\Długosz\Launcher\Icons\Bigicon.svg`. The process's current working
directory is not a search root. Path macros and environment substitutions happen
once, when configuration values load, before these filenames are located.

The policy covers the main configuration, palette definitions and explicit icon
files, including EXE/DLL containers used as icon sources. Future included files
will use the same context; configuration inclusion is not implemented yet.
Relative **launch targets** and **working directories** retain their existing
meaning relative to the selected configuration. They are not searched as
configuration assets.

## Duplicate directories

Directory roots are deduplicated while **retaining the last confirmed
occurrence**. Use filesystem identity (`std::filesystem::equivalent`), rather
than assuming differently spelled paths name different directories. Windows can
recognize aliases to the same directory using volume identity and file identity.

For example, explicitly naming a configuration in AppData must not promote
AppData ahead of the portable executable directory. Its earlier duplicate is
removed. The configuration remains explicitly selected even when this removes
its directory from the front of the search list.

Different underlying volumes are distinct. Missing directories and inconclusive
identity comparisons are conservatively retained; uncertain network aliases do
not need special mapping machinery. This can cause a repeated lookup, but must
not discard a possible source. A failed optional identity comparison is not
proof that two directories differ, and does not by itself prevent first-run
creation of a missing directory.

## Installed and portable modes

Mode comes from the executable's location. The recognized installed roots are:

- `%ProgramFiles%\Długosz\Launcher`.
- `%LOCALAPPDATA%\Programs\Długosz\Launcher`.

An executable outside these installed locations is portable. Folder writability,
the working directory and the selected configuration's location do not determine
mode. Portable mode and an explicitly supplied configuration argument are
separate flags; naming `Launcher.toml` explicitly is still an explicit selection.

## Selecting and creating the main configuration

Starting `Launcher.exe` with no arguments opens the default drawer for this
Windows account, session, and installation. If that drawer is already running,
Launcher reveals it on its current tab, including after **Hide**. Supplying a
configuration argument opens a separate drawer.

An optional positional filename or `--config <filename>` selects a configuration.
An absolute argument selects that exact file. A relative argument searches the
portable executable directory when applicable, then AppData. There is no selected
configuration directory to prepend until this search succeeds. Afterward, that file's
directory participates in dependent-file searches because the configuration was
explicitly named.

With no filename argument, search those same locations for `Launcher.toml`.
Setup prepares a missing default in the initiating user's AppData folder after
portable-profile transfer, using `Launcher.installed.default.toml`. It also
seeds the personal color demo before launch. Existing custom configuration is
preserved; exact stock guide/demo entries can be updated with a backup. The
elevated program-file worker does not create another account's profile.

If it is genuinely absent everywhere:

- A portable copy creates `Launcher.toml` beside its executable from the bundled
  `Launcher.default.toml`.
- An installed copy displays a modal choice to **create the configuration in
  AppData** or **exit**. It does not silently invent a default-named profile.

An explicitly named missing configuration is a fatal error with a modal message.
It is never seeded, copied or replaced by another configuration. Creation of a
default profile preserves any file another process created concurrently.
`Launcher.default.toml` is the portable starter;
`Launcher.installed.default.toml` is generated from it for installed use. These
program-owned templates are read from their exact locations beside the
executable. They do not add the program directory to editable-file search roots.
An installed create-choice also prepares the personal demo when needed.

## Runtime settings

Runtime settings use the same `FileLookup` object for **both reading and
writing**, with its exact-file policy. They are read and written **only beside
the selected configuration**, using its basename:

| Configuration | Runtime settings |
| --- | --- |
| `Launcher.toml` | `Launcher.runtime.toml` |
| `D:\Layouts\Studio.toml` | `D:\Layouts\Studio.runtime.toml` |

Launcher never borrows a runtime file from another search root. Saved sizes,
position, presets and active-version choices therefore follow the chosen
configuration.

A missing sibling runtime file means there are no saved overrides yet. A file
that exists but cannot be read or parsed is an error, not an empty set of
settings. Unexpected read, validation or write errors show an error box and
leave the runtime file unchanged. Startup continues using the base configuration
when saved settings cannot be loaded; a failed save does not close Launcher.
Apply, preset Store and active-version changes read and update that
same exact sibling. A staged save is published in its own directory; neither
the staging file nor the destination is located through the asset search list.

## File operations and exact destinations

One file-access context does not give every operation the same search behavior:

| Operation | Location policy |
| --- | --- |
| Configuration, palettes, explicit icon files | Shared read search order, unless the requested filename is absolute. |
| Runtime reads and writes | Exact sibling of the selected configuration, through `FileLookup`; no fallback. |
| Logs, filesystem-error journals and diagnostic reports | Their already selected output destination; creation, append and rotation never search for an existing file elsewhere. |
| Exported images and snapshots | The requested or configured output directory, retaining the export's existing overwrite rules; no read search. |
| Bundled starter and temporary save files | The identified source and owned staging destination; publication must preserve an existing user file when creation races. |
| Program metadata, discovered installations, shortcuts and Shell objects | The exact object being inspected or activated; never an alternative copy found in configuration directories. |

Launcher rotates its activity log and filesystem-error journal at a nominal
256 KiB. The newest backup has a `.1` suffix. Additional segments have a
`.1.<timestamp>` suffix and are retained for at least 48 hours from their last
write. The log writer checks for expired segments when it prepares a log or
rotates it. A busy log can accumulate several recent segments. The sibling
`.lock` file coordinates writers.

Setup seeds `%LOCALAPPDATA%\Długosz\Launcher\Demos\demo_many_colors.toml`
from the bundled example before launch, preserving any existing working copy and
runtime settings. The installed starter runs this AppData configuration directly.
Its quoted demo argument uses `$USERDATA`, the current user's Launcher profile
directory, rather than relying on environment expansion in arguments.
The standalone bundled color-demo entry point uses `--color-demo` and can also
prepare a missing working copy in installed mode.
In portable mode it selects `examples\demo_many_colors.toml` beside the program.
The selected demo keeps the usual exact-sibling runtime and log destinations;
installed defaults remain embedded in the EXE. Program Files contains no runnable
demo TOML or wrapper. Its icon is seeded in `Icons\many-colors.svg` beneath the
profile; existing edited icons and demos are preserved. Old `Demos\ManyColors`
copies remain available after migration for custom links and recovery.

Native image decoders, resource loaders and Windows metadata APIs can operate
on the selected filename or handle. They must retain that established identity;
they do not invent another directory search. Resolving a relative icon source
and then decoding that exact source are separate steps of the same operation.

Windows' executable resolution, registered editors, Shell activation and version
discovery retain their own existing semantics. Named pipes used by restart are
communication channels rather than file-location candidates. Test fixtures and
instrumented scenario outputs remain confined to their explicitly selected test
directories. None of these are extra asset search roots.

## Errors and source reporting

Only genuine absence advances to the next source. An existing file that cannot
be opened, a directory where a file is expected, a network/access failure or
invalid file contents reports the failing full path and the available operating
system or parser explanation. It does not silently select a later file or an
embedded default. Missing optional search directories are ordinary absence.

An explicit icon absent from every source keeps the existing logged generic-icon
fallback. Unsupported or invalid image data also keeps the image decoder's
logged fallback; it does not cause another directory's file to be selected.

The log identifies the full resolved filename actually loaded. An embedded
source is identified by the full executable filename and resource name. Logging
the original relative argument alone is insufficient to identify the source.

## Installing after a portable trial

A fresh installation transfers the versioned payload folder's root `*.toml` files,
except the program-owned `Launcher.default.toml` and
`Launcher.installed.default.toml`, into the initiating user's AppData
profile. This includes configurations, their sibling runtime files and palettes.
Files beneath the root `Icon` and `Icons` directories are also transferred,
preserving their relative subdirectories. No other directory names are guessed.
The outer `Install.cmd` and `Portable.txt` do not change that profile root.
The `examples` directory remains reference material. Absolute paths and `$EXE`
references keep their existing meaning; setup does not rewrite configuration.

Review lists the transfer and its destination. Existing files are never
overwritten. If an AppData profile contains differing or destination-only files
in this scope, setup skips the whole transfer with a warning instead of mixing
profiles. An identical subset can be completed during a fresh installation.

An upgrade preserves the existing installed profile and never imports trial
files, even to fill individual missing files. When trial files differ from
AppData or are absent there, review and completion warn that those files are not
copied and list the relative paths. Profile transfer runs in the initiating
user's context after affected instances have saved their final runtime settings,
independently of any administrator used to install program files. Sources and
staged copies are hash checked; a failed transfer removes only its own new files.
