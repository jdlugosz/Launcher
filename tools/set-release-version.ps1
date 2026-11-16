#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('PrepareRelease', 'NextDevelopment', 'Set')][string]$Action,
    [string]$Version,
    [string]$RepositoryDirectory = (Split-Path -Parent $PSScriptRoot)
)

# This changes the source version only. Committing, signing a tag and packaging
# remain explicit steps so candidate testing is not hidden inside a build target.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repository = [IO.Path]::GetFullPath($RepositoryDirectory)
$cmakePath = Join-Path $repository 'CMakeLists.txt'
$manifestPath = Join-Path $repository 'vcpkg.json'
foreach ($path in @($cmakePath, $manifestPath)) {
    $entry = Get-Item -LiteralPath $path -Force -ErrorAction Stop
    if ($entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Version sources must be regular files: $path"
    }
}
$cmake = [IO.File]::ReadAllText($cmakePath)
$manifest = [IO.File]::ReadAllText($manifestPath)
$pattern = '(?m)(\bproject\(Launcher\s+VERSION\s+)(\d+\.\d+\.\d+)(?=\s)'
$match = [regex]::Match($cmake, $pattern)
if (!$match.Success) { throw "Cannot read Launcher's three-part source version: $cmakePath" }
$current = [version]$match.Groups[2].Value
$manifestVersion = [string](ConvertFrom-Json -InputObject $manifest).'version-semver'
if ($manifestVersion -cne $current.ToString(3)) { throw 'CMake and vcpkg source versions do not match.' }

if ($Version) {
    if ($Version -cnotmatch '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$') {
        throw 'Supply a three-part version such as 1.2.3; the prerelease package counter is separate.'
    }
    $next = [version]$Version
} else {
    switch ($Action) {
        'PrepareRelease' {
            if (($current.Minor % 2) -eq 0) { throw 'The source is already on an even release line. Use -Version for an explicit patch release.' }
            $next = [version]::new($current.Major, $current.Minor + 1, 0)
        }
        'NextDevelopment' {
            if (($current.Minor % 2) -ne 0) { throw 'The source is already on an odd development line.' }
            $next = [version]::new($current.Major, $current.Minor + 1, 0)
        }
        'Set' { throw '-Action Set requires -Version.' }
    }
}
if (@($next.Major, $next.Minor, $next.Build | Where-Object { $_ -gt 65535 }).Count) {
    throw 'Each Windows version component must be between 0 and 65535.'
}
if ($Action -eq 'PrepareRelease' -and ($next.Minor % 2) -ne 0) { throw 'A final release must have an even minor version.' }
if ($Action -eq 'NextDevelopment' -and ($next.Minor % 2) -ne 1) { throw 'A development version must have an odd minor version.' }
if ($next -le $current) { throw 'The new source version must be greater than the current source version.' }
$nextText = $next.ToString(3)
$updatedCmake = $cmake.Remove($match.Groups[2].Index, $match.Groups[2].Length).Insert($match.Groups[2].Index, $nextText)
$manifestMatch = [regex]::Match($manifest, '("version-semver"\s*:\s*")([^"\r\n]+)(")')
if (!$manifestMatch.Success) { throw "Cannot locate vcpkg's source version: $manifestPath" }
$updatedManifest = $manifest.Remove($manifestMatch.Groups[2].Index, $manifestMatch.Groups[2].Length).Insert($manifestMatch.Groups[2].Index, $nextText)
$plans = @(
    [pscustomobject]@{ Path = $cmakePath; Original = $cmake; Updated = $updatedCmake; Published = $false; Stage = ''; Backup = '' },
    [pscustomobject]@{ Path = $manifestPath; Original = $manifest; Updated = $updatedManifest; Published = $false; Stage = ''; Backup = '' }
)
try {
    foreach ($plan in $plans) {
        $plan.Stage = Join-Path $repository ('.launcher-version-' + [Guid]::NewGuid().ToString('N') + '.tmp')
        $plan.Backup = Join-Path $repository ('.launcher-version-' + [Guid]::NewGuid().ToString('N') + '.backup')
        [IO.File]::WriteAllText($plan.Stage, $plan.Updated, [Text.UTF8Encoding]::new($false))
    }
    foreach ($plan in $plans) {
        if ([IO.File]::ReadAllText($plan.Path) -cne $plan.Original) { throw "Version source changed while preparing its update: $($plan.Path)" }
        [IO.File]::Replace($plan.Stage, $plan.Path, $plan.Backup)
        $plan.Published = $true
    }
} catch {
    $failure = $_
    foreach ($plan in $plans) {
        if (!$plan.Published) { continue }
        if ([IO.File]::ReadAllText($plan.Path) -cne $plan.Updated) {
            throw "Version update failed and a source changed concurrently. Recover the prior version from $($plan.Backup). $($failure.Exception.Message)"
        }
        [IO.File]::Replace($plan.Backup, $plan.Path, [NullString]::Value)
        $plan.Published = $false
    }
    throw $failure
} finally {
    foreach ($plan in $plans) {
        if ($plan.Stage -and [IO.File]::Exists($plan.Stage)) { [IO.File]::Delete($plan.Stage) }
        if ($plan.Backup -and [IO.File]::Exists($plan.Backup) -and
            ([IO.File]::ReadAllText($plan.Path) -ceq $plan.Updated -or !$plan.Published)) { [IO.File]::Delete($plan.Backup) }
    }
}
[pscustomobject]@{
    PreviousVersion = $current.ToString(3); Version = $nextText; Action = $Action
    NextStep = if ($Action -eq 'PrepareRelease') {
        'Review and commit the version, create its signed tag, then run LauncherPackageRelease.'
    } else { 'Review and commit the source version before further development.' }
}
