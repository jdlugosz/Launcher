#requires -Version 5.1
[CmdletBinding()]
param([string]$LifecycleScript, [string]$PackageDirectory, [string]$Launcher, [string]$OutputDirectory = [IO.Path]::GetTempPath())

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (!$LifecycleScript) { $LifecycleScript = Join-Path $PSScriptRoot '..\tools\installer\Lifecycle.ps1' }
. (Join-Path ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($LifecycleScript))) 'Discovery.ps1')
if (!$Launcher -and $PackageDirectory) { $Launcher = Get-LauncherPackageExecutable $PackageDirectory }
. $LifecycleScript
$script:checks = 0
function Check([bool]$Condition, [string]$Message) { $script:checks++; if (!$Condition) { throw $Message } }
function Reject([scriptblock]$Action, [string]$Reason) {
    $message = ''
    try { & $Action | Out-Null } catch { $message = $_.Exception.Message }
    Check ($message -like ('*' + $Reason + '*')) ('Expected rejection containing "' + $Reason + '"; got "' + $message + '".')
}
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Read-LiveLog([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    $text = [IO.StreamReader]::new($stream)
    try { $text.ReadToEnd() } finally { $text.Dispose() }
}
$fixtureParent = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
$fixtureRoot = Join-Path $fixtureParent ('launcher-lifecycle-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($fixtureRoot)
$source = Join-Path $fixtureRoot 'package'
$user = Join-Path $fixtureRoot 'user'
$machine = Join-Path $fixtureRoot 'machine'
$startup = Join-Path $fixtureRoot 'Startup'
$data = Join-Path $fixtureRoot 'settings'
foreach ($directory in @($source, $user, $machine, $startup, $data)) { [void][IO.Directory]::CreateDirectory($directory) }
$userSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$session = [Diagnostics.Process]::GetCurrentProcess().SessionId
$publisher = 'D' + [char]0x0142 + 'ugosz'
$script:metadata = @{}
function Add-App([string]$Directory, [string]$Component = 'Launcher') {
    $path = Join-Path $Directory ($Component + '.exe')
    [IO.File]::WriteAllText($path, 'Not executable; metadata is injected by this fixture.')
    $script:metadata[$path] = [pscustomobject]@{
        ProductName = $publisher + ' Launcher'; LegalCopyright = 'Copyright (c) 2026 John ' + $publisher
        ProductVersion = '1.2.0'; InternalName = $Component; OriginalFilename = $Component + '.exe'
        IsSpecialBuild = $false; IsPrivateBuild = $false; IsPreRelease = $false; SpecialBuild = ''; PrivateBuild = ''
    }
    $path
}
$sourceExe = Add-App $source
$userExe = Add-App $user
$machineExe = Add-App $machine
$reader = { param($Path) $script:metadata[$Path] }
function Read-Fixture {
    Get-LauncherDiscovery -PackageDirectory $source -LocalDestination $user -MachineDestination $machine `
        -StartupDirectory $startup -MetadataReader $reader -ProcessReader { @() }
}
function Write-Shortcut([string]$Target, [string]$Arguments = '') {
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $null
    try {
        $shortcut = $shell.CreateShortcut((Join-Path $startup 'Launcher.lnk'))
        $shortcut.TargetPath = $Target; $shortcut.WorkingDirectory = [IO.Path]::GetDirectoryName($Target)
        $shortcut.Arguments = $Arguments; $shortcut.Save()
    } finally {
        if ($null -ne $shortcut) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut) }
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell)
    }
}
function Process-Record([int]$Id, [string]$Path, [string]$Component = 'Launcher') {
    [pscustomobject]@{
        ProcessId = $Id; Path = $Path; OwnerSid = $userSid; SessionId = $session
        CreationTimeUtc = '2026-10-21T12:00:00.0000000Z'; Component = $Component
        Identity = [pscustomobject]@{ State = 'Verified'; Component = $Component }
    }
}
$script:pins = @{}
$script:closed = [Collections.Generic.List[int]]::new()
$script:disposed = [Collections.Generic.List[int]]::new()
$script:timeoutId = 0
function Add-Pin($Record) {
    $pin = [pscustomobject]@{
        ProcessId = $Record.ProcessId; Path = $Record.Path; OwnerSid = $Record.OwnerSid
        SessionId = $Record.SessionId; CreationTimeUtc = ([datetime]$Record.CreationTimeUtc).ToUniversalTime()
    }
    $pin | Add-Member -MemberType ScriptMethod -Name Close -Value {
        param($Timeout)
        if ($this.ProcessId -eq $script:timeoutId) { throw 'Fixture graceful-close timeout.' }
        $script:closed.Add($this.ProcessId)
    }
    $pin | Add-Member -MemberType ScriptMethod -Name Dispose -Value { $script:disposed.Add($this.ProcessId) }
    $script:pins[$Record.ProcessId] = $pin
}
$pinReader = { param($Id) $script:pins[$Id] }
$stopOptions = @{
    PackageDirectory = $source; DestinationDirectory = $user; UserSid = $userSid; SessionId = $session
    ProcessPin = $pinReader; MetadataReader = $reader
}
$nativeProcess = $null
$nativeProcess2 = $null
$restoredProcesses = [Collections.Generic.List[object]]::new()
try {
    $untouched = Join-Path $startup 'desktop.ini'
    [IO.File]::WriteAllText($untouched, 'Unrelated Startup entry.')
    $untouchedHash = Hash $untouched
    $discovery = Read-Fixture
    $result = Set-LauncherStartupOption $discovery $user $true
    $link = Join-Path $startup 'Launcher.lnk'
    Check ($result.Action -eq 'Create' -and (Read-LauncherShortcut $link).TargetPath -eq $userExe) 'Create must write the selected permanent entry point.'
    $discovery = Read-Fixture
    $before = Hash $link
    $result = Set-LauncherStartupOption $discovery $user $true
    Check ($result.Action -eq 'Keep' -and (Hash $link) -eq $before) 'Keep must preserve the link byte-for-byte.'
    Write-Shortcut $userExe '--config "preserved fixture.toml"'
    $discovery = Read-Fixture
    $result = Set-LauncherStartupOption $discovery $machine $true
    $written = Read-LauncherShortcut $link
    Check ($result.Action -eq 'Retarget' -and $written.TargetPath -eq $machineExe -and
        $written.WorkingDirectory -eq $machine -and $written.Arguments -eq '--config "preserved fixture.toml"') 'Retarget must retain arguments and set the selected working directory.'
    $discovery = Read-Fixture
    $before = Hash $link
    $result = Set-LauncherStartupOption $discovery $user $false
    Check ($result.Action -eq 'None' -and (Hash $link) -eq $before) 'Unchecked must preserve another installation link.'
    $result = Set-LauncherStartupOption $discovery $machine $false
    Check ($result.Action -eq 'Remove' -and !(Test-Path -LiteralPath $link)) 'Remove must remove only the selected installation link.'
    Write-Shortcut (Join-Path $fixtureRoot 'unrelated.exe')
    $discovery = Read-Fixture
    Reject { Set-LauncherStartupOption $discovery $user $true } 'Review'
    Remove-Item -LiteralPath $link
    $discovery = Read-Fixture
    Write-Shortcut $machineExe
    Reject { Set-LauncherStartupOption $discovery $user $true } 'appeared after review'
    $discovery = Read-Fixture
    Write-Shortcut $machineExe '--config "changed after review.toml"'
    Reject { Set-LauncherStartupOption $discovery $user $true } 'changed after review'
    Check ((Hash $untouched) -eq $untouchedHash) 'Only Launcher.lnk may be managed.'
    Check (@(Get-ChildItem -LiteralPath $startup -Force | Where-Object { $_.Name -like '.Launcher-*' }).Count -eq 0) 'Successful shortcut operations must not leave duplicate launch entries.'

    $sourceProcess = Process-Record 101 $sourceExe
    $targetProcess = Process-Record 102 $userExe
    $otherProcess = Process-Record 103 $machineExe
    Add-Pin $sourceProcess; Add-Pin $targetProcess; Add-Pin $otherProcess
    $secondTarget = Process-Record 105 $userExe
    Add-Pin $secondTarget
    $script:arguments = @{
        101 = '--config "portable.toml"'
        102 = '--config "release.toml"'
        105 = '--config "beta.toml" --test-ui'
    }
    $script:queried = [Collections.Generic.List[int]]::new()
    $argumentsReader = {
        param($Process, $Timeout)
        Check ($script:closed.Count -eq 0) 'All restart information must be collected before any instance closes.'
        $script:queried.Add($Process.ProcessId)
        $script:arguments[$Process.ProcessId]
    }
    $multiple = [pscustomobject]@{ Processes = @($sourceProcess, $targetProcess, $secondTarget, $otherProcess); ProcessEnumerationSucceeded = $true }
    $plan = @(Get-LauncherInstallationRestartPlan -Discovery $multiple @stopOptions -ArgumentsReader $argumentsReader)
    Check ($plan.Count -eq 3 -and ($script:queried -join ',') -eq '101,102,105' -and $script:closed.Count -eq 0) 'Restart collection must retain every affected instance, including two from the same executable.'
    Check ($plan[1].Path -eq $plan[2].Path -and $plan[1].Arguments -ceq $script:arguments[102] -and
        $plan[2].Arguments -ceq $script:arguments[105]) 'Instances of the same executable must retain their distinct configuration arguments.'
    Check (($script:disposed -join ',') -eq '101,102,105') 'Restart collection must release every pinned process without closing it.'
    $script:disposed.Clear(); $script:queried.Clear()
    $failedArgumentsReader = { param($Process, $Timeout) if ($Process.ProcessId -eq 105) { throw 'Fixture query timed out.' }; $script:arguments[$Process.ProcessId] }
    Reject { Get-LauncherInstallationRestartPlan -Discovery $multiple @stopOptions -ArgumentsReader $failedArgumentsReader } 'Fixture query timed out'
    Check ($script:closed.Count -eq 0 -and $script:disposed.Count -eq 3) 'A later query failure must leave all instances running and release every pin.'
    $script:disposed.Clear()
    $newTarget = Process-Record 106 $userExe
    Add-Pin $newTarget
    $multiple.Processes += $newTarget
    Reject { Stop-LauncherInstallationProcesses -Discovery $multiple @stopOptions -RestartInstances $plan } 'started or changed'
    Check ($script:closed.Count -eq 0) 'A new instance after collection must block the entire close operation.'
    $multiple.Processes = @($sourceProcess, $targetProcess, $secondTarget, $otherProcess)
    $targetProcess.CreationTimeUtc = '2026-10-21T12:00:01.0000000Z'
    $script:pins[102].CreationTimeUtc = ([datetime]$targetProcess.CreationTimeUtc).ToUniversalTime()
    Reject { Stop-LauncherInstallationProcesses -Discovery $multiple @stopOptions -RestartInstances $plan } 'started or changed'
    Check ($script:closed.Count -eq 0) 'A reused PID matching fresh discovery must still fail the saved restart identity check before any close.'
    $targetProcess.CreationTimeUtc = '2026-10-21T12:00:00.0000000Z'
    $script:pins[102].CreationTimeUtc = ([datetime]$targetProcess.CreationTimeUtc).ToUniversalTime()
    $script:disposed.Clear()
    $stopped = [Collections.Generic.List[int]]::new()
    $script:timeoutId = 102
    Reject { Stop-LauncherInstallationProcesses -Discovery $multiple @stopOptions -RestartInstances $plan -StoppedProcessIds $stopped } 'graceful-close timeout'
    Check (($stopped -join ',') -eq '101' -and ($script:closed -join ',') -eq '101' -and $script:disposed.Count -eq 3) 'A partial close failure must report exactly the stopped instances and release all pins.'
    $script:timeoutId = 0; $script:closed.Clear(); $script:disposed.Clear()
    $result = Stop-LauncherInstallationProcesses -Discovery $multiple @stopOptions -RestartInstances $plan
    Check (($result.StoppedProcessIds -join ',') -eq '101,102,105') 'A captured multi-instance plan must permit every intended graceful close.'
    $script:restored = [Collections.Generic.List[object]]::new()
    $startFixture = {
        param($Path, $Arguments)
        $script:restored.Add([pscustomobject]@{ Path = $Path; Arguments = $Arguments })
    }
    $recoveryErrors = @(Restore-LauncherInstallationInstances -Instances $plan -StoppedProcessIds @(102, 105, 105) -ProcessStart $startFixture)
    Check ($recoveryErrors.Count -eq 0 -and $script:restored.Count -eq 2 -and
        $script:restored[0].Path -eq $userExe -and $script:restored[0].Arguments -ceq $script:arguments[102] -and
        $script:restored[1].Path -eq $userExe -and $script:restored[1].Arguments -ceq $script:arguments[105]) 'Recovery must restore each stopped instance once with its original distinct arguments and leave nonstopped instances alone.'
    $script:restored.Clear()
    $sourceBytes = [IO.File]::ReadAllBytes($sourceExe)
    try {
        [IO.File]::WriteAllText($sourceExe, 'Different executable bytes after a failed rollback.')
        $recoveryErrors = @(Restore-LauncherInstallationInstances -Instances $plan -StoppedProcessIds @(101, 102, 105) -ProcessStart $startFixture)
        Check ($recoveryErrors.Count -eq 1 -and $recoveryErrors[0] -like '*process 101*original executable has not been restored*' -and
            $script:restored.Count -eq 2 -and @($script:restored | Where-Object { $_.Path -ne $userExe }).Count -eq 0) 'A changed executable hash must block only the affected recovery and retain its explanation while other original instances restart.'
    } finally { [IO.File]::WriteAllBytes($sourceExe, $sourceBytes) }
    $script:restored.Clear()
    $failedStart = {
        param($Path, $Arguments)
        if ($Arguments -ceq $script:arguments[102]) { throw 'Fixture restart could not create the process.' }
        $script:restored.Add([pscustomobject]@{ Path = $Path; Arguments = $Arguments })
    }
    $recoveryErrors = @(Restore-LauncherInstallationInstances -Instances $plan -StoppedProcessIds @(101, 102, 105) -ProcessStart $failedStart)
    Check ($recoveryErrors.Count -eq 1 -and $recoveryErrors[0] -like '*process 102*Fixture restart could not create the process*' -and
        $script:restored.Count -eq 2 -and $script:restored[1].Arguments -ceq $script:arguments[105]) 'A launch failure must be reported per instance while recovery continues for later stopped instances.'
    $script:closed.Clear(); $script:disposed.Clear()
    $discovery = [pscustomobject]@{ Processes = @($sourceProcess, $targetProcess, $otherProcess); ProcessEnumerationSucceeded = $true }
    $result = Stop-LauncherInstallationProcesses -Discovery $discovery @stopOptions
    Check (($script:closed -join ',') -eq '101,102' -and ($result.StoppedProcessIds -join ',') -eq '101,102') 'Graceful close must target source and selected destination only.'
    Check (($script:disposed -join ',') -eq '101,102') 'Every pinned process handle must be released.'
    $script:closed.Clear(); $script:disposed.Clear()
    $script:pins[102].CreationTimeUtc = $script:pins[102].CreationTimeUtc.AddSeconds(1)
    Reject { Stop-LauncherInstallationProcesses -Discovery $discovery @stopOptions } 'changed or cannot be verified'
    Check ($script:closed.Count -eq 0 -and $script:disposed.Count -eq 2) 'PID reuse must block before any close and release all handles.'
    $script:pins[102].CreationTimeUtc = $script:pins[102].CreationTimeUtc.AddSeconds(-1)
    $targetProcess.OwnerSid = 'S-1-5-21-999-998-997-1001'
    Reject { Stop-LauncherInstallationProcesses -Discovery $discovery @stopOptions } 'another user/session'
    $targetProcess.OwnerSid = $userSid
    $targetProcess.SessionId = $session + 1
    Reject { Stop-LauncherInstallationProcesses -Discovery $discovery @stopOptions } 'another user/session'
    $targetProcess.SessionId = $session
    $script:pins[102].OwnerSid = 'S-1-5-21-999-998-997-1001'
    Reject { Stop-LauncherInstallationProcesses -Discovery $discovery @stopOptions } 'changed or cannot be verified'
    $script:pins[102].OwnerSid = $userSid
    $targetProcess.Identity.State = 'Unknown'
    Reject { Stop-LauncherInstallationProcesses -Discovery $discovery @stopOptions } 'Cannot verify'
    $targetProcess.Identity.State = 'Verified'
    $script:metadata[$userExe].ProductName = 'A foreign application'
    Reject { Stop-LauncherInstallationProcesses -Discovery $discovery @stopOptions } 'changed or cannot be verified'
    $script:metadata[$userExe].ProductName = $publisher + ' Launcher'
    $failedPinOptions = $stopOptions.Clone()
    $failedPinOptions.ProcessPin = { throw 'Fixture process access denied.' }
    Reject { Stop-LauncherInstallationProcesses -Discovery $discovery @failedPinOptions } 'process access denied'
    Check ($script:closed.Count -eq 0) 'Changed product identity or process access denial must never close a process.'
    $helper = Process-Record 104 (Add-App $source 'LauncherDrop') 'LauncherDrop'
    $discovery.Processes = @($sourceProcess, $helper)
    Reject { Stop-LauncherInstallationProcesses -Discovery $discovery @stopOptions } 'Preserve any uncopied text'
    Check ($script:closed.Count -eq 0) 'A running LauncherDrop must block before any application closes.'
    foreach ($extractorDirectory in @($source, $user)) {
        $extractor = Process-Record 107 (Add-App $extractorDirectory 'LauncherSvgExtractor') 'LauncherSvgExtractor'
        $discovery.Processes = @($sourceProcess, $targetProcess, $extractor)
        Reject { Get-LauncherInstallationRestartPlan -Discovery $discovery @stopOptions -ArgumentsReader $argumentsReader } 'close SVG Extractor before installing'
        Reject { Stop-LauncherInstallationProcesses -Discovery $discovery @stopOptions } 'close SVG Extractor before installing'
        Reject { Assert-LauncherInstallationStopped $discovery $source $user $userSid $session } 'close SVG Extractor before installing'
        Check ($script:closed.Count -eq 0) 'A running SVG Extractor must block before any application closes.'
    }
    $unrelatedExtractor = Process-Record 108 (Add-App $machine 'LauncherSvgExtractor') 'LauncherSvgExtractor'
    $discovery.Processes = @($unrelatedExtractor)
    Check (@(Get-LauncherInstallationTargets $discovery $source $user $userSid $session).Count -eq 0) 'An SVG Extractor in an unrelated installation blocked the selected destination.'
    $discovery.Processes = @($targetProcess)
    $script:timeoutId = 102
    Reject { Stop-LauncherInstallationProcesses -Discovery $discovery @stopOptions } 'graceful-close timeout'
    $script:timeoutId = 0
    Reject { Assert-LauncherInstallationStopped $discovery $source $user $userSid $session } 'started again'
    $discovery.Processes = @($otherProcess)
    Assert-LauncherInstallationStopped $discovery $source $user $userSid $session
    Check ($true) 'An untargeted installation must not block replacement.'
    $discovery.ProcessEnumerationSucceeded = $false
    Reject { Stop-LauncherInstallationProcesses -Discovery $discovery @stopOptions } 'could not be inspected'

    $script:metadata[$sourceExe].IsPreRelease = $true
    $script:metadata[$userExe].ProductVersion = '1.0.0'
    $config = Join-Path $data 'Launcher.toml'
    $runtime = Join-Path $data 'Launcher.runtime.toml'
    [IO.File]::WriteAllText($config, 'Personal settings preserved exactly.')
    [IO.File]::WriteAllText($runtime, 'Release runtime settings preserved exactly.')
    $snapshot = Backup-LauncherFirstDevelopmentConfiguration $source $user $data -MetadataReader $reader
    Check ([IO.Path]::GetFileName($snapshot) -eq 'Launcher-R1.0.toml' -and (Hash $snapshot) -eq (Hash $config)) 'First development transition must snapshot the release settings.'
    $runtimeSnapshot = Join-Path $data 'Launcher-R1.0.runtime.toml'
    Check ((Hash $runtimeSnapshot) -eq (Hash $runtime)) 'First development transition must snapshot the matching release runtime settings.'
    $before = Hash $snapshot
    $runtimeBefore = Hash $runtimeSnapshot
    [IO.File]::WriteAllText($config, 'Changed later settings.')
    [IO.File]::WriteAllText($runtime, 'Changed later runtime settings.')
    $again = Backup-LauncherFirstDevelopmentConfiguration $source $user $data -MetadataReader $reader
    Check ($again -eq $snapshot -and (Hash $snapshot) -eq $before) 'An existing release snapshot must never be refreshed.'
    Check ((Hash $runtimeSnapshot) -eq $runtimeBefore) 'An existing release runtime snapshot must never be refreshed.'
    $script:metadata[$userExe].ProductVersion = '1.4.0'
    [IO.File]::Delete($runtime)
    [void](Backup-LauncherFirstDevelopmentConfiguration $source $user $data -MetadataReader $reader)
    [IO.File]::WriteAllText($runtime, 'Runtime first created after transition.')
    [void](Backup-LauncherFirstDevelopmentConfiguration $source $user $data -MetadataReader $reader)
    Check (!(Test-Path -LiteralPath (Join-Path $data 'Launcher-R1.4.runtime.toml'))) 'Later runtime settings invented a snapshot for a release that had none.'
    $script:metadata[$userExe].ProductVersion = '1.6.0'
    [IO.File]::Delete($runtime)
    [void][IO.Directory]::CreateDirectory($runtime)
    Reject { Backup-LauncherFirstDevelopmentConfiguration $source $user $data -MetadataReader $reader } 'must be regular files'
    Check (!(Test-Path -LiteralPath (Join-Path $data 'Launcher-R1.6.toml'))) 'A runtime snapshot failure left a partial main configuration snapshot.'
    [IO.Directory]::Delete($runtime)
    $script:metadata[$userExe].ProductVersion = '1.0.0'
    $script:metadata[$userExe].ProductName = 'A foreign application'
    $script:metadata[$userExe].ProductVersion = '2.0.0'
    Reject { Backup-LauncherFirstDevelopmentConfiguration $source $user $data -MetadataReader $reader } 'release identity changed'
    Check (!(Test-Path -LiteralPath (Join-Path $data 'Launcher-R2.0.toml'))) 'A foreign executable must not create a release configuration snapshot.'
    $script:metadata[$userExe].ProductName = 'Launcher'
    $script:metadata[$userExe].LegalCopyright = ''
    $script:metadata[$userExe].ProductVersion = '1.0.0'
    Check ((Backup-LauncherFirstDevelopmentConfiguration $source $user $data -MetadataReader $reader) -eq $snapshot) 'The legacy 1.0 release identity must remain eligible for its preserved snapshot.'
    $script:metadata[$userExe].ProductName = $publisher + ' Launcher'
    $script:metadata[$userExe].LegalCopyright = 'Copyright (c) 2026 John ' + $publisher
    $script:metadata[$userExe].IsPreRelease = $true
    Check ($null -eq (Backup-LauncherFirstDevelopmentConfiguration $source $user $data -MetadataReader $reader)) 'A development-to-development update must not create a release snapshot.'
    $script:metadata[$userExe].IsPreRelease = $false
    Remove-Item -LiteralPath $config
    Check ($null -eq (Backup-LauncherFirstDevelopmentConfiguration $source $user $data -MetadataReader $reader)) 'Absent configuration must not invent a release snapshot.'
    $script:metadata[$sourceExe].IsPreRelease = $false

    if ($Launcher) {
        $nativeDirectory = Join-Path $fixtureRoot 'native'
        [void][IO.Directory]::CreateDirectory($nativeDirectory)
        $nativeExe = Join-Path $nativeDirectory 'Launcher.exe'
        [IO.File]::Copy([IO.Path]::GetFullPath($Launcher), $nativeExe, $false)
        $nativeConfig = Join-Path $nativeDirectory 'isolated.toml'
        [IO.File]::WriteAllText($nativeConfig, @'
schema_version = 1
[dock]
edge = "top"
display = "primary"
auto_hide = true
[grid]
columns = 1
icon_size_dip = 24
[[tabs]]
name = "Installer lifecycle fixture"
color = "#BED878"
items = [{ label = "Fixture", icon = { stock = "folder" } }]
[logging]
file = "native.log"
'@, [Text.UTF8Encoding]::new($false))
        $nativeProcess = Start-Process -FilePath $nativeExe -ArgumentList @('--test-ui', '--config', ('"' + $nativeConfig + '"')) -WindowStyle Hidden -PassThru
        $nativeConfig2 = Join-Path $nativeDirectory 'beta.toml'
        [IO.File]::WriteAllText($nativeConfig2, [IO.File]::ReadAllText($nativeConfig).Replace('native.log', 'native-beta.log'), [Text.UTF8Encoding]::new($false))
        $nativeProcess2 = Start-Process -FilePath $nativeExe -ArgumentList @(('"' + $nativeConfig2 + '"'), '--test-ui') -WindowStyle Hidden -PassThru
        $nativeLog = Join-Path $nativeDirectory 'native.log'
        $nativeLog2 = Join-Path $nativeDirectory 'native-beta.log'
        $deadline = [Diagnostics.Stopwatch]::StartNew()
        while ($deadline.ElapsedMilliseconds -lt 10000) {
            if ((Test-Path -LiteralPath $nativeLog) -and (Read-LiveLog $nativeLog).Contains('Drawing initialized') -and
                (Test-Path -LiteralPath $nativeLog2) -and (Read-LiveLog $nativeLog2).Contains('Drawing initialized')) { break }
            $nativeProcess.Refresh()
            $nativeProcess2.Refresh()
            if ($nativeProcess.HasExited -or $nativeProcess2.HasExited) { throw 'An owned native fixture exited before initializing.' }
            Start-Sleep -Milliseconds 50
        }
        Check ((Test-Path -LiteralPath $nativeLog) -and (Read-LiveLog $nativeLog).Contains('Drawing initialized')) 'The owned native fixture must initialize before shutdown.'
        Check ((Test-Path -LiteralPath $nativeLog2) -and (Read-LiveLog $nativeLog2).Contains('Drawing initialized')) 'The second instance must initialize with its own configuration and log.'
        $nativeId = $nativeProcess.Id
        $nativeDiscovery = Get-LauncherDiscovery -PackageDirectory $nativeDirectory -LocalDestination $user -MachineDestination $machine `
            -StartupDirectory $startup
        $nativeRecords = @($nativeDiscovery.Processes | Where-Object { $_.ProcessId -eq $nativeId })
        Check ($nativeDiscovery.ProcessEnumerationSucceeded -and $nativeRecords.Count -eq 1) 'Native discovery must find its owned fixture without an injected process reader.'
        $nativeRecord = $nativeRecords[0]
        Check ($nativeRecord.Path -ieq $nativeExe -and $nativeRecord.Identity.State -eq 'Verified' -and
            $nativeRecord.OwnerSid -eq $userSid -and $nativeRecord.SessionId -eq $session -and
            ![string]::IsNullOrWhiteSpace($nativeRecord.Owner) -and
            [Math]::Abs((([datetime]$nativeRecord.CreationTimeUtc).ToUniversalTime() - $nativeProcess.StartTime.ToUniversalTime()).Ticks) -le 10) 'Native discovery must retain the exact path, user, session and creation time needed for safe graceful close.'
        $nativePlan = @(Get-LauncherInstallationRestartPlan $nativeDiscovery $nativeDirectory $nativeDirectory $userSid $session)
        $expectedArguments = @(('--config "' + $nativeConfig + '" --test-ui'), ('--config "' + $nativeConfig2 + '" --test-ui'))
        Check ($nativePlan.Count -eq 2 -and @($nativePlan | Where-Object { $_.Path -ieq $nativeExe }).Count -eq 2 -and
            (($nativePlan.Arguments | Sort-Object) -join '|') -ceq (($expectedArguments | Sort-Object) -join '|')) 'The COPYDATA replies must preserve both distinct configurations of the same executable.'
        Check (@($nativePlan | Where-Object { $_.Arguments.Contains('--restart-') }).Count -eq 0) 'Saved arguments must omit private restart-handoff handles.'
        $result = Stop-LauncherInstallationProcesses $nativeDiscovery $nativeDirectory $nativeDirectory $userSid $session -RestartInstances $nativePlan
        Check ($nativeProcess.WaitForExit(1000) -and $result.StoppedProcessIds -contains $nativeId) 'Native graceful close must wait for the actual owned process to exit.'
        Check ($nativeProcess2.WaitForExit(1000) -and $result.StoppedProcessIds -contains $nativeProcess2.Id -and
            @($result.StoppedProcessIds).Count -eq 2) 'Both instances from the same executable must close and be reported individually.'
        Check ([IO.File]::ReadAllText($nativeLog).Contains('Graceful close requested.')) 'Native evidence must show normal Launcher shutdown.'
        Check ([IO.File]::ReadAllText($nativeLog2).Contains('Graceful close requested.')) 'The second configuration must retain its own graceful-shutdown log.'
        $afterClose = Get-LauncherDiscovery -PackageDirectory $nativeDirectory -LocalDestination $user -MachineDestination $machine `
            -StartupDirectory $startup
        Check ($afterClose.ProcessEnumerationSucceeded -and
            @($afterClose.Processes | Where-Object { $_.ProcessId -eq $nativeId }).Count -eq 0) 'An exited fixture must disappear from native discovery rather than remain an unknown running process.'
        foreach ($instance in $nativePlan) { Start-LauncherInstallationInstance $nativeExe $instance.Arguments }
        $deadline.Restart()
        $restored = @()
        while ($deadline.ElapsedMilliseconds -lt 10000) {
            $found = Get-LauncherDiscovery -PackageDirectory $nativeDirectory -LocalDestination $user -MachineDestination $machine -StartupDirectory $startup
            $restored = @($found.Processes | Where-Object { $_.Path -ieq $nativeExe })
            if ($restored.Count -eq 2) { break }
            Start-Sleep -Milliseconds 50
        }
        foreach ($instance in $restored) {
            $owned = [Diagnostics.Process]::GetProcessById($instance.ProcessId)
            $null = $owned.Handle
            $restoredProcesses.Add($owned)
        }
        Check ($restored.Count -eq 2) 'Relaunch must restore two instances rather than collapse them into one.'
        $restoredPlan = @()
        $deadline.Restart()
        while ($deadline.ElapsedMilliseconds -lt 10000) {
            try {
                $restoredPlan = @(Get-LauncherInstallationRestartPlan $found $nativeDirectory $nativeDirectory $userSid $session)
                break
            } catch { Start-Sleep -Milliseconds 50 }
        }
        Check ($restoredPlan.Count -eq 2 -and (($restoredPlan.Arguments | Sort-Object) -join '|') -ceq
            (($expectedArguments | Sort-Object) -join '|')) 'Each relaunched instance must report the same persistent options as before shutdown.'
        $result = Stop-LauncherInstallationProcesses $found $nativeDirectory $nativeDirectory $userSid $session -RestartInstances $restoredPlan
        Check (@($result.StoppedProcessIds).Count -eq 2) 'Restored fixture instances must shut down through the same verified lifecycle.'
    }
    Write-Output ('Installer lifecycle passed: {0} checks; native graceful close: {1}.' -f $script:checks, [bool]$Launcher)
} finally {
    if ($null -ne $nativeProcess) {
        # Only the fixture child created above can be terminated on test failure.
        if (!$nativeProcess.HasExited) { $nativeProcess.Kill(); [void]$nativeProcess.WaitForExit(5000) }
        $nativeProcess.Dispose()
    }
    if ($null -ne $nativeProcess2) {
        if (!$nativeProcess2.HasExited) { $nativeProcess2.Kill(); [void]$nativeProcess2.WaitForExit(5000) }
        $nativeProcess2.Dispose()
    }
    foreach ($owned in $restoredProcesses) {
        if (!$owned.HasExited) { $owned.Kill(); [void]$owned.WaitForExit(5000) }
        $owned.Dispose()
    }
    $resolvedRoot = [IO.Path]::GetFullPath($fixtureRoot)
    if (!$resolvedRoot.StartsWith($fixtureParent + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing cleanup outside the fixture parent.' }
    Remove-Item -LiteralPath $resolvedRoot -Recurse -Force -ErrorAction Stop
}
