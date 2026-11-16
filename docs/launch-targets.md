# Launcher targets and icons

## Add a launch item

Put each item inside a tab's `items` array. Windows Shell opens executables,
shortcuts, documents, folders, and registered protocols using the same syntax.
Use the included [Launcher Drop utility](../tools/drop-target/README.md) to
turn dropped files into item entries you can paste into your configuration.

```toml
items = [
    { target = "notepad.exe" },
    { label = "Home folder", target = '%USERPROFILE%' },
    { label = "Settings", target = "ms-settings:" },
    { label = "My shortcut", target = 'Shortcuts\Example.lnk' },
    { label = "Notes", target = "notepad.exe", arguments = '"D:\Work Notes\Checklist.txt"' },
]
```

An item with `target` alone gets the target's icon and a default label.
Launcher uses the resolved executable's File Description when available,
otherwise its filename. A non-file target falls back to its target text.
Set `label = ""` to suppress the tooltip. An item can also contain just an
icon and optional label to serve as a placeholder.

| Field | Meaning |
| --- | --- |
| `label` | Optional tooltip text. An omitted label is inferred where possible. |
| `target` | What a normal click opens. |
| `shift_target` | What Shift-click opens. Also offered as **Alternate** in the item's menu. |
| `arguments` | Raw Windows argument string. Include any quotes the target program requires. |
| `working_directory` | Optional starting directory, absolute or relative to the configuration directory. |
| `show` | `"normal"` by default, with a visible console for console applications. Also accepts `"hidden"` or `"minimized"`. |
| `handler` | Defaults to `"shell"`. Use `kind = "adobeCC"` for [Adobe discovery](#adobe-creative-cloud). |
| `context_verbs` | Optional ordered list of `{ label, verb }` tables for additional Shell actions. |
| `icon` | An optional file, stock name, or Windows resource selector. |

Give `target` and `shift_target` the filename on its own, with arguments in
`arguments`. TOML single quotes keep Windows backslashes literal. Spaces in a
filename are allowed as written inside the TOML string. In the Notes example,
the inner double quotes are part of the arguments that Notepad receives.

Relative launch targets and working directories start in the configuration
directory. For bare executable names, Launcher checks the configured working
directory, Windows directories, PATH, and App Paths. It remembers the resolved
file for the inferred icon and label as well as **Open**, **Run as
administrator**, and **Reveal in Explorer**. Reload the page or restart to
resolve names again. If the selected file disappears, the action reports that
failure. App Paths can supply the filename. Any App Paths `Path` environment
augmentation for a bare name is outside that exact-file dispatch.

An executable supplied as a filesystem path starts in its own directory when
`working_directory` is omitted. Bare executable names and protocols use the
configuration directory. Shortcuts keep their stored **Start in** setting
unless a working directory is supplied.

## Alternate actions and Shell commands

For a normal or alternate click, press and release on the same icon. Launcher
checks Shift on release. If the selected target is missing from the item,
Launcher plays a short ding. In the context menu **Open** and **Alternate**
always use their corresponding targets.

```toml
{ label = "Script", target = 'Scripts\Example.cmd', shift_target = 'Scripts\Example.txt' },
{ label = "Notes", icon = { stock = "document" }, shift_target = 'Documents\Notes.txt' },
```

Both actions share the item's launch options. The alternate target resolves
independently of any default version saved for the normal target. When only
`shift_target` is present, Launcher can infer its icon. Supply an explicit
`label` for the tooltip on such an item.

Right-click a launchable icon for actions such as **Run as administrator**
and **Reveal in Explorer**. **Windows commands** opens the native Shell menu,
including **Open with** where Windows offers it. To add an action directly to
Launcher’s menu, specify its canonical Windows Shell verb and a label.

```toml
{ label = "Document", target = 'Documents\Notes.txt', context_verbs = [{ label = "Print", verb = "print" }] },
```

Custom verbs require a normal target with the `"shell"` or `"adobeCC"`
handler. They preserve the item's launch options. Windows reports an error
when the target cannot perform that verb.

## Path macros and environment variables

Launcher supplies three path macros.

| Macro | Value |
| --- | --- |
| `$EXE` | Directory containing the running Launcher, with no trailing separator. |
| `$SELF` | Full filename of the running Launcher, including its versioned name when applicable. |
| `$USERDATA` | Current user's Launcher profile directory in local AppData. |

Add custom prefixes in an optional top-level `[paths]` table before any
`[[tabs]]` block. Names distinguish case. Start a name with an ASCII letter
or underscore, followed by letters, digits, or underscores. The built-in
names are reserved. Include any path separator yourself.

```toml
[paths]
Tools = 'C:\Tools'

[[tabs]]
name = "Workspace"
color = "teal"
items = [
    { target = '$Tools\editor.exe' },
    { label = "Launcher Drop", target = '$EXE\LauncherDrop.exe' },
    { label = "Launcher guide", target = '$EXE\docs\Launcher.html' },
    { label = "Color demo", target = '$SELF', arguments = '"$USERDATA\Demos\demo_many_colors.toml"' },
]
```

The name is replaced once at the beginning of a `target`, `shift_target`,
explicit icon filename, Adobe `root_path`, or `arguments` value. In arguments
it can also follow an opening double quote. An undefined name is a
configuration error. An inserted value keeps its literal text and never
triggers another named macro.

Non-argument paths expand `%NAME%` environment variables after any named
macro substitution. `working_directory` expands environment variables but
keeps a leading `$Name` literal. `arguments` goes to the target program
without environment expansion. Use `$$` for a literal dollar and `$/` for a
literal percent in these fields. Launcher decodes those escapes after
environment expansion, once. For example,
`'%USERPROFILE%\Documents\$/PATH$/.txt'` opens `%PATH%.txt` under the user's
Documents directory without expanding `PATH`.

## Choose an icon

An explicit icon overrides the icon inferred from a target. A shortcut's
inferred icon honors its customization. If artwork is unavailable, Launcher
uses a generic application icon. Relative icon filenames follow the common
[file search order](file-locations.md#read-search-order), keeping their
whole relative path at each root.

| Form | Example |
| --- | --- |
| Image or icon file | `icon = 'Icons\Editor.svg'` |
| File table | `icon = { file = 'Icons\Editor.svg' }` |
| Stock icon | `icon = { stock = "folder" }` |
| Icon group by position | `icon = { file = '%SystemRoot%\System32\imageres.dll', index = 62 }` |
| Icon group by resource ID | `icon = { file = 'Artwork.dll', id = 4 }` |

Stock names include `application`, `folder`, `document`, `computer`, `world`,
`find`, `shield`, `recycle`, `settings`, `information`, `drive`, `network`,
`audio`, `image`, and `video`. Use `stock` in a table. A string by itself
names a file.

`index` is zero-based among logical icon groups. `id` is a numeric icon group
resource ID from 0 through 65535. Pick one selector. Launcher takes an exact
size when available, otherwise the smallest larger size, or the largest smaller
size when enlargement is needed. Higher color depth wins at equal sizes.

### Images and SVG

Launcher reads image formats supported by Windows Imaging Component, including
PNG, ICO, JPEG, BMP, GIF, and TIFF. An explicit `.svg` uses the bundled renderer
and scales to the icon size with its viewBox and aspect ratio preserved. Use
self-contained UTF-8 SVGs and embed raster images as data URIs. Static paths,
transforms, gradients, clipping, masks, and embedded CSS are supported. Flatten
animation or filter effects before export, and convert text to paths for
consistent typography on other machines.

The bundled [SVG Extractor](svg-extractor.md) can find and save SVG source
files from installed programs for use as Launcher icons.

### Windows image resources

For a complete image in a native Windows executable or DLL, add `type` and
either `name` or `id`.

```toml
icon = { file = 'Artwork.dll', type = 'SVG', name = 'LOGO' }
```

For a numeric resource with an explicit language, use a selector like this.

```toml
icon = { file = 'Artwork.dll', type = 10, id = 123, language = 1033 }
```

`type` and `name` can be literal Unicode strings. Use an integer from 0 through
65535 for a numeric type or ID. A name beginning with `#` is reserved.
`index` belongs to icon groups and cannot be
combined with `type`. `name` and `language` require `type`. Omit `language`
to use Windows language selection, or give an exact language ID from 0 through
65535. Zero selects an existing language-neutral entry.

Launcher examines the resource bytes to choose an image decoder. An SVG payload
can use a custom resource type or `RCDATA` (type 10). Complete SVG and
WIC-readable images up to 32 MiB are supported. Native bitmap and individual
icon-frame layouts, managed .NET resource containers, and compressed wrappers
require separate extraction. Launcher reads the resource without executing
its containing module. See [resource examples](../examples/pe-resources.toml)
in the source checkout for more selectors.

## Find installed versions with wildcards

Use `*` or `?` in the changing parts of an ordinary filesystem target path.
Macros and environment variables expand before discovery.

```toml
{ target = '%ProgramFiles%\Blender Foundation\Blender*\blender.exe' },
{ target = 'Tools\Suite *\bin\editor-*.exe' },
{ target = 'Documents\Report-202?.pdf' },
```

Wildcards can occur in multiple directory components and in the final filename.
Windows matches each supplied component. Use explicit directory levels for the
search. Recursive `**` and bracket ranges have no special syntax. URLs and
Shell namespace targets keep their normal meaning, including URL query strings.
Matching directories with no final file are skipped.

Launcher compares numeric Product Version and File Version metadata when
available. Targets without version metadata are sorted by the matched folder
and filename in natural numeric order, so `5.10` follows `5.9`. Metadata-bearing
files sort ahead of those without metadata.

### Choose a version

A normal click opens the newest eligible version or your saved default.
Right-click for the inline **Installed Versions** list. **Latest** marks
the newest eligible version, and the checkmark shows what a normal click
opens. Select a row to open that exact file once. Long lists continue under
**More versions** in groups of 50.

Right-click a version row for **Open**, **Run as administrator**, **Reveal
in Explorer**, **Properties**, **Set as default**, and **Windows commands**.
The Apps key or Shift+F10 opens the same menu for the highlighted row. These
actions use the file shown while the menu is open. The item's launch options
also apply when you open a specific version.

**Set as default** remembers a file for this item. **Always use latest** in
the main icon menu restores automatic selection. Both commands only change
the preference. If a saved version disappears, **Active version unavailable**
asks you to choose a different version or clear the preference. Launcher keeps
the choice until you change it. Defaults live in the profile's sibling
runtime file. Reordering distinct items preserves their defaults. Editing an
item's identifying fields, such as its tab name, launch settings, label, or
icon, gives it a new identity.

### When Launcher checks for versions

Launcher checks the selected page when it reveals or when you switch to it
while the panel is open. A click during the first check waits for its result.
An earlier complete result stays usable during a refresh or after an access
error. When a check succeeds with no matching files, opening the item reports
that no version is available. A failed check reports the discovery error.

On hover, an ellipsis badge indicates several versions. Translucent slats over
the icon indicate a pending check. An icon and its inferred tooltip follow the
active version as results change. New results wait until open menus and other
interactions finish before replacing the visible choices.

## Adobe Creative Cloud

Use `kind = "adobeCC"` and an Adobe product family name. The `Adobe` prefix
is optional. Matching ignores case. `Adobe Premiere Pro` is also accepted for
`Adobe Premiere`.

```toml
{ kind = "adobeCC", target = "Adobe After Effects" },
{ kind = "adobeCC", target = "Photoshop", include_beta = false, root_path = 'D:\Adobe' },
```

Launcher uses Adobe and Windows uninstall registrations in both registry views
to find installation roots, and also scans the native `Program Files\Adobe`
tree. It verifies the product and version from the actual executables, which
lets it group a product across year-specific folders and custom installation
paths. `root_path` can add a directory or an array of directories. Relative
roots start in the configuration directory. Each accepts path macros and
environment variables. Supply concrete roots without wildcards. The same
roots apply to an alternate target.

When product-name matching misses an application, use its executable filename
ending in `.exe`. `*` and `?` are allowed in that filename. Choose a precise
pattern to avoid including helper programs. Put the search directories in
`root_path`.

```toml
{ kind = "adobeCC", target = "Dreamweaver.exe", root_path = ['C:\Adobe', 'D:\Adobe'] },
```

The version menu and saved defaults work as for wildcard filesystem targets.
By default, the newest stable or Beta installation can win. Set
`include_beta = false` to limit the automatic choice to stable installations.
Beta entries remain visible in the menu and can still be chosen explicitly.
A product name ending in `(Beta)` selects that family. Adobe icons and
tooltips follow the selected installation. Use `label = ""` to hide the tooltip
including its version text.

After a successful scan, an unmatched product name opens a Notepad list of
available product families once per name in a Launcher session. To request
the automatic catalog without opening a dock, use either command below.
The first opens the ready-to-paste tab in Notepad. The second writes it to a
file.

```cmd
Launcher.exe --adobe-cc-list
Launcher.exe --adobe-cc-list-output "D:\AdobeCC.toml"
```

The source checkout includes an [Adobe configuration example](../examples/Adobe-CC.toml)
and a [maintainer's guide to version discovery](manual/version-discovery/README.md).
