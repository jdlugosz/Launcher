# Build and test Launcher

## Prerequisites

- Windows x64 and Visual Studio or Build Tools with **Desktop development with
  C++**, the Windows SDK, CMake, and Ninja. Use MSVC 14.51 or later with support
  for the compiler option `/std:c++23preview`.
- CMake 4.3 for the default MSVC standard-library module build, or CMake 3.25
  or later for the ordinary-header build.
- A bootstrapped vcpkg checkout. Set `VCPKG_ROOT` to its path. The
  [vcpkg manifest](../vcpkg.json) pins the registry baseline and includes
  toml++, LunaSVG, and PlutoVG. The checked-in presets enable the feature that
  installs Catch2 for tests.
- PowerShell 7 (`pwsh`) for HTML guide generation. Setup itself runs on the
  Windows PowerShell 5.1 included with Windows.
- The JMDlib.Cpp revision recorded by Launcher. In a Git checkout, run the
  following from the repository root. Repeat it after switching to a revision
  that pins a different commit.

  ```powershell
  git submodule update --init -- vendor/JMDlib.Cpp
  ```

Both compiler modes use JMDlib.Cpp. A source archive needs a copy of that
dependency at `vendor/JMDlib.Cpp` or an explicit path supplied with
`LAUNCHER_JMDLIB_CPP_SOURCE_DIR`. Configuration reads the dependency locally.
A source build can proceed without Launcher Git metadata. Formal release
packaging needs Git for version verification.

## Build from the command line

Run these commands from the repository root in an initialized x64 Visual
Studio developer terminal.

```powershell
$env:VCPKG_ROOT = 'C:\path\to\vcpkg'
cmake --preset windows-x64
cmake --build --preset release
ctest --preset release
```

The executable is `build\windows-x64\Release\Launcher.exe`. To build the
application target alone, use `cmake --build --preset release --target Launcher`.
For focused testing, give CTest a pattern, for example
`ctest --preset release -R '^Launcher\.HtmlGuides$'`.

The `build.cmd` wrapper finds Visual Studio, initializes the x64 environment,
closes Launcher copies running from its build folders, then configures,
builds, and runs the selected configuration's full test suite. It defaults
to Release and accepts `Debug` and `RelWithDebInfo`.
`.\build.cmd Release headers` selects ordinary headers. The corresponding
configure preset is `windows-x64-headers`, the build and test preset is
`release-headers`, and the executable is
`build\windows-x64-headers\Release\Launcher.exe`.

To build against a separate JMDlib.Cpp checkout, configure with its path.
Clear the cached path with `-DLAUNCHER_JMDLIB_CPP_SOURCE_DIR=` to return to
the pinned checkout.

```powershell
cmake --preset windows-x64 '-DLAUNCHER_JMDLIB_CPP_SOURCE_DIR=C:/path/to/JMDlib.Cpp'
```

The `Release` compiler configuration selects optimized binaries. Follow the
[release procedure](release-procedure.md) to select a distribution kind,
verify the package, and generate a download.

## Build from VS Code

Open the repository folder. On a fresh checkout, run **Launcher: Configure x64**
from **Terminal → Run Build Task**. **Ctrl+Shift+B** runs **Launcher: Build
Release**, which builds the application. Both tasks initialize the MSVC x64
environment and preserve your chosen vcpkg checkout.

The **Launcher x64 PowerShell** terminal opens in `build\windows-x64`. There,
`ninja Launcher` builds the Release application. If using CMake Tools, select
the `windows-x64` configure preset and the `release` build preset.
The optional [Launcher TOML extension](../tools/vscode-launcher-toml/README.md)
highlights the application's `%` pragmas. Use standard TOML mode when you need
TOML language-server validation or formatting.

## Generate the guides

A normal build renders the bundled guides as standalone HTML in the build
configuration's `docs` folder. You can generate the guides with PowerShell 7
before compiling Launcher.

```powershell
pwsh -NoProfile -File tools/build-html-guides.ps1 -OutputDirectory build/html-guides
```

See [Generate PNG illustrations](preview-images.md) for the screenshot shortcut
and command-line rendering.

## Use an installed development build

Run focused checks before replacing a working installation. From your regular
desktop session, run `.\tools\accept-build.ps1` to copy the Release build into
an existing writable Launcher installation and start it. The script hash-checks
staged files, saves the previous executable, and preserves personal settings.
The first transition from a preserved release also saves a copy of the personal
configuration when present and keeps any existing snapshot.

`Run-Launcher.cmd` starts the accepted installation. Pass a configuration path
to test with a separate profile. The scripts request startup through the
desktop Explorer so Launcher uses the interactive user's profile. For the
installed and portable paths, see [Configuration and file locations](file-locations.md).

## Diagnostics

