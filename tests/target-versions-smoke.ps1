[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $Launcher,
    [Parameter(Mandatory)][string] $OutputDirectory,
    [string] $Probe
)
$ErrorActionPreference = 'Stop'
$versionLauncher = [IO.Path]::GetFullPath($Launcher)
$versionProbe = if ($Probe) { [IO.Path]::GetFullPath($Probe) } else {
    Join-Path ([IO.Path]::GetDirectoryName($versionLauncher)) 'LauncherLaunchGuiProbe.exe'
}
if (![IO.File]::Exists($versionLauncher) -or ![IO.File]::Exists($versionProbe)) { throw 'Launcher and its GUI probe must exist.' }
$versionProbeInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($versionProbe)
if (!$versionProbeInfo.ProductVersion -or !$versionProbeInfo.FileVersion) { throw 'The probe needs Windows version resources.' }
$versionOutput = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd([char[]]'\/') + [IO.Path]::DirectorySeparatorChar
$versionRun = [IO.Path]::GetFullPath((Join-Path $versionOutput ([Guid]::NewGuid().ToString('N'))))
if (!$versionRun.StartsWith($versionOutput, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture escaped OutputDirectory.' }
[void][IO.Directory]::CreateDirectory($versionRun)
function FixturePath([string] $Name) {
    $versionPath = [IO.Path]::GetFullPath((Join-Path $versionRun $Name))
    if (!$versionPath.StartsWith($versionRun + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Fixture escaped output: $Name" }
    return $versionPath
}
function FixtureFile([string] $Name, [string] $Content) {
    $versionPath = FixturePath $Name
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($versionPath))
    if ([IO.File]::Exists($versionPath)) { throw "Fixture already exists: $versionPath" }
    [IO.File]::WriteAllText($versionPath, $Content, [Text.UTF8Encoding]::new($false))
    return $versionPath
}
$versionLockedFile = $null
try {
    $versionSuitePaths = @('Suite 5.10\Build 2 & stable\bin\Version Probe.exe',
        'Suite 5.10\Build 1 & stable\bin\Version Probe.exe', 'Suite 5.9\Build 9 & preview\bin\Version Probe.exe')
    $versionSuitePaths = @($versionSuitePaths | ForEach-Object {
        $versionCopy = FixturePath $_
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($versionCopy))
        [IO.File]::Copy($versionProbe, $versionCopy, $false)
        $versionCopy
    })
    $versionDocumentPaths = @('Release 5.10\Guide & 资料.pdf', 'Release 5.9\Guide & 资料.pdf')
    $versionDocumentPaths = @($versionDocumentPaths | ForEach-Object { FixtureFile $_ 'A document without version resources.' })
    [void](FixtureFile 'Release 99.0\Other.pdf' 'This branch deliberately omits the final target filename.')
    $versionOtherDocumentPaths = @('Other release 2\Reference.pdf', 'Other release 1\Reference.pdf')
    $versionOtherDocumentPaths = @($versionOtherDocumentPaths | ForEach-Object { FixtureFile $_ 'A document on the initially hidden page.' })
    $versionLockedPath = FixturePath 'Locked 1\Version Probe.exe'
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($versionLockedPath))
    [IO.File]::Copy($versionProbe, $versionLockedPath, $false)
    # The real scanner must report this metadata read as an incomplete attempt,
    # while independent successful and known-empty searches still publish.
    $versionLockedFile = [IO.File]::Open($versionLockedPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
    $versionReceipt = FixturePath 'unexpected-program-execution.bin'
    $versionArguments = ('"' + $versionReceipt + '"').Replace('\', '\\').Replace('"', '\"')
    $versionOtherReceipt = FixturePath 'unexpected-second-page-execution.bin'
    $versionOtherArguments = ('"' + $versionOtherReceipt + '" second-page-options').Replace('\', '\\').Replace('"', '\"')
    $versionSuitePattern = 'Suite *\Build *\bin\Version Probe.exe'
    $versionDocumentPattern = 'Release *\Guide & 资料.pdf'
    $versionMissingPattern = 'Absent *\Missing.exe'
    $versionLockedPattern = 'Locked *\Version Probe.exe'
    $versionOtherDocumentPattern = 'Other release *\Reference.pdf'
    $versionAdobeProduct = 'Adobe nonexistent fixture product'
    $versionConfig = FixtureFile 'target-versions.toml' @"
schema_version = 1
[dock]
edge = 'top'
display = 'primary'
auto_hide = false
suppress_fullscreen = false
[grid]
columns = 4
rows = 'auto'
[logging]
file = 'target-versions.log'
[[tabs]]
name = 'General target versions'
short_name = 'Versions'
color = 'salmon'
items = [
    { target = '$versionSuitePattern', label = 'Suite & versions', arguments = "$versionArguments", show = 'minimized' },
    { target = '$versionDocumentPattern', label = 'Guide & 资料', icon = { stock = 'document' } },
    { target = '$versionMissingPattern', label = '' },
    { target = '$versionLockedPattern', label = 'Locked metadata', icon = { stock = 'application' } },
    { kind = 'adobeCC', target = '$versionAdobeProduct', label = 'Missing Adobe' },
]
[[tabs]]
name = 'Other target versions'
short_name = 'Other'
color = '#F3CE82'
items = [
    { target = '$versionSuitePattern', label = 'Same search, other options', arguments = "$versionOtherArguments", working_directory = '.', show = 'hidden' },
    { target = '$versionOtherDocumentPattern', label = 'Other page document', icon = { stock = 'document' } },
]
"@
    $versionLog = FixturePath 'target-versions.log'
    $versionProcess = Start-Process -FilePath $versionLauncher -ArgumentList @('--smoke-test', '--config', ('"' + $versionConfig + '"')) -WorkingDirectory $versionRun -WindowStyle Hidden -PassThru
    try {
        if (!$versionProcess.WaitForExit(20000)) {
            $versionProcess.Kill(); $versionProcess.WaitForExit()
            throw 'Target-version smoke timed out after 20 seconds.'
        }
        $versionProcess.Refresh()
        $versionPid = $versionProcess.Id
        $versionExit = $versionProcess.ExitCode
    } finally { $versionProcess.Dispose() }
    $versionLines = @(@("$versionLog.1", $versionLog) | ForEach-Object {
        if ([IO.File]::Exists($_)) { [IO.File]::ReadAllLines($_) | Where-Object { $_ -match (' pid=' + $versionPid + ' ') } }
    })
    if ($versionExit -ne 0) { $versionLines | Select-Object -Last 8 | Write-Output; throw "Native smoke failed (exit $versionExit)." }
    if (@($versionLines | Where-Object { $_.Contains('; config=' + $versionConfig + '.') }).Count -ne 1) { throw 'Missing isolated configuration startup evidence.' }
    if ([IO.File]::Exists($versionReceipt) -or [IO.File]::Exists($versionOtherReceipt)) { throw 'A configured probe executed during smoke.' }
    foreach ($versionProduct in @($versionSuitePattern, $versionDocumentPattern, $versionMissingPattern, $versionLockedPattern, $versionAdobeProduct)) {
        if (@($versionLines | Where-Object { $_.Contains('Smoke installed versions passed: product=' + $versionProduct + ';') }).Count -ne 1) {
            throw "Missing menu/tooltip/pinned-option smoke evidence for $versionProduct"
        }
    }
    foreach ($versionGroup in @(@{ Pattern = $versionSuitePattern; Paths = $versionSuitePaths }, @{ Pattern = $versionDocumentPattern; Paths = $versionDocumentPaths })) {
        $versionRecords = @($versionLines | Where-Object { $_.Contains('Smoke installed versions version: product=' + $versionGroup.Pattern + ';') })
        if ($versionRecords.Count -ne $versionGroup.Paths.Count) { throw "Wrong number of discovered versions: $($versionGroup.Pattern)" }
        for ($versionIndex = 0; $versionIndex -lt $versionRecords.Count; ++$versionIndex) {
            $versionDefault = if ($versionIndex -eq 0) { 'yes' } else { 'no' }
            if ($versionGroup.Pattern -eq $versionSuitePattern -and !$versionRecords[$versionIndex].Contains('; version=' + $versionProbeInfo.ProductVersion + ';')) {
                throw 'Copied executable version resources were not retained in the discovered versions.'
            }
            if (!$versionRecords[$versionIndex].Contains('; default=' + $versionDefault + '; executable=' + $versionGroup.Paths[$versionIndex] + '.')) {
                throw "Wrong version order or default selection: $($versionGroup.Pattern)"
            }
        }
    }
    if (@($versionLines | Where-Object { $_.Contains('Smoke installed versions version:') }).Count -ne 5) { throw 'Unrelated versions leaked into a missing target group.' }
    foreach ($versionRequired in @('Smoke test passed:', 'Smoke installed versions discovery service passed:', 'Smoke version activation passed:')) {
        if (!($versionLines | Where-Object { $_.Contains($versionRequired) })) { throw "Missing native evidence: $versionRequired" }
    }
    foreach ($versionSearch in @(
        @{ Pattern = $versionSuitePattern; Count = 3 },
        @{ Pattern = $versionDocumentPattern; Count = 2 },
        @{ Pattern = $versionMissingPattern; Count = 0 },
        @{ Pattern = $versionOtherDocumentPattern; Count = 2 }
    )) {
        $versionSearchIdentity = '; search=' + (Join-Path $versionRun $versionSearch.Pattern) + ';'
        $versionAttempts = @($versionLines | Where-Object {
            $_.Contains('Version discovery attempt:') -and
                $_.IndexOf($versionSearchIdentity, [StringComparison]::OrdinalIgnoreCase) -ge 0 -and
                $_.Contains('; status=complete;') -and $_.Contains('; observed=' + $versionSearch.Count + ';')
        })
        if (!$versionAttempts.Count) { throw "Missing independent complete attempt: $($versionSearch.Pattern)" }
    }
    $versionLockedIdentity = '; search=' + (Join-Path $versionRun $versionLockedPattern) + ';'
    $versionLockedAttempts = @($versionLines | Where-Object {
        $_.Contains('Version discovery attempt:') -and
            $_.IndexOf($versionLockedIdentity, [StringComparison]::OrdinalIgnoreCase) -ge 0 -and
            $_.Contains('; status=incomplete; retained=no; observed=1; error=32.')
    })
    if (!$versionLockedAttempts.Count) { throw 'Missing native sharing-violation evidence for the independent locked search.' }
    $versionErrorLog = FixturePath 'target-versions.filesystem-errors.log'
    if (![IO.File]::Exists($versionErrorLog)) { throw 'Missing separate filesystem-error journal.' }
    $versionErrorRecords = @([IO.File]::ReadAllLines($versionErrorLog) | ForEach-Object { $_ | ConvertFrom-Json })
    $versionLockedErrors = @($versionErrorRecords | Where-Object {
        $_.path -eq $versionLockedPath -and $_.error_code -eq 32 -and $_.error_category -eq 'Win32' -and
            $_.timestamp -and $_.error_text
    })
    if (!$versionLockedErrors.Count) { throw 'Journal omitted the full locked-file path, native error code or Windows text.' }
    if ($versionErrorRecords | Where-Object { $_.path -like '*\Absent *\*' }) {
        throw 'An ordinary missing match was incorrectly written as a filesystem fault.'
    }
    if ($versionLines | Where-Object { $_.Contains('Version background cache:') }) { throw 'Legacy mixed-cache refresh ran during the service smoke.' }
    [pscustomobject]@{ Passed = $true; Pid = $versionPid; Exit = $versionExit; Configuration = $versionConfig; Log = $versionLog
        MetadataVersions = $versionSuitePaths; DocumentVersions = $versionDocumentPaths
        InitiallyHiddenDocumentVersions = $versionOtherDocumentPaths; LockedMetadataTarget = $versionLockedPath
        FilesystemErrorLog = $versionErrorLog; FilesystemErrorRecords = $versionErrorRecords.Count
        ExecutedConfiguredPrograms = $false
    } | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (FixturePath 'results.json') -Encoding UTF8
    "Target-version native smoke passed: independent searches, natural ordering, missing branches, locked metadata, page activation, menus/tooltips/pinned options and discovery completion. Evidence: $versionRun"
} catch { Write-Warning "Target-version smoke evidence preserved: $versionRun"; throw }
finally { if ($versionLockedFile) { $versionLockedFile.Dispose() } }
