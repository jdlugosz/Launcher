# Advanced scanner reference: SVG and XML-based artwork

For finding artwork to use in Launcher, use **Launcher SVG Extractor**, included
beside Launcher. Its guide is `docs\SvgExtractor.md` in the Launcher download or
`README.txt` in the standalone extractor ZIP.
It shares this scanner, with program/folder pickers, cancellation and a gallery.
Its **Keep** button copies chosen SVGs into Launcher's AppData `Icons` folder.
It can also reopen exported catalog galleries, including existing mixed-format
catalogs, without scanning their source files again.

This document describes the separate **AdvancedIconCatalog** command-line tool
for source-tree users and researchers. It remains useful for broader SVG, XAML,
BAML and other resource inventories. The graphical extractor replaces these
commands for everyday SVG extraction; you do not need to build or run the
command-line tool to use it. The commands below require a source checkout and
are not commands to run from the Launcher download folder.

The standalone scanner inventories files on mounted disks and archive volumes.
Programs do not have to be installed or runnable. It reads PE/CLR resource bytes
directly, without loading a target assembly, running a program, deserializing
resource objects, or resolving that program's dependencies. Both 32-bit and
64-bit Windows images can be scanned on this computer.

No Python installation, Blender, Visual Studio, Windows SDK or NuGet package is
needed. The executable uses Windows' .NET Framework 4.6.2 or later. The
PowerShell runner builds it with Windows' supplied Framework compiler.
It works in Windows PowerShell 5.1 and PowerShell 7. Copy the resulting executable
elsewhere to use it independently of the source checkout.

## Run from the source tree

Choose one or more directories, drives, or individual files:

```powershell
.\tools\scan-advanced-icons.ps1 -Roots 'C:\Program Files', 'H:\Archived Programs'
```

By default, results go into a fresh directory under `build\icon-catalog`. The
runner prints the location. To choose it yourself:

```powershell
.\tools\scan-advanced-icons.ps1 -Roots 'H:\' `
    -OutputDirectory 'D:\Icon catalogs\Archive-H'
```

The output directory must not already exist. This prevents accidental replacement
of an earlier catalog. To scan all ready fixed, removable and mapped network
drives visible to the current account:

```powershell
.\tools\scan-advanced-icons.ps1 -AllDrives `
    -Exclude 'C:\Windows\WinSxS' `
    -OutputDirectory 'D:\Icon catalogs\All-drives'
```

Excluding `WinSxS` is optional: it avoids cataloging many copies of Windows
components. There is no default exclusion of that directory. All requested
exclusions appear in `summary.json`. The output directory itself is always
excluded. For an unmounted archive disk, mount it first or supply its UNC path.
The scanner does not mount disk images or change permissions.

## Add views to an existing catalog

There is no need to walk the disks again. Use the earlier **resources.csv** to
open only its listed physical files and select their exact resources:

```powershell
.\tools\scan-advanced-icons.ps1 `
    -CatalogPath 'D:\Icon catalogs\All-drives\resources.csv' -ExportAssets `
    -OutputDirectory 'D:\Icon catalogs\All-drives-with-views'
```

The original CSV format works too. Files are grouped and each containing file
is opened once; package entries and resource keys select the payloads within it.
Directory traversal is disabled in this mode. The disks must be mounted at the
paths recorded in the catalog. Missing files/resources and changed resource
formats are reported in the new `skips.csv`. Copies reflect the files' current
contents, rather than a snapshot from the earlier scan. You may filter `resources.csv`
to export just a selection; keep the File, Member, Location and Kind columns.
Duplicate requests are combined. Do not combine `-CatalogPath` with roots or
`-AllDrives`.

With `-ExportAssets`, **examples.html** includes SVG image previews and links to
copied assets. Open it in a browser; keep its `assets` folder beside it. The
gallery is searchable, paginated at 100 entries per page, and shows all unique
exports. All copies and their original locations remain listed in the
new CSV. The Kind column uses a color emoji status with one legend, without
repeating hover text on each entry.

Copies use SHA-256 filenames inside `assets`, with identical payloads of the
same format sharing a file. Resource names never become output paths. Native
payload bytes and managed byte arrays/streams are preserved; gzip is expanded
and .NET string resources are encoded as UTF-8. SVG can be viewed immediately.
XAML is copied as markup; BAML, XBF, PRI and custom serialized resource data are
copied as raw files, without decompiling, running or deserializing them. An index
or reference is not automatically followed to other files.

Exports default to a **512 MiB total budget**, in addition to the per-asset
limit. Raise it with `-MaxExportMB 2048` / `--max-export-mb 2048`. When the budget
is exhausted, cataloging continues and omitted copies are recorded in skips.
CSV mode processes files containing SVG/XAML first. The HTML page renders SVG
only in an image element, with browser image restrictions; it never inserts SVG
markup into the page, renders XAML, or executes target code.

## Use the executable directly

Build without scanning:

```powershell
.\tools\scan-advanced-icons.ps1 -BuildOnly
```

This prints the path to a build under
`build\tools\advanced-icon-catalog\<source-hash>\AdvancedIconCatalog.exe`.
Copy that single executable to any convenient folder. For example:

```powershell
& 'D:\Utilities\AdvancedIconCatalog.exe' 'H:\' `
    --output 'D:\Icon catalogs\Archive-H'
