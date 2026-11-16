[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $Launcher,
    [Parameter(Mandatory)][string] $OutputDirectory,
    [ValidateSet('All', 'Close', 'Fatal', 'Restart', 'Retained', 'Cold', 'ColdFailure')][string] $Mode = 'All',
    [switch] $RealSMB
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3
$scenarioLauncher = [IO.Path]::GetFullPath($Launcher)
$scenarioInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($scenarioLauncher)
if (!$scenarioInfo.IsSpecialBuild -or !$scenarioInfo.SpecialBuild.Contains('version-scenarios')) {
    throw 'This driver requires the separately instrumented version-scenarios executable.'
}
if (!('LauncherVersionScenarioNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class LauncherVersionScenarioNative {
    public delegate bool Callback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] static extern bool EnumWindows(Callback callback, IntPtr parameter);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr window, StringBuilder name, int length);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool PostMessage(IntPtr window, uint message, UIntPtr parameter, IntPtr process);
    public static IntPtr Window(uint process) {
        IntPtr found=IntPtr.Zero;
        EnumWindows((window, unused) => {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner==process) {
                var name=new StringBuilder(128);
                GetClassName(window,name,name.Capacity);
                if (name.ToString()=="Launcher.DrawingPrototype.Window") { found=window; return false; }
            }
            return true;
        },IntPtr.Zero);
        return found;
    }
}
'@
}
function TomlString([string] $Value) { return '"' + $Value.Replace('\', '\\').Replace('"', '\"').Replace("`r", '\r').Replace("`n", '\n') + '"' }
function WaitCondition([scriptblock] $Condition, [string] $Description, [int] $Milliseconds = 10000) {
    $scenarioWatch = [Diagnostics.Stopwatch]::StartNew()
    do {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 25
    } while ($scenarioWatch.ElapsedMilliseconds -lt $Milliseconds)
    throw "Timed out: $Description"
}
function ReadSharedText([string] $Path) {
    if (![IO.File]::Exists($Path)) { return '' }
    $scenarioStream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $scenarioReader = [IO.StreamReader]::new($scenarioStream, [Text.Encoding]::UTF8)
    try {
        return $scenarioReader.ReadToEnd()
    } finally { $scenarioReader.Dispose() }
}
function ReadTrace([string] $Path) {
    $scenarioText = ReadSharedText $Path
    return @($scenarioText.Split("`n") | Where-Object { $_.EndsWith('}') } | ForEach-Object { $_ | ConvertFrom-Json })
}
function Manifest([string] $Run, [int] $ProcessId) {
    return Join-Path $Run ('evidence\process-' + $ProcessId + '\manifest.json')
}
function Trace([string] $Run, [int] $ProcessId) {
    return Join-Path $Run ('evidence\process-' + $ProcessId + '\trace.jsonl')
}
function Control($Manifest, [int] $Command, [int] $Index = 0) {
    $scenarioWindow = [LauncherVersionScenarioNative]::Window([uint32]$Manifest.pid)
    if ($scenarioWindow -eq [IntPtr]::Zero) { throw "No Launcher window for owned scenario PID $($Manifest.pid)." }
    $scenarioParameter = [UIntPtr]::new([uint64](($Index -shl 16) -bor $Command))
    if (![LauncherVersionScenarioNative]::PostMessage($scenarioWindow, [uint32]$Manifest.control_message, $scenarioParameter, [IntPtr]::new([long]$Manifest.pid))) {
        throw 'Scenario control message could not be posted.'
    }
}
function NewFixture([string] $Run, [string] $Case) {
    [void][IO.Directory]::CreateDirectory($Run)
    [IO.File]::WriteAllText((Join-Path $Run 'LauncherVersionScenario.fixture'), "Launcher version scenario fixture v1`n")
    foreach ($scenarioDocument in @('Local 1\Guide.txt', 'Held 1\Guide.txt', 'Held 2\Guide.txt')) {
        $scenarioPath = Join-Path $Run $scenarioDocument
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($scenarioPath))
        [IO.File]::WriteAllText($scenarioPath, 'Owned version-scenario fixture; no executable dispatch.')
    }
    $scenarioConfig = Join-Path $Run 'fixture.toml'
    $scenarioLocal = TomlString (Join-Path $Run 'Local *\Guide.txt')
    $scenarioHeld = TomlString (Join-Path $Run 'Held *\Guide.txt')
    [IO.File]::WriteAllText($scenarioConfig, @"
schema_version = 1
[dock]
edge = 'top'
display = 'primary'
auto_hide = false
suppress_fullscreen = false
[logging]
file = 'unused-scenario-config.log'
[[tabs]]
name = 'Instrumented version scenarios'
items = [
    { target = $scenarioLocal, label = 'Independent local', icon = { stock = 'document' } },
    { target = $scenarioHeld, label = 'Held fixture', arguments = 'owned-options', working_directory = '.', show = 'minimized', icon = { stock = 'document' } },
]
[[tabs]]
name = 'Activation boundary'
items = [ { label = 'Empty page', icon = { stock = 'document' } } ]
"@, [Text.UTF8Encoding]::new($false))
    $scenarioPath = Join-Path $Run 'scenario.toml'
    $scenarioConfigText = TomlString $scenarioConfig
    $scenarioOutputText = TomlString (Join-Path $Run 'evidence')
    $scenarioHeldPlans = if ($Case -eq 'Retained') {
        "{ outcome = 'native' }, { outcome = 'incomplete', error = 53 }, { outcome = 'failed', error = 5 }"
    } elseif ($Case -eq 'Cold') {
        "{ outcome = 'native', gate = true, gate_stage = 'cursor' }, {}"
    } elseif ($Case -eq 'ColdFailure') {
        "{ outcome = 'failed', gate = true }, {}"
    } else { "{ outcome = 'native', gate = true, gate_stage = 'cursor' }" }
    [IO.File]::WriteAllText($scenarioPath, @"
schema_version = 1
instance = '$([Guid]::NewGuid().ToString('N'))'
configuration = $scenarioConfigText
output_directory = $scenarioOutputText
dispatch = 'capture-only'
[[searches]]
id = 'local'
path = $scenarioLocal
attempts = [ {}, {}, {} ]
[[searches]]
id = 'held'
path = $scenarioHeld
attempts = [ $scenarioHeldPlans ]
"@, [Text.UTF8Encoding]::new($false))
    return [pscustomobject]@{ Config = $scenarioConfig; Scenario = $scenarioPath }
}
$scenarioRoot = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
if ($RealSMB -and !$scenarioRoot.StartsWith('\\')) { throw '-RealSMB requires the explicitly owned UNC fixture directory.' }
[void][IO.Directory]::CreateDirectory($scenarioRoot)
$scenarioCases = if ($Mode -eq 'All') { @('Close', 'Fatal', 'Restart', 'Retained', 'Cold', 'ColdFailure') } else { @($Mode) }
$scenarioResults = @()
foreach ($scenarioCase in $scenarioCases) {
    $scenarioRun = [IO.Path]::GetFullPath((Join-Path $scenarioRoot ($scenarioCase.ToLowerInvariant() + '-' + [Guid]::NewGuid().ToString('N'))))
    if (!$scenarioRun.StartsWith($scenarioRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture escaped OutputDirectory.' }
    $scenarioFixture = NewFixture $scenarioRun $scenarioCase
    $scenarioOwned = [Collections.Generic.List[Diagnostics.Process]]::new()
    $scenarioManifests = [Collections.Generic.List[object]]::new()
    try {
        $scenarioProcess = Start-Process -FilePath $scenarioLauncher -ArgumentList @('--version-scenario', ('"' + $scenarioFixture.Scenario + '"'), '--config', ('"' + $scenarioFixture.Config + '"')) -WorkingDirectory $scenarioRun -WindowStyle Hidden -PassThru
        $scenarioOwned.Add($scenarioProcess)
        $scenarioPid = $scenarioProcess.Id
        $scenarioManifestPath = Manifest $scenarioRun $scenarioPid
        WaitCondition { [IO.File]::Exists($scenarioManifestPath) -or $scenarioProcess.HasExited } 'scenario control manifest'
        if ($scenarioProcess.HasExited) { throw "Instrumented Launcher exited before startup ($($scenarioProcess.ExitCode))." }
        $scenarioManifest = [IO.File]::ReadAllText($scenarioManifestPath) | ConvertFrom-Json
        $scenarioManifests.Add($scenarioManifest)
        $scenarioTrace = Trace $scenarioRun $scenarioPid
        $scenarioLog = Join-Path (Split-Path -Parent $scenarioManifestPath) 'launcher.log'
        WaitCondition { [LauncherVersionScenarioNative]::Window([uint32]$scenarioPid) -ne [IntPtr]::Zero } 'scenario UI window'
        WaitCondition { @(ReadTrace $scenarioTrace | Where-Object { $_.stage -eq 'finished' -and $_.search -eq 'local' }).Count -eq 1 } 'independent local discovery'
        Control $scenarioManifest 5
        WaitCondition { @(ReadTrace $scenarioTrace | Where-Object { $_.stage -eq 'ui-checkpoint' }).Count -ge 1 } 'responsive UI checkpoint'
        if ($scenarioCase -eq 'Cold' -or $scenarioCase -eq 'ColdFailure') {
            $scenarioHeld = @($scenarioManifest.searches | Where-Object { $_.id -eq 'held' })[0].attempts[0]
            $scenarioEntered = [Threading.EventWaitHandle]::OpenExisting($scenarioHeld.entered)
            try { if (!$scenarioEntered.WaitOne(5000)) { throw 'Cold provider did not enter its deterministic gate.' } }
            finally { $scenarioEntered.Dispose() }
            Control $scenarioManifest 6 1
            Control $scenarioManifest 4 1
            $scenarioRelease = [Threading.EventWaitHandle]::OpenExisting($scenarioHeld.release)
            try { [void]$scenarioRelease.Set() } finally { $scenarioRelease.Dispose() }
            $scenarioOutcome = if ($scenarioCase -eq 'Cold') { 'complete' } else { 'failed' }
            WaitCondition {
                $scenarioLines = ReadSharedText $scenarioLog
                return @($scenarioLines.Split("`n") | Where-Object {
                    $_.Contains('Version discovery attempt:') -and $_.IndexOf('Held *\Guide.txt;', [StringComparison]::OrdinalIgnoreCase) -ge 0 -and $_.Contains("status=$scenarioOutcome;")
                }).Count -ge 1
            } 'terminal cold service result while another page is shown'
            Control $scenarioManifest 5
            WaitCondition { @(ReadTrace $scenarioTrace | Where-Object { $_.stage -eq 'ui-checkpoint' }).Count -ge 2 } 'cold-intent UI barrier'
            if ($scenarioCase -eq 'Cold') {
                WaitCondition { @(ReadTrace $scenarioTrace | Where-Object { $_.stage -eq 'action-captured' }).Count -eq 1 } 'once-only cold launch capture'
                $scenarioCapture = @(ReadTrace $scenarioTrace | Where-Object { $_.stage -eq 'action-captured' })[0]
                if ($scenarioCapture.target -ne (Join-Path $scenarioRun 'Held 2\Guide.txt') -or $scenarioCapture.arguments -ne 'owned-options' -or
                        [IO.Path]::GetFullPath($scenarioCapture.working_directory).TrimEnd('\') -ne $scenarioRun.TrimEnd('\') -or $scenarioCapture.show -ne 2) {
                    throw 'The cold intent lost its original target/options after switching pages.'
                }
            } elseif (@(ReadTrace $scenarioTrace | Where-Object { $_.stage -eq 'action-captured' }).Count) {
                throw 'The failed cold intent dispatched an action.'
            }
            Control $scenarioManifest 4 0
            WaitCondition { @(ReadTrace $scenarioTrace | Where-Object { $_.stage -eq 'finished' -and $_.search -eq 'held' -and $_.attempt -eq 2 }).Count -eq 1 } 'later successful activation'
            WaitCondition {
                $scenarioLines = ReadSharedText $scenarioLog
                return @($scenarioLines.Split("`n") | Where-Object {
                    $_.Contains('Version discovery attempt:') -and $_.IndexOf('Held *\Guide.txt;', [StringComparison]::OrdinalIgnoreCase) -ge 0 -and $_.Contains('status=complete;')
                }).Count -ge $(if ($scenarioCase -eq 'Cold') { 2 } else { 1 })
            } 'later successful service result'
            Control $scenarioManifest 5
            WaitCondition { @(ReadTrace $scenarioTrace | Where-Object { $_.stage -eq 'ui-checkpoint' }).Count -ge 3 } 'post-retry UI barrier'
            $scenarioExpectedCaptures = if ($scenarioCase -eq 'Cold') { 1 } else { 0 }
            if (@(ReadTrace $scenarioTrace | Where-Object { $_.stage -eq 'action-captured' }).Count -ne $scenarioExpectedCaptures) {
                throw 'A completed/failed cold intent was resurrected on a later activation.'
            }
            Control $scenarioManifest 1
        } elseif ($scenarioCase -eq 'Retained') {
            WaitCondition { @(ReadTrace $scenarioTrace | Where-Object { $_.stage -eq 'finished' -and $_.search -eq 'held' -and $_.attempt -eq 1 }).Count -eq 1 } 'initial complete catalog'
            [void][IO.Directory]::CreateDirectory((Join-Path $scenarioRun 'Held 3'))
            [IO.File]::WriteAllText((Join-Path $scenarioRun 'Held 3\Guide.txt'), 'Newly observed partial evidence.')
            foreach ($scenarioAttempt in @(2, 3)) {
                Control $scenarioManifest 4 1
                Control $scenarioManifest 4 0
                WaitCondition { @(ReadTrace $scenarioTrace | Where-Object { $_.stage -eq 'finished' -and $_.search -eq 'held' -and $_.attempt -eq $scenarioAttempt }).Count -eq 1 } "held attempt $scenarioAttempt"
                $scenarioExpected = if ($scenarioAttempt -eq 2) { 'incomplete' } else { 'failed' }
                WaitCondition {
                    if (![IO.File]::Exists($scenarioLog)) { return $false }
                    $scenarioLines = ReadSharedText $scenarioLog
                    return $scenarioLines.Contains("status=$scenarioExpected; retained=yes;")
                } "retained $scenarioExpected service result"
                Control $scenarioManifest 6 1
                WaitCondition { @(ReadTrace $scenarioTrace | Where-Object { $_.stage -eq 'action-captured' }).Count -eq ($scenarioAttempt - 1) } 'captured retained target'
            }
            $scenarioCaptures = @(ReadTrace $scenarioTrace | Where-Object { $_.stage -eq 'action-captured' })
            foreach ($scenarioCapture in $scenarioCaptures) {
                if ($scenarioCapture.target -ne (Join-Path $scenarioRun 'Held 2\Guide.txt') -or $scenarioCapture.arguments -ne 'owned-options' -or
                        [IO.Path]::GetFullPath($scenarioCapture.working_directory).TrimEnd('\') -ne $scenarioRun.TrimEnd('\') -or $scenarioCapture.show -ne 2) {
                    throw 'An incomplete/failed refresh changed the retained launch target, arguments, working directory, or show mode.'
                }
            }
            Control $scenarioManifest 1
        } else {
            $scenarioHeld = @($scenarioManifest.searches | Where-Object { $_.id -eq 'held' })[0].attempts[0]
            $scenarioEntered = [Threading.EventWaitHandle]::OpenExisting($scenarioHeld.entered)
            try { if (!$scenarioEntered.WaitOne(5000)) { throw 'Held provider did not enter its deterministic gate.' } }
            finally { $scenarioEntered.Dispose() }
            if (@(ReadTrace $scenarioTrace | Where-Object { $_.stage -eq 'released' -and $_.search -eq 'held' }).Count) { throw 'Held worker was unexpectedly released.' }
            $scenarioCommand = switch ($scenarioCase) { 'Close' { 1 } 'Fatal' { 2 } 'Restart' { 3 } }
            Control $scenarioManifest $scenarioCommand
        }
        if (!$scenarioProcess.WaitForExit(15000)) { throw "Scenario parent failed to finish: $scenarioCase" }
        $scenarioProcess.Refresh()
        $scenarioExpectedExit = if ($scenarioCase -eq 'Fatal') { 1 } else { 0 }
        if ($scenarioProcess.ExitCode -ne $scenarioExpectedExit) {
            $scenarioExitValue = $scenarioProcess.ExitCode
            $scenarioExitDescription = if ($null -eq $scenarioExitValue) { '<null>' } else { "$scenarioExitValue ($($scenarioExitValue.GetType().FullName))" }
            throw "Unexpected $scenarioCase exit: $scenarioExitDescription; expected $scenarioExpectedExit"
        }
        if (!(ReadTrace $scenarioTrace | Where-Object { $_.stage -eq 'owners-cleaned' })) { throw 'Explicit UI/COM/log cleanup evidence is missing.' }
        $scenarioChildPid = $null
        if ($scenarioCase -eq 'Restart') {
            WaitCondition { @(Get-ChildItem -LiteralPath (Join-Path $scenarioRun 'evidence') -Filter manifest.json -Recurse -File | Where-Object { $_.FullName -ne $scenarioManifestPath }).Count -eq 1 } 'replacement scenario manifest'
            $scenarioChildFile = @(Get-ChildItem -LiteralPath (Join-Path $scenarioRun 'evidence') -Filter manifest.json -Recurse -File | Where-Object { $_.FullName -ne $scenarioManifestPath })[0]
            $scenarioChildManifest = [IO.File]::ReadAllText($scenarioChildFile.FullName) | ConvertFrom-Json
            $scenarioManifests.Add($scenarioChildManifest)
            $scenarioChildPid = [int]$scenarioChildManifest.pid
            $scenarioChild = [Diagnostics.Process]::GetProcessById($scenarioChildPid)
            if ($scenarioChild.MainModule.FileName -ne $scenarioLauncher) { throw 'Replacement PID is not the instrumented fixture executable.' }
            # GetProcessById initially owns only an identity. Pin its real process
            # handle while it is alive so Windows PowerShell can read ExitCode
            # after the held-worker process boundary terminates it.
            [void]$scenarioChild.Handle
            $scenarioOwned.Add($scenarioChild)
            $scenarioChildTrace = Trace $scenarioRun $scenarioChildPid
            WaitCondition { @(ReadTrace $scenarioChildTrace | Where-Object { $_.stage -eq 'entered' -and $_.search -eq 'held' }).Count -eq 1 } 'replacement reveal discovery'
            Control $scenarioChildManifest 5
            WaitCondition { @(ReadTrace $scenarioChildTrace | Where-Object { $_.stage -eq 'ui-checkpoint' }).Count -eq 1 } 'responsive replacement'
            Control $scenarioChildManifest 1
            if (!$scenarioChild.WaitForExit(10000)) { throw 'Replacement failed to close while its provider was held.' }
            $scenarioChild.Refresh()
            if ($scenarioChild.ExitCode -ne 0) {
                $scenarioExitValue = $scenarioChild.ExitCode
                $scenarioExitDescription = if ($null -eq $scenarioExitValue) { '<null>' } else { "$scenarioExitValue ($($scenarioExitValue.GetType().FullName))" }
                throw "Replacement failed to close while its provider was held: exit=$scenarioExitDescription; expected 0"
            }
            if (!(ReadTrace $scenarioChildTrace | Where-Object { $_.stage -eq 'owners-cleaned' })) { throw 'Replacement cleanup evidence is missing.' }
        }
        $scenarioResult = [pscustomobject]@{ Passed = $true; Case = $scenarioCase; ParentPid = $scenarioPid; ChildPid = $scenarioChildPid
            Exit = $scenarioExpectedExit; IndependentSearchProgress = $true; IndependentLocalProgress = !$RealSMB.IsPresent; ExplicitCleanup = $true
            CapturedDispatchOnly = $true; ProviderHoldIsSimulated = $true; RealSMBCoverage = $RealSMB.IsPresent; Evidence = $scenarioRun }
        $scenarioResult | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $scenarioRun 'results.json') -Encoding utf8
        $scenarioResults += $scenarioResult
        "Version scenario $scenarioCase passed. Evidence: $scenarioRun"
    } finally {
        # A restart child can be ready even when a later parent assertion fails.
        # Discover only manifests inside this run and verify its exact executable
        # before admitting that process to cleanup.
        $scenarioEvidenceRoot = Join-Path $scenarioRun 'evidence'
        if ([IO.Directory]::Exists($scenarioEvidenceRoot)) {
            foreach ($scenarioControlFile in @(Get-ChildItem -LiteralPath $scenarioEvidenceRoot -Filter manifest.json -Recurse -File)) {
                try {
                    $scenarioAdditional = [IO.File]::ReadAllText($scenarioControlFile.FullName) | ConvertFrom-Json
                    if (@($scenarioManifests | Where-Object { $_.pid -eq $scenarioAdditional.pid }).Count) { continue }
                    $scenarioAdditionalProcess = [Diagnostics.Process]::GetProcessById([int]$scenarioAdditional.pid)
                    if ($scenarioAdditionalProcess.MainModule.FileName -eq $scenarioLauncher) {
                        [void]$scenarioAdditionalProcess.Handle
                        $scenarioManifests.Add($scenarioAdditional)
                        $scenarioOwned.Add($scenarioAdditionalProcess)
                    } else { $scenarioAdditionalProcess.Dispose() }
                } catch { }
            }
        }
        foreach ($scenarioKnown in $scenarioManifests) {
            foreach ($scenarioSearch in $scenarioKnown.searches) {
                foreach ($scenarioAttempt in $scenarioSearch.attempts) {
                    try { $scenarioGate = [Threading.EventWaitHandle]::OpenExisting($scenarioAttempt.release); try { [void]$scenarioGate.Set() } finally { $scenarioGate.Dispose() } } catch { }
                }
            }
        }
        foreach ($scenarioOwnedProcess in $scenarioOwned) {
            try {
                if (!$scenarioOwnedProcess.HasExited) {
                    [void]$scenarioOwnedProcess.CloseMainWindow()
                    if (!$scenarioOwnedProcess.WaitForExit(2000)) { $scenarioOwnedProcess.Kill(); [void]$scenarioOwnedProcess.WaitForExit(2000) }
                }
            } finally { $scenarioOwnedProcess.Dispose() }
        }
    }
}
$scenarioResults | ConvertTo-Json -Depth 4
