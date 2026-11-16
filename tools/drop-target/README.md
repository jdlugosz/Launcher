# Launcher Drop

`LauncherDrop.exe` turns dropped programs, documents, folders, shortcuts, and
reusable Windows Shell objects into editable entries for `Launcher.toml`.
The starter layout opens it with `target = '$EXE\LauncherDrop.exe'`; `$EXE`
locates the bundled helper beside the running Launcher in any installation folder.
It is a separate native Windows utility, using its own red target-on-a-stem icon
and Launcher's version. The larger drop zone uses a pale Windows accent tint,
a large Launcher icon on the left, and red target rings on the right. It follows
accent/theme changes, uses system colors in high contrast, and strengthens its
highlight when a supported drop is over the zone.

1. Drop one or more items onto the top zone, or use **Add files...**.
2. Repeat as needed. New entries always append at the end of the text.
3. Edit the text if desired, then **Copy All** or select and cut/copy normally.
4. Use **Open Launcher.toml in Notepad** or **Other editors**, and paste inside
   the desired tab's `items = [ ... ]` array. Save, then quit and restart Launcher.

```toml
    { target = 'C:\Program Files\Example\Example.exe' },
    { target = 'D:\Documents\Notes.pdf' },
```

Launcher supplies the icon and label when those fields are omitted. Shortcut
paths are retained, preserving their arguments, working directory, and icon.
The chooser also preserves shortcuts rather than following their targets.
Folders can be dragged to the zone. Virtual items use their absolute Shell
parsing name when Windows can reopen that name as the same item. Temporary
file-content transfers without a persistent Shell identity are unsupported.

The text area supports selection, cut/copy/paste, Ctrl+A, Ctrl+Z, and horizontal
and vertical scrolling. **Clear** can be undone with Ctrl+Z. Drops never move
or open their source items. The helper never inserts generated entries into the
configuration; you paste and save them in your editor.
TOML quoting, Unicode, and Launcher's `$$` and `$/` escapes are handled
automatically. Every literal dollar becomes `$$`, and every literal percent
becomes `$/`, so names such as `%WINDIR%.txt` remain filenames. These generated
percent escapes require a Launcher build with `$/` support.

The helper shares Launcher's [file-location policy](../../docs/file-locations.md),
independent of the working directory. In portable mode it first looks for
`Launcher.toml` beside its executable, then in
`%LOCALAPPDATA%\Długosz\Launcher`. Installed copies search AppData without an
automatic fallback to their program directory. If no default exists, a portable
copy creates it beside the executable from `Launcher.default.toml`; an installed copy offers
to create it in AppData or exit. Existing files are preserved.

An explicit `--config` uses an absolute filename exactly, or searches a relative
filename through those same locations. An explicitly named missing configuration
is an error and is never created. Read/access/network errors report the failing
source rather than selecting another configuration. Both tools select the same
profile for the same executable location and configuration argument.

The dedicated Notepad button opens the selected configuration. The **Other editors** dropdown
below it opens the same file in an installed editor: Notepad++, Visual Studio
Code, Sublime Text, Notepad3, or Notepad4. It checks per-user and machine-wide
Windows App Paths in both registry views, ignores missing executables, and also
checks PATH and the editors' usual Program Files and per-user Programs folders.
The list refreshes each time it opens. A portable editor in an arbitrary folder
must be on PATH or registered in App Paths to be discovered.
TOML highlighting depends on the editor's version and configuration; VS Code
needs a TOML extension such as Even Better TOML. No editors or extensions are
installed by this helper. If no supported editors are found, the dropdown says
so; the Notepad button remains available independently.

To edit a different configuration:

```text
LauncherDrop.exe --config "D:\My Launcher\Launcher.toml"
```

Paths or Shell parsing names can also be supplied as positional arguments,
including by dropping files on the helper's executable icon.

Build in Launcher's initialized x64 developer environment:

```text
cmake --build --preset release --target LauncherDrop LauncherDropTests
ctest --preset release -R "Dropped targets|Drop feedback|Shell drop conversion|Dropped shortcuts|Drop configuration|Editor "
```

The application is `build\windows-x64\Release\LauncherDrop.exe`. It uses the
static C++ runtime and Windows system DLLs, with no Launcher graphics or SVG
dependencies. CMake installation includes the helper beside `Launcher.exe`.

`drop-icon.svg` is the editable source for the dedicated paddle icon. Regenerate
the nine-size `drop.ico` with `tools/drop-target/package-icon.ps1` (ImageMagick).
The drop-zone artwork is drawn at the window's DPI using Windows GDI/GDI+.