```

Or add copies and previews to an existing inventory:

```powershell
& 'D:\Utilities\AdvancedIconCatalog.exe' `
    --from-catalog 'D:\Icon catalogs\Archive-H\resources.csv' --export `
    --output 'D:\Icon catalogs\Archive-H-with-views'
```

`--help` lists all options. Use a different output directory for each disk or
scan, which keeps provenance clear when volumes are later disconnected.

## Read the catalog

- **examples.html** without exports is a searchable, self-contained page of up to 250 selected
  examples. It favors native embedded SVG, then managed SVG and vector XAML.
  It shows at most one example per file/package member and kind so one icon
  library does not fill the page. With exports, it shows the unique asset gallery
  described above. The CSVs contain all hits.
- **files.csv** has one row per physical file with findings and counts by kind.
  For a package, these counts include its inspected members.
- **resources.csv** has one row per finding: physical path, package member,
  resource locator, kind, confidence, decoded payload size, name hint and evidence,
  plus ExportedFile (relative to the output folder) and AssetSha256. Empty export
  fields indicate no copied payload; a hash can be present when the copy budget
  prevented writing the file.
  A locator such as `PE/SVG/APP_LOGO/#1033` identifies a native resource; a locator
  such as `CLR/Demo.g.resources/images/logo.svg` identifies a managed resource.
- **skips.csv** records unreadable paths, reparse points, invalid files, parsing
  failures, unsupported custom resource data and size/depth limits when relevant.
- **summary.json** records roots, exclusions, counts, limits, time and whether the
  scan finished, was interrupted, failed, or stopped at an explicit limit. It
  also records the SHA-256 of the scanner executable used for that run, export
  counts/bytes, and the source catalog and request counts in CSV mode.
- **progress.txt** is updated about every ten seconds during a scan. CSVs are
  flushed at the same interval and when the scan finishes. The final state is
  written to progress.txt too.

The CSVs use UTF-8 with a BOM for Excel. Cells beginning with formula characters
are prefixed with an apostrophe to preserve them as text when opened in a
spreadsheet. The HTML page displays names as text. Without export mode it contains
metadata only; export mode adds the copied SVG previews described above.

The confidence column matters:

| Kind | What the scanner established |
| --- | --- |
| `svg` / confirmed | Parsed XML with an SVG root, whether loose, native, managed, in a package, or gzip-compressed. This can be artwork other than the app icon. |
| `vector-xaml` / confirmed | Parsed WPF/WinUI/Avalonia XAML containing drawing or path elements. This establishes vector markup, not its use as the Shell icon. |
| `baml`, `xbf` / candidate | Compiled XAML or a resource named for that format. It may describe ordinary windows, controls or themes. It is not decompiled. |
| `pri` / candidate | A Windows package resource index. It can index raster artwork, text and other resources; it is not itself proof of vector art. |
| `xaml` / candidate | XAML markup without recognized drawing/path elements. |
| `svg-named-resource` / candidate | The name suggests SVG, but the payload was not recognized as SVG XML. |
| `svg-reference`, `package-logo`, `linked-resource` / reference | A resource/manifest names another asset. The referenced bytes have not necessarily been found. |
| `compiled-resource` / candidate | A BAML/XBF-named value uses custom serialization, which is deliberately not deserialized. |

Names containing `icon`, `logo`, `brand` or `appimage` receive a name hint. This
is a heuristic, not evidence of which image Explorer or a running window uses.
A `.NET` header alone is not a finding. Ordinary PNG/ICO resources are not listed.

## Scope and limits