### Build identity and logs

**About… → Copy details** provides the full Git commit when available, version,
compiler configuration, UTC build time, and any recorded instrumentation. A
`+` after the short commit identifies a build from a modified tree. About uses
an exact, trusted signed version tag on the commit when one is available. See
the [release procedure](release-procedure.md#prepare-and-verify-the-release-commit)
for the tag checks.

The build stamp refreshes during compilation. CMake detects common sanitizer,
coverage, and profiling flags. If you instrument the executable with other
tools, set `-DLAUNCHER_BUILD_INSTRUMENTATION="Code coverage"` or another
descriptive label in the configure command. This option records a label in build
details and the Windows resource. Configure the instrumentation separately.
Instrumented builds cannot be packaged.

For an installed copy, the default logs are in
`%LOCALAPPDATA%\Długosz\Launcher`. The application log records startup, graphics,
and window activity. The filesystem journal records external version-discovery
errors as UTF-8 JSON lines with the timestamp, affected path, error code, and
diagnostic text. If you redirect the application log, the journal follows its
directory and filename stem.

```toml
[logging]
file = 'logs/launcher.log'
```

An explicit log path may be absolute or relative to the selected configuration.
Logs rotate at 256 KiB and the latest backup has a `.1` suffix. See
[Configuration and file locations](file-locations.md) for profile paths and
[External errors and useful diagnostics](manual/version-discovery/chapters/08-errors.md)
for journal retention and notification behavior.

### Automated checks

- `Launcher.exe --smoke-test --config <path>` draws through the native graphics
  APIs in a hidden window and reports success or failure. CTest supplies
  explicit configuration fixtures to bypass the personal profile.
- `Launcher.TabStackingSmoke` checks page selection, exposed tab areas, and click
  ownership across multiple rows. Drawing recovery checks cover backbuffer
  recovery and simulate returning to the desktop from elevation.
- `Launcher.DisplayEdges` tests two orientations on each connected monitor.
  It uses isolated configurations and checks only their own logs. Its report
  identifies whether secondary-monitor and different-DPI coverage were available.
  Core native checks that share the default log use the
  `LauncherNativeSmoke` CTest resource lock.
- CTest cleans up generated `*.runtime.toml` files in test locations. After a
  separate interactive test with a repository sample, run
  `cmake --build --preset release --target LauncherCleanTestRuntime`.

### Interactive probes

- `Launcher.exe --test-ui` gives the window a taskbar entry for automation and
  logs event counts after five seconds and on exit. `Launcher.exe --stop`
  closes running copies from the same executable path.
- `LauncherFullscreenProbe.exe --display primary --duration-ms 2000` creates a
  temporary fullscreen window. Add `--focus-other-display` or `--cover` for
  those cases. Use `--color-key-overlay` separately to test a transparent
  overlay. Escape closes the probe early. Pass `--inspect-windows <output-path>`
  for a read-only UTF-8 report of foreground, taskbar, and visible ruler-overlay
  window metadata without window titles.
- `LauncherFullscreenEventsProbe.exe <Launcher.exe> <evidence-directory>`
  runs event-routing checks with temporary configurations and windows, including
  cross-display checks when another display is connected. This tool and the
  fullscreen probe are separate from the CTest suite.

## Compiler modes and rendering dependencies

The default build imports `std` and the shared JMDlib.Cpp modules. The
ordinary-header presets use the same vcpkg dependencies and corresponding
JMDlib.Cpp headers. Run `cmake --preset windows-x64 --fresh` from the x64
developer environment if enabling modules in a previously configured
header-mode build directory.

Native targets use private precompiled headers for Windows SDK declarations.
Project code, imports, and third-party headers stay outside those PCHs.
In module mode `std_header_macros.hh` provides required C wrappers and standard
feature macros before import. The generated `seenthemall.hh` adapter guards
against repeated declarations in standard headers included indirectly by
project and third-party headers. The [standalone compiler probe](../experiments/seenthemall/README.md)
reproduces this combination.

CMake runs the [adapter generator](../tools/generate-seenthemall.ps1) against
the selected compiler and dependencies, then tracks input changes during
builds. It writes the adapter under `generated/launcher` in the build directory.
The adapter depends on MSVC's header guards. Code that needs a new
standard-header macro should acquire the real definition before import.

Win32 manages the dock and menus, Direct2D draws shapes and bitmaps,
DirectWrite draws text, WIC decodes images, and the Windows Shell supplies
stock icons. The C++ runtime and vcpkg libraries link statically. Windows
system DLLs remain runtime dependencies.

## Implementation notes

- [Design notes](design-notes.md) record implementation decisions.
- The [version-discovery manual](manual/version-discovery/README.md) explains
  scanning, presentation, exact-file actions, and testing.
