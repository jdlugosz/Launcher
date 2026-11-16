# Launcher SVG Extractor

Find SVG artwork inside a program, library, or installation folder, and copy
it to ordinary SVG files. You can browse previews to pick a better image when
the program's bitmap icon is poor.

## Use the utility

1. Run **LauncherSvgExtractor.exe** beside Launcher. It is included in Launcher's
   download and installed by Launcher setup. You can also run it straight from
   the extracted ZIP or its standalone download. It uses Windows' .NET Framework
   4.6.2 or later; no separate installation or administrator approval is needed.
2. Choose **Choose program...** for an EXE or DLL, or **Choose folder...** for
   its whole installation folder. Folder scans include every subfolder. You can
   also paste a path, or drop a program/folder onto the utility.
3. Choose where to save the results, then click **Find and extract SVGs**.
   Each run creates a new folder there, preserving earlier results.

The gallery opens when the scan finishes unless you clear that checkbox.
Search by file or resource name, inspect the previews, and use **Save SVG**
to save a chosen image. **Open results folder** shows all extracted files.
The `assets` subfolder holds the SVGs. Keep it beside `examples.html` when
copying the gallery elsewhere. Copies remain available after an archive
drive is unmounted or the source application is uninstalled.

Click **Keep** beside an image to copy it to
`%LOCALAPPDATA%\Długosz\Launcher\Icons`. The gallery shows the saved path.
An identical existing copy is reused; a different file is never replaced.
Keep the extractor open while choosing images: it handles the copy requested
by the browser. Nothing is uploaded. You can use **Open saved gallery...** to
reopen an earlier extraction and keep images without rescanning its source.
Opening `examples.html` directly still gives offline previews and downloads;
open it through the extractor to enable Keep.

Each SVG filename includes a cleaned-up resource name and a content hash.
Identical SVG payloads share one copy; `resources.csv` preserves every source
location. `skips.csv` explains files or resources that could not be checked.
**Stop** finishes a partial report and keeps the files already extracted.

SVG artwork can include buttons and other graphics as well as app logos.
The utility identifies SVG files; you choose the artwork you want.
Launcher can load a saved `.svg` directly through an item's `icon` field. For
the default personal configuration, a file saved by **Keep** can be referenced
as `icon = 'Icons\saved-name.svg'`. Other configurations can use an absolute
path or a path relative to their configuration file. The launcher rasterizes
the vector at the actual icon size, so resizing does not enlarge a small PNG.
Launcher's main guide describes the renderer's supported artwork and export
limitations; it is supplied as `docs\Launcher.md` in the combined download.
For a portable profile, copy the chosen SVGs into that profile's `Icons` folder
if you want to move the profile to another computer.

## What it can find

The utility reads loose SVG/SVGZ files, native Windows resources, managed
.NET resources, and assets in supported Windows/extension/NuGet packages.
It checks EXEs, DLLs, resource modules and common plugin formats throughout
the selected folder. Gzip-compressed SVG is expanded. Source programs are
read without loading their code, running them, or requiring their dependencies.
The selected source is read-only; only the results folder is written.

Only parsed SVG is exported. Compiled XAML/BAML/XBF, PRI indexes, fonts and
ordinary bitmap icons are not converted into SVG. Arbitrary installers,
CAB/7z/RAR archives, Qt resource bundles and Electron ASAR are not unpacked.
Junctions, symbolic links, cloud placeholders and inaccessible paths are skipped.
The optional [advanced scanner reference](advanced-icon-catalog.md) describes
the shared scanner's coverage and the separate command-line research tool.
The graphical extractor handles ordinary SVG extraction without those commands.

An individual asset is limited to 32 MiB, and a run can export up to 512 MiB.
Reaching a limit is recorded in `skips.csv`; other supported files continue
to be checked. No administrator access is requested.

## Build from source

From the repository root, run:

```powershell
.\tools\build-svg-extractor.ps1 -Package
```

This prints the standalone portable ZIP path. Omit `-Package` for just the
executable. The package contains the EXE, this guide, the catalog guide,
`LICENSE.txt` and `THIRD-PARTY-NOTICES.txt`. The selected distribution terms
cover this companion as well as Launcher. No source checkout,
PowerShell, Python, Visual Studio, Windows SDK, or NuGet package is needed to
run it. Building from source uses the Roslyn C# compiler supplied with Visual Studio Build Tools and targets .NET Framework.

The normal Launcher packaging targets build and include the same companion. Its
Product version is `1.1`. Its File version has an independent fourth component
defined in `tools/build-svg-extractor.ps1`. Increment that component when
changing the Extractor binary. Unchanged build inputs produce the same binary
across numbered Launcher packages of the same distribution kind.

The existing command-line scanner also accepts `--svg-only --export` to make
SVG-only copies with the same filenames and gallery.

Developer checks: run `.\tests\svg-extractor.ps1`. It builds isolated native and
managed fixtures, checks the actual window and scan lifecycle, and tests Keep
against a temporary destination. If Node.js is available it also exercises the
gallery's Keep button, filtering and error recovery. The combined download's
shipping checks also verify startup and normal close of its exact extractor
executable. No personal AppData files are changed by these checks.
