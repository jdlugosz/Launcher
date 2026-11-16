#requires -Version 5.1
[CmdletBinding()]
param([Parameter(Mandatory)][Alias('PackageDirectory')][string]$SourceDirectory,
    [string]$InstallerDirectory,
    [string]$OutputDirectory = [IO.Path]::GetTempPath())

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$fixtureSourceDirectory = $SourceDirectory
if (!$InstallerDirectory) {
    $InstallerDirectory = if (Test-Path -LiteralPath (Join-Path $SourceDirectory 'Maintenance.ps1') -PathType Leaf) { $SourceDirectory }
        else { Join-Path $PSScriptRoot '..\tools\installer' }
}
. (Join-Path $InstallerDirectory 'Maintenance.ps1')
$SourceDirectory = $fixtureSourceDirectory
$script:checks = 0
function Check([bool]$Condition, [string]$Message) { $script:checks++; if (!$Condition) { throw $Message } }
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Text([string]$Path, [string]$Value) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
    [IO.File]::WriteAllText($Path, $Value, [Text.UTF8Encoding]::new($false))
}
function Fails([scriptblock]$Action, [string]$Message) { $failed = $false; try { & $Action | Out-Null } catch { $failed = $true }; Check $failed $Message }

$run = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('launcher-maintenance-' + [Guid]::NewGuid().ToString('N'))
$install = Join-Path $run 'selected install'
[void][IO.Directory]::CreateDirectory($install)
$sourceExe = Join-Path $SourceDirectory 'Launcher.exe'
if (!(Test-Path -LiteralPath $sourceExe)) { $sourceExe = Get-LauncherPackageExecutable -PackageDirectory $SourceDirectory }
$active = Join-Path $install 'Launcher.exe'
$retained = Join-Path $install 'Launcher-R1.1.0.8.exe'
[IO.File]::Copy($sourceExe, $active)
[IO.File]::Copy($sourceExe, $retained)
$extractor = Join-Path $install 'LauncherSvgExtractor.exe'
[IO.File]::Copy((Join-Path $SourceDirectory 'LauncherSvgExtractor.exe'), $extractor)
Text (Join-Path $install 'docs\SvgExtractor.md') 'owned SVG Extractor guide'
Text (Join-Path $install 'docs\advanced-icon-catalog.md') 'owned scanner guide'
$htmlGuides = @('docs\Launcher.html', 'docs\LauncherDrop.html', 'docs\Installer.html', 'docs\SvgExtractor.html',
    'docs\advanced-icon-catalog.html', 'docs\FileLocations.html',
    'docs\Configuration.html', 'docs\LaunchTargets.html', 'docs\Appearance.html')
$guideHashes = @{}
foreach ($name in $htmlGuides) {
    Text (Join-Path $install $name) "owned HTML guide $name"
    $guideHashes[$name] = Hash (Join-Path $install $name)
}
Text (Join-Path $install 'docs\personal.html') 'unrelated HTML remains'
[IO.File]::AppendAllText($active, 'owned active fixture differs from rollback')
$oldActiveHash = Hash $active
$retainedHash = Hash $retained
Text (Join-Path $install 'Launcher.toml') 'personal config remains'
Text (Join-Path $install 'Launcher.runtime.toml') 'personal runtime remains'
Text (Join-Path $install 'Icon\personal.ico') 'personal icon remains'
Text (Join-Path $install 'examples\colors-light.toml') 'edited palette remains'
Text (Join-Path $install 'LICENSE.txt') 'unmodified owned notice'
Text (Join-Path $install 'foreign.txt') 'unrelated remains'
Text (Join-Path $install 'Launcher-R3.0.exe') 'foreign executable with Launcher filename'
$otherInstall = Join-Path $run 'other install\Launcher.exe'
Text $otherInstall 'another install untouched'

$listed = @(Get-LauncherRetainedExecutables $install)
Check ($listed.Count -eq 2) 'List included a foreign executable or omitted owned Launcher.'
Check ($listed.Name -notcontains 'LauncherSvgExtractor.exe') 'The SVG Extractor appeared in the main-version rollback list.'
Fails { Set-LauncherRollbackVersion $install 'LauncherSvgExtractor.exe' -ProcessReader { @() } } 'Rollback accepted the companion as a main Launcher version.'
Check (@(Get-LauncherSupersededExecutables -Directory $install -IncomingVersion '9.0' | Where-Object Path -EQ $extractor).Count -eq 0) 'Main-version cleanup claimed the SVG Extractor.'
Fails { Assert-LauncherMaintenanceStopped $install -ProcessReader { [pscustomobject]@{ ExecutablePath = $extractor } } } 'Maintenance ignored a running SVG Extractor.'
Fails { Set-LauncherRollbackVersion $install '..\other.exe' -ProcessReader { @() } } 'Rollback accepted a path outside the selected installation.'
Fails { Set-LauncherRollbackVersion $install 'Launcher-R3.0.exe' -ProcessReader { @() } } 'Rollback accepted a foreign executable.'
Fails { Set-LauncherRollbackVersion $install ([IO.Path]::GetFileName($retained)) -ProcessReader { [pscustomobject]@{ ExecutablePath = $active } } } 'Rollback ignored an open process in the selected installation.'
Fails { Set-LauncherRollbackVersion $install ([IO.Path]::GetFileName($retained)) -ProcessReader { @() } -VerifyActive { throw 'forced post-publication verification failure' } } 'Rollback accepted failed activation verification.'
Check ((Hash $active) -eq $oldActiveHash) 'Failed rollback did not restore the previous active executable.'
Check ((Hash $retained) -eq $retainedHash) 'Failed rollback changed its retained target.'
$rolledBack = Set-LauncherRollbackVersion $install ([IO.Path]::GetFileName($retained)) -ProcessReader { @() } -CopyOnly
Check ((Hash $active) -eq $retainedHash) 'Rollback did not activate the selected bytes.'
Check ((Hash $rolledBack.PreviousExecutable) -eq $oldActiveHash) 'Rollback failed to retain the previous executable.'
Check ((Hash $retained) -eq $retainedHash) 'Rollback altered its retained target.'
Check ([IO.File]::ReadAllText((Join-Path $install 'Launcher.toml')) -eq 'personal config remains') 'Rollback overwrote the current profile.'
Check ([IO.File]::ReadAllText((Join-Path $install 'Launcher.runtime.toml')) -eq 'personal runtime remains') 'Rollback overwrote current runtime.'
foreach ($name in $htmlGuides) {
    Check ((Hash (Join-Path $install $name)) -eq $guideHashes[$name]) "Executable rollback changed the HTML guide: $name"
}