Native executable/library extensions (including `.mun` resource modules and
common Adobe/plugin extensions), `.resources`, `.svg`, `.svgz`, `.xaml`,
`.axaml`, `.baml`, `.xbf`, `.pri`, `.xml` and `.manifest` are considered. Native
resources are inspected by content, so SVG can be found in a custom type or
`RCDATA`, without a `.svg` resource name. Text may be UTF-8, UTF-16 or UTF-32.
The XML sniff uses the first 4 KiB; a known `.svg`/`.xaml` name also triggers
inspection. External XML entities/DTDs are not resolved.

MSIX/APPX packages and bundles, VSIX packages and NuGet packages are inspected
as ZIP containers without extracting them. Include general `.zip` files with
`-IncludeZip` or `--include-zip`. Package nesting is limited to two levels.
Resource-container/gzip nesting is limited to four levels. Resource directories
and managed resource tables are bounded to 200,000 entries.

The default maximum decoded asset or package-member size is 32 MiB; raise it
with `-MaxAssetMB 128` or `--max-mb 128`. A large on-disk EXE/DLL can still be
inspected using small reads: the limit applies to its individual resources,
not its total file size. Package members require bounded decompression.

This is an inventory of recognized formats, not a universal executable unpacker.
It does not unpack arbitrary installers, CAB/7z/RAR archives, encrypted packages,
.NET single-file bundle overlays, Electron ASAR, Qt resource collections, or
proprietary/compressed resource layouts other than gzip. Extract such containers
with their normal tools first. It does not decompile BAML/XBF, decode PRI indexes,
or inspect icon glyphs in font files. SDK `MakePri dump` can investigate a listed
PRI separately. Custom serialized .NET objects are not materialized; an image
hidden in one can therefore be missed. Ordinary UI resources and web artwork
remain distinguishable from confirmed Shell app-logo references.

Paths longer than 260 characters are supported by the scanner's manifest and
modern Framework path handling. Junctions, symbolic links and cloud placeholder
reparse points are not followed.
This avoids loops and unintended cloud recalls. Use the actual target directory
when you want to scan it. Permission failures are logged and scanning continues;
the utility never requests elevation. A finished scan can still have skips.
Root disks and files are read-only; only the chosen catalog directory is written.

## Interrupt or bound a scan

Ctrl+C ends a foreground scan and finishes a partial catalog. From another
terminal, creating an empty `cancel.request` in its output directory does the
same thing. The scanner checks for it about once a second between operations:

```powershell
New-Item -ItemType File 'D:\Icon catalogs\Archive-H\cancel.request'
```

For a quick sample, use `-MaxFiles 10000` / `--max-files 10000` or
`-MaxSeconds 60` / `--max-seconds 60`. These produce a clearly marked partial
catalog. Counts refer to relevant files inspected, not every filesystem entry.
There is no resume of an unfinished directory walk; scan one volume or program
tree at a time for convenient partitions. A flushed resources.csv, including one
from a partial scan, can be used for targeted export. New runs preserve existing
catalogs.

Exit codes: **0** traversal finished (consult skips), **1** fatal failure,
**2** explicit file/time limit, **130** interruption/cancel request. A forced
process termination or power loss can leave flushed CSVs without a final summary.

## Developer verification and format references

Run the focused standalone check from the repository root:

```powershell
.\tests\advanced-icon-catalog.ps1
```

It builds isolated x86/x64 test assemblies with native UTF-8/UTF-16/gzip SVG and
managed SVG, XAML and BAML resources, plus a package fixture. It checks resource
keys that are not legal filenames, opaque SVG candidates, malformed inputs,
raw custom-object handling, paths longer than 260 characters, scan limits,
cancellation and overwrite protection. It also checks CSV-only targeting,
quoted/newline keys, nested ZIP members, byte-preserving exports, deduplication,
raw compiled resources, export budgets, report layout and emoji status markers.
Generated fixtures and reports remain under ignored `build` directories.

Format references:

- [Microsoft PE format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)
- [ECMA-335 CLI metadata tables](https://ecma-international.org/publications-and-standards/standards/ecma-335/)
- [WPF resource/content/data files](https://learn.microsoft.com/en-us/dotnet/desktop/wpf/app-development/wpf-application-resource-content-and-data-files)
- [.NET ResourceReader implementation](https://github.com/dotnet/runtime/blob/main/src/libraries/System.Private.CoreLib/src/System/Resources/ResourceReader.cs)
- [MakePri command-line options](https://learn.microsoft.com/en-us/windows/uwp/app-resources/makepri-exe-command-options)
