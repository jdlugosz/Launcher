# Launcher configuration

## Edit the current profile

Right-click a tab or an empty part of the panel and choose **Edit configuration**.
**Open configuration folder** opens the same file's parent in Explorer and selects
the file. Save your changes, then choose **Restart** to reload the configuration,
palettes, and saved adjustments. The current dock stays available while the
replacement loads. A configuration error leaves the current dock running and
shows the error. **Quit** cancels a restart in progress.

The starter has **Launcher**, **Windows**, and **Examples** tabs. They open the
bundled tools and guides, standard Windows apps and folders, and the color demo.
**Hide** minimizes the dock to the Windows taskbar until restored. Use its taskbar
button to restore it on the same tab.

## Select a different profile

Starting `Launcher.exe` with no arguments opens the default profile or brings
its existing drawer back to the edge on the current tab. Pass a filename to open
a separate drawer with another profile.

```cmd
Launcher.exe "D:\Layouts\Studio.toml"
```

`--config "D:\Layouts\Studio.toml"` selects the same file. Relative names
are looked up in the portable executable directory when applicable, then the
user's Launcher AppData folder. Create an explicitly named profile before
launching it. Multiple profiles can run at the same time. **Restart** reloads
only the drawer and profile you use it on.

Setup preserves existing settings and can update exact stock guide or demo
entries. Its configuration backups are named
`Backups\Launcher.before-setup-<date>-<unique-id>.toml`. Use the
[file-location guide](file-locations.md) for starter creation, the complete
search order, and profile and runtime filenames.

## Add tabs and items

Add a `[[tabs]]` block with a `name`, `color`, and `items` array. The optional
`short_name` appears when the tab is inactive. An item can launch a target,
display artwork, or make room in the grid. `spacer` occupies one cell and
`break` advances to the next row or column when the current one has entries.

```toml
[[tabs]]
name = "Workspace"
short_name = "Work"
color = "teal"
items = [
    { target = "notepad.exe" },
    { kind = "spacer" },
    { label = "Projects", target = 'D:\Projects' },
    { kind = "break" },
    { label = "Settings", target = "ms-settings:" },
]
```

See [launch targets and icons](launch-targets.md) for item fields and examples,
and [appearance and adjustments](appearance.md) for colors and labels.

The starter's annotated examples follow `%STOP ===========examples below`.
Copy an item line with its trailing comma into an `items` array above that
marker, or copy a complete `[[tabs]]` block to add a tab. To browse the starter
again, open `Launcher.default.toml` beside the executable.

### The `%STOP` marker

Launcher reads only the text before `%STOP`. The directive starts in column
one, uses uppercase letters, and ends at a space, tab, or the end of the line.
Keep it outside strings and arrays so the text before it remains complete TOML.
The marker is recognized before TOML parsing, including inside a multiline
string. It works in the main configuration, color files, and runtime settings.
The text after the marker can contain notes, unfinished edits, or examples.

## Set the page grid

```toml
[grid]
columns = 6
rows = "auto"
flow = "row-major"
icon_size_dip = "desktop"
icon_spacing_px = 8
```

Put the `[grid]` table before the first `[[tabs]]` block. Each dimension can be
an integer from 1 through 128 or `"auto"`. Launcher sizes automatic dimensions
across all tabs so their panels share one grid. `"row-major"` fills each row in
turn and `"column-major"` fills each column in turn. Breaks end the current run
early and spacers leave an empty cell.

Rows and columns follow the top-edge layout. On a vertical edge the layout
rotates and the icon artwork stays upright. For example, on the left edge the
first row in a row-major grid becomes the leftmost visual column, farthest from
the tabs. Its items fill from top to bottom, then the next row fills the column
nearer the tabs. Bottom-edge labels remain horizontal.

Set `icon_size_dip` to `"desktop"` to use the desktop's nominal icon size, or
enter a positive size through 256 DIPs. Launcher can shrink the contents of an
oversized grid to fit the display work area while preserving the gap in physical
pixels. If the gaps alone cannot fit, choose fewer cells or a smaller gap. See
[icon sizes and gaps](appearance.md#icon-size-and-spacing) for scaling details.

## Place the dock and set its motion

```toml
[dock]
edge = "right"
display = "primary"
auto_hide = true
hover_delay_ms = 180
hide_delay_ms = 350
reveal_duration_ms = 220
suppress_fullscreen = true
position = 0.5
```

`edge` accepts `"top"`, `"bottom"`, `"left"`, or `"right"`. Set `display`
to `"primary"` or a Windows display name such as `'\\.\DISPLAY2'`. The startup
log lists the available names and their screen and work areas.

`position` is a fraction of the usable edge travel, with 0 at the start, 0.5
at the center, and 1 at the end. The dock uses the display work area so it can
stay clear of the taskbar. On a shared monitor edge the collapsed portion is
clipped away. You can also [drag the dock along the edge](appearance.md#position-along-the-edge).

The hover and departure delays control when the panel moves. Set
`reveal_duration_ms` for the slide outward and optionally `hide_duration_ms`
for its return. If the return duration is omitted, it follows the reveal
duration. Delays and durations take integer milliseconds from 0 through 5000.
Set a duration to zero for immediate movement. Hover preserves keyboard focus.

With `suppress_fullscreen` enabled, Launcher hides while the top main window on
its display is full-screen. A normal maximized window with a caption leaves it
available. Transparent overlays may be skipped so their underlying main window
determines whether the dock appears.
