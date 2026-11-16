# Launcher appearance and adjustments

## Tabs and labels

Click a tab to show its items. The active tab shows its full `name`. An inactive
tab shows `short_name` when supplied. Each startup selects the first tab. When
tabs occupy several rows, the active row moves next to the panel and the other
rows keep their order.

Set `[appearance]` before the first `[[tabs]]` block to change the default text,
tooltip delay, and page thickness. This example shows the defaults.

```toml
[appearance]
font_family = "Segoe UI Variable"
font_weight = 600
text_size_pt = 9
tooltip_delay_ms = 700
thickness = 2.0
reverse_thickness = false
tab_text_direction = "auto"
```

Launcher uses Segoe UI when the requested family is unavailable. Font weights
run from 1 through 999. Text size in points is converted to DIPs for drawing
and then scaled to the active display. For example, 9 points is 12 DIPs at the
nominal size. Existing `text_size_dip` values are supported. Choose one size
field per profile.

`tooltip_delay_ms` is an integer from 0 through 5000. An icon's label appears
after the pointer rests on its cell. Moving to another icon starts a new delay.
Tooltips stay upright on every edge and disappear during menus, dragging, or a
dock transition. Use `label = ""` to turn off an item's tooltip.

### Thickness and direction

`thickness` accepts 0 through 16 DIPs and determines the depth of the page
walls. The depth snaps to physical pixels at the dock's display scaling. For
example, 2.4 DIPs gives 3 pixels at 125% scaling and 2 pixels at 100%. Zero
removes the walls, shadows, and outline. Icon and text scales leave thickness
unchanged. `reverse_thickness = true` shows the opposite side wall.

`tab_text_direction` accepts `"auto"`, `"top-to-bottom"`, and
`"bottom-to-top"`. Auto reads downward on the right edge and upward on the
left edge. The explicit directions apply to the vertical edges. Labels on
the top and bottom stay horizontal, and icons are upright on all edges.

## Tab colors

Set `color` inside a `[[tabs]]` block to select a named palette or generate one
from a six-digit hexadecimal color.

```toml
[[tabs]]
name = "General"
color = "salmon" # Or "#EE9696" for a generated gradient.
items = [{ target = "notepad.exe" }]
```

A hexadecimal color seeds a gradient toward a lighter shade. For a solid fill,
set both gradient endpoints to the same color. The active tab uses the page
gradient's first color. The `tab` gradient applies to the inactive tab.

### Load a color set

Launcher loads `basic_colors.toml` followed by `extra_colors.toml`. The basic
set includes salmon, orange, yellow, green, red, rose, teal, cyan, blue,
indigo, violet, and lavender. The extra set adds copper, purple, plum,
gold, lime, emerald, turquoise, silver, and slate.

Put the optional `colors` setting before the first TOML table. Use
`colors = "light"` or `colors = "dark"` for a bundled set, or specify several
as `colors = ["light", "custom"]`. Later files replace matching colors. Each
selected name looks up `colors-<name>.toml` using the shared
[file search order](file-locations.md#read-search-order). Omit `colors` or use
an empty array to keep just the two fixed sets. The fixed names are looked up
literally, in the same search locations.

Find readable copies of the bundled palettes in `examples` beside the executable.
Copy one to your profile's color-file search location as `colors-custom.toml`,
edit it, and select `"custom"`. An edited copy of `basic_colors.toml` or
`extra_colors.toml` in the same location loads automatically. Save edits and
choose **Restart**. The log records the file or embedded resource loaded.

### Define and override colors

Palette names are ASCII and compare without regard to case. Each name may occur
once in a file. Define `tab`, `page`, and `outline` for a new color. A gradient
needs 2–16 strictly increasing stops, starting at 0 and ending at 1. `text` is
optional and defaults to `#131313`.

```toml
[colors.copper]
tab.gradient = [{ at = 0, color = "#FFE0C1" }, { at = 1, color = "#E39954" }]
tab.hover = "#FFE0C1"
page.gradient = [{ at = 0, color = "#E39954" }, { at = 1, color = "#F6C995" }]
page.hover = "#CC8A4C"
outline = "#80502B"
text = "#131313"
```

`tab.hover` colors an inactive tab under the pointer. `page.hover` colors a
launchable icon's cell, including while its menu is open. Existing array forms
`tab = [...]` and `page = [...]` use default hover shades. The full form above
can customize them.

Use `partial = true` to change selected properties of an existing color.
Unspecified properties retain their previous values. A supplied gradient
replaces all its stops.

```toml
[colors.orange]
partial = true
outline = "#84520A"
tab.hover = "#000000"
```

An unknown tab color stops startup. Interactive Launcher opens an
`available-colors-<pid>.txt` report with the missing name, the loaded sources,
and the available choices. Fix the name or supply the missing definition and
start it again.

## Icon size and spacing

`grid.icon_size_dip = "desktop"` samples the desktop's nominal icon size at
startup. Launcher draws it at the dock's own display scaling. For example, 48
DIPs draws as 60 physical pixels at 125%. A numeric value sets the nominal size
directly. A display resolution change triggers a restart to refresh the size.
An open menu, adjustment dialog, or drag can finish first.

`grid.icon_spacing_px` sets both the gap between icons and the margin at the
panel edge. It defaults to 8 and accepts integers from 0 through 1024. The gap
stays the same number of physical pixels when either DPI or icon or text size
changes. Zero places the icon canvases flush with each other and the panel.

## Save live adjustments

Choose **Adjust sizes...** to preview text scale, icon and panel scale, spacing
in physical pixels, and page thickness. Text and icon scales accept 25–400%.
**Apply** saves the preview and **OK** saves and closes. **Cancel** returns to
the last applied settings. **Reset sizes** restores 100% scales and the base
spacing and thickness. It leaves the base file and the dock position as they
were. If a preview cannot fit, Launcher keeps the previous valid values.

Settings are saved beside the selected profile. `Studio.toml`, for example,
uses `Studio.runtime.toml`. The runtime text and icon scales multiply the base
`[adjustments]` scales. To absorb a saved scale into the base file, multiply
the two factors and remove the corresponding runtime key. Spacing and thickness
are absolute overrides, so copy the saved value into the base key and remove
the runtime key. See [runtime settings and file locations](file-locations.md#runtime-settings).

For a legacy profile, convert to `grid.icon_spacing_px` by removing
`grid.icon_padding_dip` and `adjustments.spacing_scale` from the base file and
using the physical gap shown in the dialog. `appearance.panel_padding_dip` is
accepted for compatibility. The current margin follows the icon gap.

### Size and position presets

Define named slots at the end of the runtime file before opening the dialog.
Use distinct nonempty names. The dialog reloads them each time you choose
**Store...** or **Recall...**.

```toml
[[preset]]
name = "Normal"

[[preset]]
name = "Larger icons"
```

Select a slot and press Enter, double-click it, or press the same button again.
Escape returns to the size notes. **Store...** immediately saves the current
preview in that slot. **Recall...** shows a preview of that slot's settings
and edge position. **Apply** or **OK** makes the preview active for future
launches. **Cancel** returns to the last applied settings and leaves stored
slots intact. Runtime files are rewritten as TOML with a standard header.

### Position along the edge

With the panel open, drag a blank area along the edge. Releasing saves the
position. Escape cancels. Position runs from 0 at the left or top through 0.5
at the center to 1 at the right or bottom. To make the saved position part of
the base profile, copy it into `dock.position` and remove the runtime override.
