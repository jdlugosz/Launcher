# MSVC standard-module guard experiment

This standalone probe reproduces the adapter now used by Launcher's default
standard-module build. The ordinary-header production fallback remains
available with `build.cmd Release headers`. The generated adapter prevents
textual standard-library declarations in project and dependency headers from
being parsed again after `import std;`.

The working order on MSVC 19.51 is:

```cpp
#include <launcher/windows_pch.hpp>
#include <launcher/std_header_macros.hh>
import std;
#include <launcher/seenthemall.hh>
#include <launcher/dock.hpp>
#include <toml++/toml.hpp>
```

`std_header_macros.hh` processes the real C wrappers and `<version>` before the
import. They supply macros, global CRT names and overloads that named-module
imports do not replace. `seenthemall.hh` then sets actual public include guards
found in the selected MSVC STL. It does not invent SDK, internal STL or CRT
guards. `<cassert>` intentionally has no guard and retains its normal behavior.

The SDK/PCH goes first: MSVC `/Yu` skips source preceding its PCH boundary, and
importing first with the PCH disabled can redefine runtime types pulled in by
WRL. Both PCH-enabled and PCH-disabled sources should use the explicit order.

Regenerate the two headers after changes to the toolset or dependencies, from
an x64 developer PowerShell prompt at the Launcher repository root:

```powershell
./tools/generate-seenthemall.ps1 -StlInclude "$env:VCToolsInstallDir/include" -IncludeTests
```

The generator follows local/dependency includes from project source/header
files and, with `-IncludeTests`, test sources. It conservatively includes
conditional branches and pins the generated output to the detected STL version
and update. This is an implementation-specific adapter, not a portable standard
library facility. It should be validated again after regeneration.

To build the standalone probe with the installed CMake 4.3 and Ninja tools:

```powershell
cmake -S experiments/seenthemall -B build/seenthemall-example -G "Ninja Multi-Config"
cmake --build build/seenthemall-example --config Release
ctest --test-dir build/seenthemall-example -C Release --output-on-failure
cmake --build build/seenthemall-example --config Debug
ctest --test-dir build/seenthemall-example -C Debug --output-on-failure
```

CMake builds and links the standard module separately for each configuration.
Its module target and consumer share `/utf-8` and related language flags; using
only target-local `/utf-8` produced C5050 under `/WX` in the initial experiment.
The runtime library also matches: `/MT` in Release and `/MTd` in Debug.
The experimental import-std gate is pinned to CMake 4.3 and must be updated
when using another minor version.

The probe checks TOML parsing, formatting, errors and UTF-8, standard containers,
ranges/type traits, expected, streams, filesystem paths, format, macro availability
and WRL declarations. Separate compiler probes under `build/seenthemall-probe`
also exercised file-stream round trips. Initial failures and detailed compiler
flags are retained there and under `build/seenthemall-pch`.

Single warm compile/link measurements of the larger compiler probe were about
3.08 seconds using ordinary headers and 2.24 seconds using the generated shim,
with a previously compiled matching IFC. These are not whole-project build
benchmarks and exclude standard-module creation time.

The module's own implementation macros have already been expanded when its
headers are compiled into the module. Importing their declarations and template
bodies does not require those macros in the caller. The remaining macro need
comes from ordinary source still processed in the importing translation unit:
for example, toml++ uses `CHAR_BIT`, assertions, and feature-test conditions.
The real prelude supplies that source-level interface with its conditions intact.

A follow-up audit of all 45 masked headers found one additional public macro
interface outside this prelude: `<atomic>` supplies 13 `ATOMIC_*` macros,
including `ATOMIC_CHAR8_T_LOCK_FREE` conditional on `__cpp_lib_char8_t`.
Launcher and the audited dependency headers do not currently use those macros.
Consequently, this is an adapter for the tested source, not a replacement for
every standard header's complete macro interface. New code can generally use
the imported atomic types and constexpr properties directly.

An isolated candidate adding real `<atomic>` before import preserved that
interface and passed Release and Debug atomic initialization/operation checks.
The original prelude failed the same checks when they explicitly required the
macros. Evidence is in `build/seenthemall-probe/atomic-results.json`.
No atomic-header addition was made to the generated default prelude by this
audit. If later dependency source uses these macros, preserve the real header
before import or implement and validate a separate macro adapter.
MSVC `/EP /d1PP` also reproduced active conditional macro definitions for this
toolset, but `/d1PP` is absent from compiler help and is only a diagnostic aid.

Primary references:

- [MSVC named standard modules and their macro boundary](https://learn.microsoft.com/en-us/cpp/cpp/tutorial-import-stl-named-module?view=msvc-170)
- [MSVC PCH input boundary](https://learn.microsoft.com/en-us/cpp/build/reference/yu-use-precompiled-header-file?view=msvc-170)
- [CMake's standard-module target](https://cmake.org/cmake/help/v4.3/prop_tgt/CXX_MODULE_STD.html)
- [CMake 4.3 experimental import-std gate](https://github.com/Kitware/CMake/blob/v4.3.1/Help/dev/experimental.rst)