# Replacing an alias after removing its entry cannot write into its old target.
[IO.File]::Delete($active)
New-LauncherMaintenanceAlias $active $retained
Check ((Hash $active) -eq $retainedHash) 'Hard-link or verified-copy alias differs from its version.'
$copyFallback = Join-Path $install 'fallback.exe'
New-LauncherMaintenanceAlias $copyFallback $retained -CopyOnly
Check ((Hash $copyFallback) -eq $retainedHash) 'Explicit copy fallback differs from its version.'
[IO.File]::Delete($copyFallback)

$manifest = [pscustomobject]@{ SchemaVersion = 1; Files = @(
    [pscustomobject]@{ RelativePath = 'Launcher.exe'; Sha256 = Hash $active },
    [pscustomobject]@{ RelativePath = 'LauncherSvgExtractor.exe'; Sha256 = Hash $extractor },
    [pscustomobject]@{ RelativePath = 'docs\SvgExtractor.md'; Sha256 = Hash (Join-Path $install 'docs\SvgExtractor.md') },
    [pscustomobject]@{ RelativePath = 'docs\advanced-icon-catalog.md'; Sha256 = Hash (Join-Path $install 'docs\advanced-icon-catalog.md') },
    [pscustomobject]@{ RelativePath = 'LICENSE.txt'; Sha256 = Hash (Join-Path $install 'LICENSE.txt') },
    [pscustomobject]@{ RelativePath = 'examples\colors-light.toml'; Sha256 = '0' * 64 },
    [pscustomobject]@{ RelativePath = 'Launcher-R3.0.exe'; Sha256 = Hash (Join-Path $install 'Launcher-R3.0.exe') }
) }
$manifest.Files += @($htmlGuides | ForEach-Object { [pscustomobject]@{ RelativePath = $_; Sha256 = $guideHashes[$_] } })
$manifestPath = Join-Path $install 'Launcher.install-manifest.json'
Text $manifestPath ($manifest | ConvertTo-Json -Depth 5)
[IO.File]::AppendAllText($extractor, 'updated development companion after last package receipt')
$editedGuide = 'docs\Launcher.html'
[IO.File]::AppendAllText((Join-Path $install $editedGuide), ' personal annotations remain')
$editedGuideHash = Hash (Join-Path $install $editedGuide)
$plan = @(Get-LauncherUninstallFiles $install)
Check ($plan.Path -contains $extractor) 'Uninstall omitted an independently verified companion refreshed after the last package receipt.'
Check ($plan.Path -notcontains (Join-Path $install 'examples\colors-light.toml')) 'Uninstall plan includes edited palette.'
Check ($plan.Path -notcontains (Join-Path $install 'Launcher-R3.0.exe')) 'Uninstall trusts a foreign EXE solely because of its manifest hash.'
Check ($plan.Path -notcontains (Join-Path $install 'Launcher.toml')) 'Uninstall plan contains a personal configuration.'
Check ($plan.Path -notcontains (Join-Path $install $editedGuide)) 'Uninstall plan contains an edited HTML guide.'
Check ($plan.Path -notcontains (Join-Path $install 'docs\personal.html')) 'Uninstall plan sweeps unowned HTML files.'
foreach ($name in @($htmlGuides | Where-Object { $_ -ne $editedGuide })) {
    Check ($plan.Path -contains (Join-Path $install $name)) "Uninstall omitted a verified owned HTML guide: $name"
}
$lock = [IO.File]::Open((Join-Path $install 'LICENSE.txt'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try { Fails { Remove-LauncherInstallationFiles $install -ProcessReader { @() } } 'Uninstall ignored a locked support file.' } finally { $lock.Dispose() }
Check (Test-Path -LiteralPath $active) 'Uninstall partially removed files before access preflight finished.'
$null = Remove-LauncherInstallationFiles $install -ProcessReader { @() }
Check (!(Test-Path -LiteralPath $active)) 'Uninstall kept a verified owned active executable.'
Check (!(Test-Path -LiteralPath $retained)) 'Uninstall kept a verified owned retained executable.'
foreach ($name in @('LauncherSvgExtractor.exe', 'docs\SvgExtractor.md', 'docs\advanced-icon-catalog.md')) {
    Check (!(Test-Path -LiteralPath (Join-Path $install $name))) "Uninstall kept the owned companion file: $name"
}
foreach ($name in @($htmlGuides | Where-Object { $_ -ne $editedGuide })) {
    Check (!(Test-Path -LiteralPath (Join-Path $install $name))) "Uninstall kept an owned HTML guide: $name"
}
Check ((Hash (Join-Path $install $editedGuide)) -eq $editedGuideHash) 'Uninstall changed or removed an edited HTML guide.'
Check (!(Test-Path -LiteralPath $rolledBack.PreviousExecutable)) 'Uninstall kept a verified owned executable backup.'
foreach ($name in @('Launcher.toml', 'Launcher.runtime.toml', 'Icon\personal.ico', 'examples\colors-light.toml', 'foreign.txt', 'Launcher-R3.0.exe', 'docs\personal.html')) {
    Check (Test-Path -LiteralPath (Join-Path $install $name)) "Uninstall removed edited or unrelated file: $name"
}
Check ([IO.File]::ReadAllText($otherInstall) -eq 'another install untouched') 'Uninstall touched another installation.'
Check (!(Test-Path -LiteralPath $manifestPath)) 'Uninstall left its completed manifest.'

$invalid = Join-Path $run 'invalid manifest'
[void][IO.Directory]::CreateDirectory($invalid)
Text (Join-Path $invalid 'Launcher.install-manifest.json') '{"SchemaVersion":1,"Files":[{"RelativePath":"..\\outside.txt","Sha256":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}]}'
Fails { Get-LauncherUninstallFiles $invalid } 'Uninstall accepted a manifest path outside its installation.'
foreach ($name in @('docs\personal.html', 'docs\FileLocations.md')) {
    $unlisted = [pscustomobject]@{ SchemaVersion = 1; Files = @([pscustomobject]@{ RelativePath = $name; Sha256 = 'A' * 64 }) }
    Text (Join-Path $invalid 'Launcher.install-manifest.json') ($unlisted | ConvertTo-Json -Depth 5)
    Fails { Get-LauncherUninstallFiles $invalid } "Uninstall accepted an unlisted documentation manifest entry: $name"
}

$startup = Join-Path $run 'isolated Startup'
[void][IO.Directory]::CreateDirectory($startup)
$shortcutPath = Join-Path $startup 'Launcher.lnk'
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $otherInstall
$shortcut.Save()
Remove-LauncherMatchingStartupShortcut $install $startup
Check (Test-Path -LiteralPath $shortcutPath) "Uninstall removed another installation's Startup shortcut."
$shortcut.TargetPath = $active
$shortcut.Save()
Remove-LauncherMatchingStartupShortcut $install $startup
Check (!(Test-Path -LiteralPath $shortcutPath)) "Uninstall kept the selected installation's Startup shortcut."
[void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut)
[void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell)

# The uninstall coordinator uses the installer's pinned graceful-close path.
# Only isolated fake process handles are signaled by these deterministic cases.
$closeInstall = Join-Path $run 'Graceful uninstall'
$otherInstall = Join-Path $run 'Other running installation'
[void][IO.Directory]::CreateDirectory($closeInstall)
[void][IO.Directory]::CreateDirectory($otherInstall)
$closeExe = Join-Path $closeInstall 'Launcher.exe'
$versionedCloseExe = Join-Path $closeInstall 'Launcher-R1.1.0.10.exe'
foreach ($path in @($closeExe, $versionedCloseExe, (Join-Path $otherInstall 'Launcher.exe'))) { [IO.File]::Copy($sourceExe, $path) }
foreach ($name in @('LauncherDrop.exe', 'LauncherSvgExtractor.exe')) {
    [IO.File]::Copy((Join-Path $SourceDirectory $name), (Join-Path $closeInstall $name))
}
$closeSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$closeSession = [Diagnostics.Process]::GetCurrentProcess().SessionId
$closeContext = [pscustomobject]@{ UserSid = $closeSid; IsElevated = $false }
function New-CloseRecord([int]$Id, [string]$Path) {
    [pscustomobject]@{ ProcessId = $Id; ExecutablePath = $Path; OwnerSid = $closeSid;
        SessionId = $closeSession; CreationDate = [datetime]'2026-10-25T00:00:00Z' }
}
$closeState = [pscustomobject]@{
    Remaining = @(); Reads = 0; FailId = 0; ChangedId = 0; NewProcess = $null
    Closed = [Collections.Generic.List[int]]::new(); Disposed = [Collections.Generic.List[int]]::new()
}
function Reset-CloseFixture {
    $closeState.Remaining = @((New-CloseRecord 101 $closeExe), (New-CloseRecord 102 $versionedCloseExe),
        (New-CloseRecord 103 (Join-Path $otherInstall 'Launcher.exe')))
    $closeState.Reads = 0; $closeState.FailId = 0; $closeState.ChangedId = 0; $closeState.NewProcess = $null
    $closeState.Closed.Clear(); $closeState.Disposed.Clear()
}
$closeReader = {
    $closeState.Reads++
    $closeState.Remaining
    if ($closeState.Reads -gt 1 -and $null -ne $closeState.NewProcess) { $closeState.NewProcess }
}.GetNewClosure()
$closePin = {
    param($Id)
    $record = @($closeState.Remaining | Where-Object ProcessId -EQ $Id)[0]
    $pin = [pscustomobject]@{ ProcessId = $Id; Path = $record.ExecutablePath; OwnerSid = $record.OwnerSid;
        SessionId = $record.SessionId; CreationTimeUtc = $record.CreationDate.ToUniversalTime(); State = $closeState }
    if ($Id -eq $closeState.ChangedId) { $pin.CreationTimeUtc = $pin.CreationTimeUtc.AddSeconds(1) }
    $pin | Add-Member -MemberType ScriptMethod -Name Close -Value {
        param($Timeout)
        if ($this.ProcessId -eq $this.State.FailId) { throw 'Fixture graceful-close timeout.' }
        $this.State.Closed.Add($this.ProcessId)
        $this.State.Remaining = @($this.State.Remaining | Where-Object ProcessId -NE $this.ProcessId)
    }
    $pin | Add-Member -MemberType ScriptMethod -Name Dispose -Value { $this.State.Disposed.Add($this.ProcessId) }
    $pin
}.GetNewClosure()
$closeOptions = @{ Directory = $closeInstall; Context = $closeContext; SessionId = $closeSession;
    ProcessReader = $closeReader; ProcessPin = $closePin }
Reset-CloseFixture
$closed = Stop-LauncherUninstallProcesses @closeOptions
Check (($closed.StoppedProcessIds -join ',') -ceq '101,102') 'Uninstall did not close every verified selected Launcher instance.'
Check (($closeState.Remaining.ProcessId -join ',') -ceq '103') 'Uninstall signaled another installation.'
Check (($closeState.Disposed -join ',') -ceq '101,102') 'Uninstall did not release its pinned process handles.'
foreach ($unsafe in @('LauncherDrop.exe', 'LauncherSvgExtractor.exe', 'Unverified', 'OtherUser', 'OtherSession', 'MissingCreation', 'UnknownPath', 'EnumerationFailure')) {
    Reset-CloseFixture
    if ($unsafe -in @('LauncherDrop.exe', 'LauncherSvgExtractor.exe')) {
        $closeState.Remaining += New-CloseRecord 104 (Join-Path $closeInstall $unsafe)
    } elseif ($unsafe -eq 'Unverified') {
        $foreign = Join-Path $closeInstall 'Launcher-Foreign.exe'; Text $foreign 'Not a verified Launcher.'
        $closeState.Remaining += New-CloseRecord 104 $foreign
    } elseif ($unsafe -eq 'OtherUser') { $closeState.Remaining[1].OwnerSid = 'S-1-5-21-9-8-7-1001' }
    elseif ($unsafe -eq 'OtherSession') { $closeState.Remaining[1].SessionId++ }
    elseif ($unsafe -eq 'MissingCreation') { $closeState.Remaining[1].CreationDate = $null }
    elseif ($unsafe -eq 'UnknownPath') { $closeState.Remaining[1].ExecutablePath = '' }
    $options = $closeOptions.Clone()
    if ($unsafe -eq 'EnumerationFailure') { $options.ProcessReader = { throw 'Fixture enumeration unavailable.' } }
    Fails { Stop-LauncherUninstallProcesses @options } "Uninstall accepted unsafe running state: $unsafe"
    Check ($closeState.Closed.Count -eq 0) "Uninstall partially closed applications before rejecting $unsafe."
}
Reset-CloseFixture; $closeState.ChangedId = 102
Fails { Stop-LauncherUninstallProcesses @closeOptions } 'Uninstall trusted a changed process identity.'
Check ($closeState.Closed.Count -eq 0 -and $closeState.Disposed.Count -eq 2) 'A changed pin allowed partial close or leaked a handle.'
Reset-CloseFixture; $closeState.FailId = 102
Fails { Stop-LauncherUninstallProcesses @closeOptions } 'Uninstall ignored a graceful-close timeout.'
Check (($closeState.Closed -join ',') -ceq '101' -and $closeState.Disposed.Count -eq 2) 'Timeout handling forced another close or leaked a handle.'
Reset-CloseFixture; $closeState.NewProcess = New-CloseRecord 104 $closeExe
Fails { Stop-LauncherUninstallProcesses @closeOptions } 'Uninstall ignored a new instance after graceful shutdown.'

# Real disposable Launcher windows prove that WM_CLOSE waits for exit before
# removing the executable, while keeping each selected configuration/runtime.
$nativeInstall = Join-Path $run 'Native graceful uninstall'
[void][IO.Directory]::CreateDirectory($nativeInstall)
$nativeExe = Join-Path $nativeInstall 'Launcher.exe'
[IO.File]::Copy($sourceExe, $nativeExe)
foreach ($name in @('basic_colors.toml', 'extra_colors.toml', 'colors-light.toml', 'colors-dark.toml')) {
    [IO.File]::Copy((Join-Path $SourceDirectory ('examples\' + $name)), (Join-Path $nativeInstall $name))
}
$nativeConfig = Join-Path $nativeInstall 'isolated.toml'
$nativeConfig2 = Join-Path $nativeInstall 'second.toml'
$nativeRuntime = Join-Path $nativeInstall 'isolated.runtime.toml'
$nativeRuntime2 = Join-Path $nativeInstall 'second.runtime.toml'
$nativeLog = Join-Path $nativeInstall 'native.log'
$nativeLog2 = Join-Path $nativeInstall 'second.log'
Text $nativeConfig @'
schema_version = 1
[dock]
edge = "top"
display = "primary"
auto_hide = true
[grid]
columns = 1
icon_size_dip = 24
[[tabs]]
name = "Uninstall shutdown fixture"
color = "#BED878"
items = [{ icon = { stock = "folder" } }]
[logging]
file = "native.log"
'@
Text $nativeConfig2 ([IO.File]::ReadAllText($nativeConfig).Replace('native.log', 'second.log'))
Text $nativeRuntime "[dock]`nposition=0.21`n"
Text $nativeRuntime2 "[dock]`nposition=0.47`n"
$nativePreserved = @{}
foreach ($path in @($nativeConfig, $nativeConfig2, $nativeRuntime, $nativeRuntime2)) { $nativePreserved[$path] = Hash $path }
Text (Join-Path $nativeInstall 'LICENSE.txt') 'Owned native uninstall fixture.'
Text (Join-Path $nativeInstall 'Launcher.install-manifest.json') ([pscustomobject]@{
    SchemaVersion = 1; Files = @([pscustomobject]@{ RelativePath = 'LICENSE.txt'; Sha256 = (Hash (Join-Path $nativeInstall 'LICENSE.txt')) })
} | ConvertTo-Json -Depth 5)
function Read-MaintenanceLiveLog([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    $reader = [IO.StreamReader]::new($stream)
    try { $reader.ReadToEnd() } finally { $reader.Dispose() }
}
$nativeProcesses = [Collections.Generic.List[object]]::new()
try {
    foreach ($configuration in @($nativeConfig, $nativeConfig2)) {
        $nativeProcesses.Add((Start-Process -FilePath $nativeExe -ArgumentList @('--test-ui', '--config', ('"' + $configuration + '"')) -WindowStyle Hidden -PassThru))
    }
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    while ($deadline.ElapsedMilliseconds -lt 10000) {
        if ([IO.File]::Exists($nativeLog) -and [IO.File]::Exists($nativeLog2) -and
            (Read-MaintenanceLiveLog $nativeLog).Contains('Drawing initialized') -and
            (Read-MaintenanceLiveLog $nativeLog2).Contains('Drawing initialized')) { break }
        foreach ($process in $nativeProcesses) { if ($process.HasExited) { throw 'An owned native fixture exited before initializing.' } }
        Start-Sleep -Milliseconds 50
    }
    Check ($deadline.ElapsedMilliseconds -lt 10000) 'The native shutdown fixtures did not initialize.'
    $nativeClosed = Stop-LauncherUninstallProcesses -Directory $nativeInstall
    Check (@($nativeClosed.StoppedProcessIds).Count -eq 2) 'Native uninstall did not report both independently configured Launcher instances.'
    foreach ($process in $nativeProcesses) { Check ($process.WaitForExit(1000)) 'Uninstall did not wait for actual Launcher exit.' }
    Check ((Read-MaintenanceLiveLog $nativeLog).Contains('Graceful close requested.') -and
        (Read-MaintenanceLiveLog $nativeLog2).Contains('Graceful close requested.')) 'Native uninstall did not use normal graceful shutdown.'
    $null = Remove-LauncherInstallationFiles $nativeInstall
    Check (![IO.File]::Exists($nativeExe) -and ![IO.File]::Exists((Join-Path $nativeInstall 'Launcher.install-manifest.json'))) 'Native uninstall did not remove the stopped owned program.'
    foreach ($path in $nativePreserved.Keys) { Check ((Hash $path) -ceq $nativePreserved[$path]) 'Native uninstall changed a preserved configuration or runtime file.' }
} finally {
    foreach ($process in $nativeProcesses) {
        # Test-failure cleanup may terminate only children created above.
        if (!$process.HasExited) { $process.Kill(); [void]$process.WaitForExit(5000) }
        $process.Dispose()
    }
}

# Verify the customer reporter without opening a modal window in automation.
$reported = [Collections.Generic.List[object]]::new()
$display = { param($Message, $Failure) $reported.Add([pscustomobject]@{ Message = $Message; Failure = $Failure }) }.GetNewClosure()
Show-LauncherMaintenanceMessage -Message 'Completed fixture uninstall.' -Display $display
Show-LauncherMaintenanceMessage -Message 'Close the fixture Launcher and retry.' -Failure -Display $display
Check ($reported.Count -eq 2 -and !$reported[0].Failure -and $reported[1].Failure) 'Interactive maintenance lost the completion/error distinction.'
Check ($reported[1].Message -ceq 'Close the fixture Launcher and retry.') 'Interactive maintenance lost the actionable failure detail.'

# Exercise the real Windows PowerShell -File entry, rather than only dot-sourced
# functions. The only process reader is fake, native registry use is forbidden,
# and FileOnly keeps all Startup/profile operations out of these subprocesses.
$harness = Join-Path $run 'CLI harness'
[void][IO.Directory]::CreateDirectory($harness)
foreach ($name in @('Maintenance.ps1', 'Discovery.ps1', 'Deployment.ps1', 'Registration.ps1', 'Lifecycle.ps1')) {
    [IO.File]::Copy((Join-Path $InstallerDirectory $name), (Join-Path $harness $name))
}
[IO.File]::AppendAllText((Join-Path $harness 'Registration.ps1'), "`nfunction Invoke-LauncherRegistrationStore { throw 'Native registration is disabled in the CLI fixture.' }`n")
[IO.File]::AppendAllText((Join-Path $harness 'Lifecycle.ps1'), @'

$script:fixtureRecoveryPlan = (Get-Command Get-LauncherRecoveryShortcutPlan -CommandType Function).ScriptBlock
function Get-LauncherRecoveryShortcutPlan {
    param($Registration)
    if (!$env:LauncherMaintenanceFixtureRecoveryPrograms -or !$env:LauncherMaintenanceFixtureRecoveryRoot) {
        throw 'The isolated recovery shortcut fixture was not supplied.'
    }
    & $script:fixtureRecoveryPlan $Registration -ProgramsFolder $env:LauncherMaintenanceFixtureRecoveryPrograms `
        -ProgramRoot $env:LauncherMaintenanceFixtureRecoveryRoot
}

'@)
Text (Join-Path $harness 'Processes.ps1') @'
function Get-LauncherProcessCandidates {
    if ($env:LauncherMaintenanceFixtureRunning) {
        [pscustomobject]@{ ExecutablePath = (Join-Path $env:LauncherMaintenanceFixtureRunning 'Launcher.exe') }
    }
}
'@
$cliInstall = Join-Path $run 'CLI selected install'
[void][IO.Directory]::CreateDirectory($cliInstall)
$cliLicense = Join-Path $cliInstall 'LICENSE.txt'
$cliManifest = Join-Path $cliInstall 'Launcher.install-manifest.json'
$cliPersonal = Join-Path $cliInstall 'Launcher.toml'
Text $cliLicense 'Owned disposable CLI fixture.'
Text $cliPersonal 'Unowned configuration must survive.'
$cliLicenseHash = Hash $cliLicense
Text $cliManifest ([pscustomobject]@{ SchemaVersion = 1; Files = @([pscustomobject]@{ RelativePath = 'LICENSE.txt'; Sha256 = $cliLicenseHash }) } | ConvertTo-Json -Depth 5)
function Invoke-MaintenanceCli([switch]$Running) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $start.Arguments = '-NoLogo -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $harness 'Maintenance.ps1') +
        '" -Action Uninstall -InstallDirectory "' + $cliInstall + '" -FileOnly -NoElevation'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.EnvironmentVariables['PSModulePath'] = 'Z:\LauncherFixture\UnrelatedPowerShellModules'
    if ($Running) { $start.EnvironmentVariables['LauncherMaintenanceFixtureRunning'] = $cliInstall }
    else { $start.EnvironmentVariables.Remove('LauncherMaintenanceFixtureRunning') }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        Check ($process.Start()) 'The isolated maintenance CLI could not start.'
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(15000)) { $process.Kill(); throw 'The owned CLI fixture did not finish.' }
        return [pscustomobject]@{ ExitCode = $process.ExitCode; Output = $stdout.Result + $stderr.Result }
    } finally { $process.Dispose() }
}
$blocked = Invoke-MaintenanceCli -Running
Check ($blocked.ExitCode -ne 0 -and ([regex]::Replace($blocked.Output, '\s+', ' ')).Contains('Close Launcher and its companion tools')) 'The real CLI hid or ignored a running-application block.'
Check ((Hash $cliLicense) -ceq $cliLicenseHash -and [IO.File]::Exists($cliManifest)) 'A blocked CLI uninstall removed owned files or its receipt.'
$completed = Invoke-MaintenanceCli
Check ($completed.ExitCode -eq 0 -and $completed.Output.Contains('Launcher program files removed.')) ('The stopped headless CLI uninstall failed: ' + $completed.Output)
Check (![IO.File]::Exists($cliLicense) -and ![IO.File]::Exists($cliManifest)) 'The successful CLI uninstall kept its owned payload or receipt.'
Check ([IO.File]::ReadAllText($cliPersonal) -ceq 'Unowned configuration must survive.') 'The CLI uninstall changed a personal configuration.'

# Keep the real coordinator entry point and dependency graph. Override only its
# environment: an elevated identity, an in-memory registration store, an isolated
# Startup directory, and a captured dialog instead of a visible message box.
$coordinatorHarness = Join-Path $run 'Elevated coordinator harness'
[void][IO.Directory]::CreateDirectory($coordinatorHarness)
foreach ($name in @('Maintenance.ps1', 'Discovery.ps1', 'Deployment.ps1', 'Registration.ps1', 'Lifecycle.ps1', 'Processes.ps1')) {
    [IO.File]::Copy((Join-Path $harness $name), (Join-Path $coordinatorHarness $name))
}
$coordinatorScript = Join-Path $coordinatorHarness 'Maintenance.ps1'
$coordinatorText = [IO.File]::ReadAllText($coordinatorScript)
$parseErrors = $null; $parseTokens = $null
$coordinatorAst = [Management.Automation.Language.Parser]::ParseInput($coordinatorText, [ref]$parseTokens, [ref]$parseErrors)
Check ($parseErrors.Count -eq 0) 'The coordinator fixture could not parse the maintenance entry point.'
$main = @($coordinatorAst.EndBlock.Statements)[-1]
Check ($main -is [Management.Automation.Language.IfStatementAst] -and
    $main.Extent.Text.StartsWith("if (`$MyInvocation.InvocationName -ne '.')")) 'The coordinator fixture could not find the production main invocation.'
$fixtureEnvironment = @'
$script:fixtureStartupRemoval = (Get-Command Remove-LauncherMatchingStartupShortcut -CommandType Function).ScriptBlock
function Get-LauncherRegistrationContext {
    [pscustomobject]@{ UserSid = 'S-1-5-21-9-8-7-1001'; IsElevated = $true }
}
function Invoke-LauncherRegistrationStore {
    param($Operation, $Plan, $Snapshot)
    if (!$env:LauncherMaintenanceFixtureRegistry) { throw 'The fixture registration log was not supplied.' }
    switch ($Operation) {
        'AssertWritable' { return }
        'Read' {
            [pscustomobject]@{ Exists = $true; Values = [ordered]@{
                LauncherRegistrationIdentity = [pscustomobject]@{ Value = $Plan.Values.LauncherRegistrationIdentity }
            } }
            return
        }
        'Remove' {
            [IO.File]::WriteAllText($env:LauncherMaintenanceFixtureRegistry, $Plan.Scope + '|' + $Plan.Directory)
            return
        }
        default { throw "Unexpected registration operation in uninstall fixture: $Operation" }
    }
}
function Remove-LauncherMatchingStartupShortcut {
    param([string]$Directory)
    if (!$env:LauncherMaintenanceFixtureStartup -or !$env:LauncherMaintenanceFixtureCleanup) {
        throw 'The isolated Startup fixture was not supplied.'
    }
    [IO.File]::AppendAllText($env:LauncherMaintenanceFixtureCleanup, $Directory + "`n")
    & $script:fixtureStartupRemoval -Directory $Directory -StartupDirectory $env:LauncherMaintenanceFixtureStartup
}
function Show-LauncherMaintenanceMessage {
    param([string]$Message, [switch]$Failure)
    if (!$env:LauncherMaintenanceFixtureMessage) { throw 'The fixture dialog log was not supplied.' }
    [IO.File]::WriteAllText($env:LauncherMaintenanceFixtureMessage,
        ([pscustomobject]@{ Message = $Message; Failure = [bool]$Failure;
            StartupExists = [IO.File]::Exists((Join-Path $env:LauncherMaintenanceFixtureStartup 'Launcher.lnk')) } | ConvertTo-Json))
}

'@
Text $coordinatorScript ($coordinatorText.Insert($main.Extent.StartOffset, $fixtureEnvironment + "`n"))
$caller = Join-Path $coordinatorHarness 'Call-Maintenance.ps1'
Text $caller @'
param([string]$InstallDirectory, [switch]$FileOnly)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'Maintenance.ps1') -Action Uninstall -InstallDirectory $InstallDirectory `
    -NoElevation -Interactive -FileOnly:$FileOnly
'@

function Invoke-StartupMaintenanceCli($Fixture, [switch]$FileWorker, [switch]$Nested) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $start.Arguments = '-NoLogo -NoProfile -STA -ExecutionPolicy Bypass -File ' + $(if ($Nested) {
        '"' + $caller + '" -InstallDirectory "' + $Fixture.Install + '"'
    } else {
        '"' + $coordinatorScript + '" -Action Uninstall -InstallDirectory "' + $Fixture.Install + '" -NoElevation -Interactive'
    })
    if ($FileWorker) { $start.Arguments += ' -FileOnly' }
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    $start.EnvironmentVariables.Remove('LauncherMaintenanceFixtureRunning')
    $start.EnvironmentVariables['PSModulePath'] = 'Z:\LauncherFixture\UnrelatedPowerShellModules'
    $start.EnvironmentVariables['LauncherMaintenanceFixtureStartup'] = $Fixture.Startup
    $start.EnvironmentVariables['LauncherMaintenanceFixtureCleanup'] = $Fixture.Cleanup
    $start.EnvironmentVariables['LauncherMaintenanceFixtureMessage'] = $Fixture.Message
    $start.EnvironmentVariables['LauncherMaintenanceFixtureRegistry'] = $Fixture.Registry
    $start.EnvironmentVariables['LauncherMaintenanceFixtureRecoveryPrograms'] = $Fixture.RecoveryPrograms
    $start.EnvironmentVariables['LauncherMaintenanceFixtureRecoveryRoot'] = $Fixture.RecoveryRoot
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $start
    try {
        Check ($process.Start()) 'The isolated elevated maintenance fixture could not start.'
        $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(15000)) { $process.Kill(); throw 'The owned elevated maintenance fixture did not finish.' }
        [pscustomobject]@{ ExitCode = $process.ExitCode; Output = $stdout.Result + $stderr.Result }
    } finally { $process.Dispose() }
}

foreach ($scenario in @('Matching', 'Absent', 'Foreign', 'Versioned', 'FileOnly', 'PersonalFileOnly')) {
    # Shipping runs this fixture under a versioned build tree. Keep room for
    # the real Start menu link name and its atomic staging suffix on WinPS 5.1.
    $case = Join-Path $run ('e-' + $scenario)
    $scope = if ($scenario -eq 'PersonalFileOnly') { 'CurrentUser' } else { 'AllUsers' }
    $fileOnly = $scenario -in @('FileOnly', 'PersonalFileOnly')
    $owner = if ($scope -eq 'CurrentUser') { 'S-1-5-21-9-8-7-1001' } else { '' }
    $fixture = [pscustomobject]@{
        Install = Join-Path $case 'install'; Startup = Join-Path $case 'Startup'
        Cleanup = Join-Path $case 'cleanup.txt'; Message = Join-Path $case 'message.json'; Registry = Join-Path $case 'registration.txt'
        RecoveryPrograms = Join-Path $case 'Menu'; RecoveryRoot = Join-Path $case 'Program Files'
    }
    [void][IO.Directory]::CreateDirectory($fixture.Startup)
    [void][IO.Directory]::CreateDirectory($fixture.RecoveryPrograms)
    Text (Join-Path $fixture.Install 'LICENSE.txt') 'Owned elevated uninstall fixture.'
    Text (Join-Path $fixture.Install 'Maintenance.ps1') 'Owned disposable helper removed before Startup cleanup.'
    Text (Join-Path $fixture.Install 'Launcher.toml') 'Personal configuration must remain.'
    $fixtureLauncher = Join-Path $fixture.Install 'Launcher.exe'
    [IO.File]::Copy($sourceExe, $fixtureLauncher)
    $fixtureManifest = Join-Path $fixture.Install 'Launcher.install-manifest.json'
    Text $fixtureManifest ([pscustomobject]@{
        SchemaVersion = 1; InstallScope = $scope; InstallUserSid = $owner
        Files = @('LICENSE.txt', 'Launcher.exe', 'Maintenance.ps1' | ForEach-Object {
            [pscustomobject]@{ RelativePath = $_; Sha256 = Hash (Join-Path $fixture.Install $_) }
        })
    } | ConvertTo-Json -Depth 5)
    $link = Join-Path $fixture.Startup 'Launcher.lnk'
    $recoveryRegistration = Get-LauncherRegistrationPlan -Directory $fixture.Install -Scope $scope -UserSid $owner
    $recovery = Get-LauncherRecoveryShortcutPlan $recoveryRegistration -ProgramsFolder $fixture.RecoveryPrograms -ProgramRoot $fixture.RecoveryRoot
    $launch = Get-LauncherStartMenuLaunchPlan $recovery
    $oldRecovery = Get-LauncherLegacyRecoveryShortcutPlan $recovery
    $legacy = Join-Path $fixture.RecoveryPrograms 'Launcher Update History and Recovery.lnk'
    Text $legacy 'Unrelated legacy link fixture.'
    $otherProduct = Join-Path $recovery.Parent 'WindowRecall.lnk'
    Text $otherProduct 'Unrelated publisher shortcut fixture.'
    $recoveryIcon = $fixtureLauncher + ',0'
    Set-LauncherStartMenuShortcuts $recovery $recoveryIcon
    Set-LauncherStartMenuShortcut $oldRecovery $recoveryIcon
    $ownedLinks = @($launch.Path, $recovery.Path, $oldRecovery.Path)
    $ownedLinkHashes = @{}
    foreach ($path in $ownedLinks) { $ownedLinkHashes[$path] = Hash $path }
    $linkHash = $null
    if ($scenario -ne 'Absent') {
        $target = if ($scenario -eq 'Foreign') { Join-Path $case 'other install\Launcher.exe' }
            elseif ($scenario -eq 'Versioned') { Join-Path $fixture.Install 'Launcher-R1.1.0.10.exe' }
            else { Join-Path $fixture.Install 'Launcher.exe' }
        $shell = New-Object -ComObject WScript.Shell
        $shortcut = $null
        try {
            $shortcut = $shell.CreateShortcut($link); $shortcut.TargetPath = $target; $shortcut.Save()
            $linkHash = Hash $link
        } finally {
            if ($shortcut) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut) }
            [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell)
        }
    }
    # A script calling Maintenance creates a child scope, unlike direct -File.
    $result = Invoke-StartupMaintenanceCli -Fixture $fixture -FileWorker:$fileOnly -Nested:($scope -eq 'CurrentUser')
    Check ($result.ExitCode -eq 0) ("The elevated $scenario uninstall failed: " + $result.Output)
    Check (![IO.File]::Exists((Join-Path $fixture.Install 'LICENSE.txt')) -and ![IO.File]::Exists($fixtureLauncher) -and ![IO.File]::Exists($fixtureManifest) -and
        ![IO.File]::Exists((Join-Path $fixture.Install 'Maintenance.ps1'))) "The elevated $scenario uninstall kept its owned payload, helper, or manifest."
    Check ([IO.File]::ReadAllText((Join-Path $fixture.Install 'Launcher.toml')) -ceq 'Personal configuration must remain.') "The elevated $scenario uninstall changed personal settings."
    Check ([IO.File]::ReadAllText($fixture.Registry) -ceq ($scope + '|' + $fixture.Install)) "The elevated $scenario uninstall did not remove the selected registration."
    foreach ($path in $ownedLinks) {
        $shortcutIsCorrect = if ($scenario -eq 'PersonalFileOnly') { (Hash $path) -ceq $ownedLinkHashes[$path] }
            else { ![IO.File]::Exists($path) }
        Check $shortcutIsCorrect "The elevated $scenario uninstall left or changed a Launcher Start menu shortcut: $path"
    }
    Check ([IO.File]::ReadAllText($legacy) -ceq 'Unrelated legacy link fixture.' -and
        [IO.File]::ReadAllText($otherProduct) -ceq 'Unrelated publisher shortcut fixture.') `
        "The elevated $scenario uninstall changed an unrelated Start menu shortcut."
    Check ([IO.File]::Exists($fixture.Message)) "The elevated $scenario uninstall omitted its interactive completion."
    $notice = [IO.File]::ReadAllText($fixture.Message) | ConvertFrom-Json
    Check (!$notice.Failure -and $notice.Message.Contains('Launcher program files removed.')) "The elevated $scenario uninstall did not report success."
    Check (!$notice.Message.Contains('RemoveStartupShortcut') -and !$result.Output.Contains('RemoveStartupShortcut')) "The elevated $scenario uninstall requested a removed cleanup helper."
    if ($scenario -in @('Matching', 'Versioned', 'Absent')) {
        Check (![IO.File]::Exists($link) -and !$notice.StartupExists) "The elevated $scenario uninstall reported success before removing its matching Startup shortcut."
    } else {
        Check ((Hash $link) -ceq $linkHash -and $notice.StartupExists) "The elevated $scenario uninstall changed an unrelated or worker-owned Startup shortcut."
    }
    if ($fileOnly) {
        Check (![IO.File]::Exists($fixture.Cleanup)) 'The elevated file worker accessed Startup cleanup.'
    } else {
        Check ([IO.File]::ReadAllText($fixture.Cleanup) -ceq ($fixture.Install + "`n")) "The elevated $scenario coordinator did not perform exactly one current-account Startup cleanup."
    }
}
Write-Output "Maintenance checks passed: $script:checks; fixtures: $run"
