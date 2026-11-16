# Generate PNG illustrations

## Generate the overlay gallery

The generated version-check overlay gallery provides normal/checking pairs
at different sizes, comparison backdrops, 100%/200% raster samples, and a manual
sheet. Its HTML works offline and can save a 2400 × 3040 PNG or an SVG with
editable text and embedded native icon samples.

To regenerate after an artwork change, build the explicit documentation target
in the usual x64 developer environment, then run the assembler with Node.js:

```powershell
cmake --build --preset release --target LauncherOverlayGallery
.\tools\render-overlay-gallery.ps1
```

Use `-NodePath` if Node.js is not on PATH, `-ExporterPath` for another matching
exporter, and `-OutputDirectory` for a new destination. Existing export folders
are preserved. Open the resulting `gallery/gallery.html`, then select **Save
manual PNG** or **Save manual SVG**. No Node packages are required.

The exporter uses Launcher's `IconImages`, embedded `render_pending_overlay`,
Direct2D source-over bitmap drawing and alpha-preserving PNG writer. It embeds
the actual three SVG resources in its executable. It opens no Launcher window,
starts no version checks and reads no personal configuration. The assembler
records source/sample hashes in `provenance.json`. Windows stock icons can
change between Windows versions, so regenerate on the desired reference system.

## Snapshot the running window

Press **Ctrl+Alt+Shift+F11** while Launcher is visible. This global shortcut works
without giving Launcher keyboard focus and can be assigned to a trackball button.
It captures the current view without revealing a collapsed dock or taking focus.
It does nothing while Launcher is minimized to the taskbar or suppressed.

Alternatively, hold **Shift** while right-clicking a Launcher tab or blank panel
area, then choose **Save window snapshot**. This command appears only in the
Shift menu. If another program has registered the shortcut, Launcher logs the
conflict and the menu command remains available. Restart releases the old
registration before the replacement registers it; export and smoke runs do not
register the global shortcut.

Both methods automatically create **Pictures > Launcher** using Windows' configured
Pictures location, including a relocated or OneDrive Pictures folder.

Files use local time, for example `Launcher-2026-10-16_14-35-22-123.png`.
If that name already exists, a numeric suffix is added; existing files are never
overwritten. No save dialog or success popup interrupts the capture.

The snapshot copies the actual window image (before the menu opens, if used), preserving
the selected page, current expanded/collapsed view, physical-pixel dimensions,
transparent gaps and partial-opacity shadows. Other windows, the desktop,
tooltips, and the menu are absent. A partly visible sliding frame is captured as
shown, including the current screen-edge clipping.

## Render illustrations from a command line

The preview generator draws through Launcher's own Direct2D/DirectWrite renderer
and writes transparent PNGs with the current display DPI. It runs on Windows x64
in a desktop session. It does not capture the screen or require the launcher to
be visible. Existing personal configuration/runtime files are not rewritten.

## Quick start

From the repository directory, after building Release:

```powershell
.\tools\render-lighting-previews.ps1 -Edges top,right,bottom,left -Pages 0,1
```

This uses `examples/preview.toml`, the editable light palette set, and Windows
stock icons. All titles include descenders. It prints each output path under a
fresh, ignored `build/previews/<timestamp>-<suffix>` directory. The generated PNGs
are not source dependencies or checked-in deliverables.

The script uses the same prism renderer as the interactive dock.
Both include the side walls and soft exterior shadows in a transparent frame;
the exported image includes the full silhouette without monitor-edge clipping.

For a header build or an executable downloaded with the source:

```powershell
.\tools\render-lighting-previews.ps1 `
    -LauncherPath .\build\windows-x64-headers\Release\Launcher.exe `
    -Edges top,right,bottom,left -Pages 0,1
```

With a supplied matching executable, image export needs only Windows PowerShell
5.1 or later. For source builds, see [Build and test Launcher](build.md). The
ordinary-header route uses `build.cmd Release headers`.

## Other configurations

```powershell
.\tools\render-lighting-previews.ps1 `
    -ConfigurationPath .\examples\preview.toml `
    -OutputDirectory .\build\manual-illustrations `
    -Edges left,bottom -Pages 1
```

`-FixturePath` remains an alias for `-ConfigurationPath`. Explicit relative
arguments are resolved from the caller's current directory; defaults are resolved
from the script's location, so the script also works when called from elsewhere.
An output filename is `prism-<edge>-page<index>.png`. Page indexes start at zero.
An existing output filename is an error; choose a fresh directory for another run.

The script passes the original configuration to Launcher. Its relative icon paths
and the configured palette search therefore retain their normal meaning, and a
configuration that already says `edge = "left"` can still produce all four views.
The script does not copy or edit that TOML file. Font/stock-icon availability and
monitor DPI can change the result on another Windows computer; this is native
rendering rather than a promise of identical pixels across Windows installations.

The corresponding native command is:

```powershell
& .\build\windows-x64\Release\Launcher.exe `
    --config (Resolve-Path .\examples\preview.toml).Path `
    --preview-edge left --preview-page 1 `
    --render-preview .\build\prism-left.png
```

`--preview-edge` only applies with export or smoke mode. To start Launcher
interactively for development, see
[Use an installed development build](build.md#use-an-installed-development-build).

## Repository hosting and wikis

The generator reads local files and invokes a local Windows executable. A source
ZIP and a clone behave the same way; the hosting provider and remote URL have no
role in rendering. Prebuilt matching executables can also be used with
`-LauncherPath`. Moving the repository does not require changing the script.

Publish selected generated images as wiki assets or documentation attachments,
separately from the source history. Keep the Markdown and those selected assets
together where possible, using ordinary relative image links, for example:

```markdown
![Launcher with left-edge tabs](images/prism-left-page0.png)
```

Both [Gitea's Markdown renderer](https://docs.gitea.com/usage/repository/markdown/)
and [GitHub's Markdown renderer](https://docs.github.com/en/get-started/writing-on-github/getting-started-with-writing-and-formatting-on-github/basic-writing-and-formatting-syntax)
resolve relative image paths. Wiki asset locations and special page-link syntax
can differ, so check those paths when moving a wiki between hosts. Avoid local
Windows paths and hard-coded server/raw-file URLs in published Markdown.
