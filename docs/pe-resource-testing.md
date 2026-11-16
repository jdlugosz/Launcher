# PE resource artwork verification

The native resource reader and image cache are linked into the production
application and the test programs. The extractor reads without executing the
module, returns owned bytes and uses the same path resolution as icon loading.

Build Release targets `Launcher`, `LauncherTests`, `LauncherImageTests` and
`LauncherResourceProbe`. `LauncherResourceFixture.dll` is built automatically;
it contains only resources and has no entry point.

The checked-in original assets in `tests/resources` require no downloads or
Python during a build. The optional standard-library Python generator reproduces
them. SVG and PNG have known colors, transparency and asymmetric positions.
The resource script covers named/numeric types and names/IDs, Unicode names,
distinct payloads under different types and languages, and embedded binary zeros.

Focused checks:

```powershell
build/windows-x64/Release/LauncherTests.exe '[icon-source]'
build/windows-x64/Release/LauncherImageTests.exe '[icons]'
ctest --preset release -R 'Launcher\.(PeResourcesConfig|PeResourcesSmoke|DrawingSmoke)$'
```

The Catch2 checks compare exact extracted bytes with the original files, verify
explicit neutral and English selection, reject missing resources/languages and
invalid images, check cache separation and hits, and compare resource/file pixels
for both LunaSVG and WIC. Known interior colors and alpha independently check the
decoded result. SVG detection covers BOM/preamble/doctype/comments, namespace
prefixes, and a malformed suffix match that must fail actual decoding.

The integration probe parses a real configuration and exercises production
lookup and decoding. Every item must produce visible 48x48 pixels; there is no
stock fallback in that probe. The separate hidden-window smoke check exercises
Launcher drawing. A normal smoke success alone is insufficient evidence of a
requested icon loading, because normal Launcher fallback is deliberately retained.

Independent comparisons use the installed Resource Hacker:

```powershell
./tests/pe-resources.ps1
./tests/pe-resources.ps1 -IncludeInstalledExamples
```

The PowerShell driver runs Resource Hacker's documented CLI extraction to RES,
saves each invocation and log, selects the exact type/name/language tuple from
that standard container, and compares raw payload lengths and every byte against
the production reader. The RES headers are not part of that comparison. It writes source
and payload SHA-256 values to `comparison.csv`, the decode evidence to `probe.txt`,
and a labelled `examples.toml` for review. Each run requires a fresh directory.
An extraction process times out after 15 seconds rather than waiting on a tool
error dialog. Resource Hacker is not a dependency of ordinary CTest runs.
Hash-verified private copies of the source modules avoid 32-bit Resource Hacker's
WOW64 redirection of System32. The production probe reads the original paths.
Exporting a whole module to RES avoids Resource Hacker's individual-name mask
bugs with Unicode and dotted names. Unicode selectors are included in the
independent comparisons as well as the fixture assertions.

The optional installed examples are BatteryWidgetProvider's Bluetooth device,
webauthn's numeric SVG entry, InDesign's magnifying glass, and archived Zoom's
meeting icon. All four happen to be SVG; the fixture supplies PNG/WIC coverage.
Unavailable installation paths are explicitly reported as skipped. Ordinary tests
use the fixture and remain independent of those paths and application versions.

For visual acceptance, run the installed development Launcher with the absolute
path to `examples/pe-resources.toml`, or with the generated fixture/sample config.
The source sample uses environment/path macros and includes PNG. After review,
restart without a configuration argument to resume normal use. Automated results
do not claim interactive appearance approval; that sign-off remains with John.
