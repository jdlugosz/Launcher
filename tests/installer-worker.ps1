[CmdletBinding()]
param([string]$PackageDirectory, [string]$WorkerScript)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (!$WorkerScript) { $WorkerScript = Join-Path $PSScriptRoot '..\tools\installer\Worker.ps1' }
. (Join-Path ([IO.Path]::GetDirectoryName($WorkerScript)) 'Deployment.ps1')
. $WorkerScript
$script:checks = 0
function Check([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
    $script:checks++
}
function Must-Throw([scriptblock]$Action, [string]$Message) {
    $caught = $false
    try { $null = & $Action } catch { $caught = $true }
    Check $caught $Message
}
function Clone($Object) { return $Object | ConvertTo-Json -Depth 16 | ConvertFrom-Json }
function Read-WorkerLiveLog([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    $reader = [IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}

$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$fixtureName = 'LauncherInstallerWorkerTests-' + [Guid]::NewGuid().ToString('N')
$fixture = Join-Path $temporaryRoot $fixtureName
[void][IO.Directory]::CreateDirectory($fixture)
$cimOverrides = @{}
$script:cimQueries = 0
try {
    $script:context = [pscustomobject]@{
        UserSid = 'S-1-5-21-111-222-333-1001'; SessionId = 7
        LocalDestination = Join-Path $fixture 'Local Programs\Vendor\Launcher'
        MachineDestination = Join-Path $fixture 'Program Files\Vendor\Launcher'
    }
    $script:package = Join-Path $fixture 'package'
    $script:payload = [pscustomobject]@{
        PackageDirectory = $script:package; IsRelease = $false; ReleaseLabel = $null
        Files = @(
            [pscustomobject]@{ RelativePath = 'Launcher.exe'; SourcePath = Join-Path $script:package 'Launcher.exe'; Sha256 = 'A' * 64; PreserveExisting = $false },
            [pscustomobject]@{ RelativePath = 'examples\basic_colors.toml'; SourcePath = Join-Path $script:package 'examples\basic_colors.toml'; Sha256 = 'B' * 64; PreserveExisting = $true }
        )
    }
    function New-Request([string]$Scope = 'CurrentUser', [string]$Action = 'Install') {
        return [pscustomobject]@{
            PackageDirectory = $script:package
            DestinationDirectory = if ($Scope -eq 'AllUsers') { $script:context.MachineDestination } else { $script:context.LocalDestination }
            Scope = $Scope; Action = $Action
            LocalDestination = $script:context.LocalDestination; MachineDestination = $script:context.MachineDestination
            UserSid = $script:context.UserSid; SessionId = $script:context.SessionId
            StartupDirectory = Join-Path $fixture 'Startup'; Payload = Clone $script:payload; RestartInstances = @()
        }
    }
    function New-Discovery([string[]]$InstalledScopes = @()) {
        $locations = foreach ($scope in @('AllUsers', 'CurrentUser')) {
            $installed = $InstalledScopes -contains $scope
            [pscustomobject]@{
                Scope = $scope
                Directory = if ($scope -eq 'AllUsers') { $script:context.MachineDestination } else { $script:context.LocalDestination }
                State = if ($installed) { 'Verified' } else { 'Missing' }
                IsInstalled = $installed; CanUpgrade = $installed
            }
        }
        return [pscustomobject]@{ Locations = @($locations); Installations = @($locations | Where-Object IsInstalled) }
    }
    function Run-WorkerFixture {
        param(
            $Request = (New-Request), $Discovery = (New-Discovery),
            [scriptblock]$Deploy = {
                param($Package, $Destination, $BeforeCommit, $Progress, $AfterActivation)
                & $Progress 40 'Staged and verified.'
                & $BeforeCommit
                & $AfterActivation (Join-Path $Destination 'Launcher.exe')
                return [pscustomobject]@{ ActiveExecutable = Join-Path $Destination 'Launcher.exe'; Sha256 = 'A' * 64 }
            },
            [scriptblock]$PayloadReader,
            [scriptblock]$AssertStopped,
            [scriptblock]$StopProcesses,
            [scriptblock]$RegistrationPrepare,
            [scriptblock]$RegistrationWrite,
            [scriptblock]$RecoveryShortcut
        )
        $events = [Collections.Generic.List[string]]::new()
        $reports = [Collections.Generic.List[object]]::new()
        $freshPayload = Clone $script:payload
        if ($null -eq $PayloadReader) { $PayloadReader = { param($Directory) $freshPayload }.GetNewClosure() }
        $reader = { param($Plan) $events.Add('discover'); return $Discovery }.GetNewClosure()
        if ($null -eq $StopProcesses) {
            $StopProcesses = {
                param($Found, $Plan, $StoppedIds)
                $events.Add('stop')
                foreach ($instance in @($Plan.RestartInstances)) { $StoppedIds.Add([int]$instance.ProcessId) }
            }.GetNewClosure()
        }
        if ($null -eq $AssertStopped) { $AssertStopped = { param($Found, $Plan) $events.Add('assert-stopped') }.GetNewClosure() }
        $writer = { param($Path, $Result) $reports.Add($Result) }.GetNewClosure()
        if (!$RegistrationPrepare) { $RegistrationPrepare = { param($Plan, $Payload, $Context) $events.Add('registration-preflight'); [pscustomobject]@{ Applied = $false; Plan = $Plan } }.GetNewClosure() }
        if (!$RegistrationWrite) { $RegistrationWrite = { param($Transaction) $events.Add('register'); $Transaction.Applied = $true }.GetNewClosure() }
        $registrationRestore = { param($Transaction) if ($Transaction.Applied) { $events.Add('registration-restore'); $Transaction.Applied = $false } }.GetNewClosure()
        $outcome = Invoke-LauncherInstallWorker -Request $Request -ResultPath (Join-Path $fixture 'unused.json') `
            -Context $script:context -PayloadReader $PayloadReader -DiscoveryReader $reader -Deploy $Deploy `
            -StopProcesses $StopProcesses -AssertStopped $AssertStopped -ResultWriter $writer `
            -RegistrationPrepare $RegistrationPrepare -RegistrationWrite $RegistrationWrite -RegistrationRestore $registrationRestore `
            -RecoveryShortcut $RecoveryShortcut
        return [pscustomobject]@{ Result = $outcome; Events = $events; Reports = $reports }
    }

    $plan = New-Request
    Assert-LauncherWorkerRequest -Request $plan -Context $script:context
    Assert-LauncherWorkerPayload -Expected $plan.Payload -Actual $script:payload
    Check $true 'A valid personal installation request must validate.'
    $missingRestartPlan = New-Request
    $missingRestartPlan.PSObject.Properties.Remove('RestartInstances')
    Must-Throw { Assert-LauncherWorkerRequest -Request $missingRestartPlan -Context $script:context } 'A request without reviewed restart information must not reach shutdown.'
    function New-RestartInstance([int]$Id, [string]$Arguments) {
        [pscustomobject]@{
            ProcessId = $Id; Path = Join-Path $script:context.LocalDestination 'Launcher.exe'
            OwnerSid = $script:context.UserSid; SessionId = $script:context.SessionId
            CreationTimeUtc = '2026-10-21T12:00:00.0000000Z'; Arguments = $Arguments
            ExecutableHash = 'C' * 64
        }
    }
    $multiple = New-Request
    $multiple.RestartInstances = @((New-RestartInstance 201 '--config "release.toml"'), (New-RestartInstance 202 '--config "beta.toml" --test-ui'))
    Assert-LauncherWorkerRequest -Request $multiple -Context $script:context
    foreach ($change in @(
        { param($p) $p.RestartInstances[1].ProcessId = 201 },
        { param($p) $p.RestartInstances[0].OwnerSid = 'S-1-5-21-111-222-333-1002' },
        { param($p) $p.RestartInstances[0].SessionId = 8 },
        { param($p) $p.RestartInstances[0].Path = Join-Path $fixture 'another-installation\Launcher.exe' },
        { param($p) $p.RestartInstances[0].Arguments = 'bad' + [char]0 + 'argument' },
        { param($p) $p.RestartInstances[0].ExecutableHash = 'invalid hash' },
        { param($p) $p.RestartInstances[0].CreationTimeUtc = 'invalid time' }
    )) {
        $invalid = Clone $multiple
        & $change $invalid
        Must-Throw { Assert-LauncherWorkerRequest -Request $invalid -Context $script:context } 'Invalid saved restart identities or argument strings must fail before shutdown.'
    }
    foreach ($change in @(
        { param($p) $p.Scope = 'Other' },
        { param($p) $p.Action = 'Remove' },
        { param($p) $p.SessionId = 8 },
        { param($p) $p.UserSid = 'S-1-5-21-111-222-333-1002' },
        { param($p) $p.DestinationDirectory = Join-Path $fixture 'arbitrary-target' },
        { param($p) $p.LocalDestination = $p.DestinationDirectory = Join-Path $fixture 'another-profile' },
        { param($p) $p.MachineDestination = Join-Path $fixture 'arbitrary-program-files' },
        { param($p) $p.PackageDirectory = $p.DestinationDirectory }
    )) {
        $invalid = New-Request
        & $change $invalid
        Must-Throw { Assert-LauncherWorkerRequest -Request $invalid -Context $script:context } 'An invalid scope, user, session or destination was accepted.'
    }
    $plan = New-Request 'AllUsers'
    $otherAdministrator = Clone $script:context
    $otherAdministrator.UserSid = 'S-1-5-21-111-222-333-1002'
    $otherAdministrator.LocalDestination = Join-Path $fixture 'Administrator\Programs\Launcher'
    Assert-LauncherWorkerRequest -Request $plan -Context $otherAdministrator
    Check $true 'All-users elevation with another administrator must keep the original user paths.'
    $invalid = Clone $plan
    $invalid.DestinationDirectory = $invalid.LocalDestination
    Must-Throw { Assert-LauncherWorkerRequest -Request $invalid -Context $otherAdministrator } 'An elevated worker accepted a personal destination as all-users.'

    foreach ($change in @(
        { param($p) $p.Files[0].Sha256 = 'C' * 64 },
        { param($p) $p.Files[0].SourcePath = Join-Path $fixture 'outside.exe' },
        { param($p) $p.Files[0].RelativePath = '..\outside.exe' },
        { param($p) $p.Files[0].PreserveExisting = $true },
        { param($p) $p.Files[0].PreserveExisting = 'false' },
        { param($p) $p.Files += $p.Files[0] },
        { param($p) $p.Files = @($p.Files[0]) },
        { param($p) $p.PackageDirectory = Join-Path $fixture 'changed-package' }
    )) {
        $invalid = Clone $script:payload
        & $change $invalid
        Must-Throw { Assert-LauncherWorkerPayload -Expected $invalid -Actual $script:payload } 'A changed or unsafe reviewed package was accepted.'
    }

    $result = Run-WorkerFixture
    Check ($result.Result.Status -ceq 'Succeeded' -and $result.Result.Progress -eq 100) 'A valid worker transaction must succeed.'
    Check (($result.Events -join ',') -ceq 'discover,registration-preflight,discover,stop,discover,assert-stopped,register') 'Registry preflight and activation must bracket the file/process transaction.'
    Check ($result.Reports[0].Status -ceq 'Running' -and $result.Reports[1].Progress -eq 40 -and
        $result.Reports[$result.Reports.Count - 1].Status -ceq 'Succeeded') 'Worker progress must end with one terminal report.'
    Check ($result.Result.Deployment.ActiveExecutable -ieq (Join-Path $script:context.LocalDestination 'Launcher.exe')) 'The worker must return its deployment receipt.'
    Check (@($result.Result.StoppedProcessIds).Count -eq 0) 'Fresh installation must report an empty stopped-instance list.'
    $shortcutState = [pscustomobject]@{ Calls = 0; Scope = $null; Destination = $null }
    $mockShortcut = {
        param($Plan, $Deployment)
        $shortcutState.Calls++
        $shortcutState.Scope = $Plan.Scope
        $shortcutState.Destination = $Deployment.ActiveExecutable
        throw 'Isolated recovery shortcut collision.'
    }.GetNewClosure()
    $result = Run-WorkerFixture -Request (New-Request AllUsers) -RecoveryShortcut $mockShortcut
    Check ($result.Result.Status -ceq 'Succeeded' -and $result.Result.RecoveryShortcutWarning.Contains('Isolated recovery shortcut collision.') -and
        $shortcutState.Calls -eq 1 -and $shortcutState.Scope -ceq 'AllUsers' -and
        $shortcutState.Destination -ceq (Join-Path $script:context.MachineDestination 'Launcher.exe') -and
        !$result.Events.Contains('registration-restore')) 'A shortcut failure must warn without undoing the committed scoped install.'
    $result = Run-WorkerFixture -Deploy { throw 'Isolated deployment failure.' } -RecoveryShortcut $mockShortcut
    Check ($result.Result.Status -ceq 'Failed' -and $shortcutState.Calls -eq 1) 'A failed deployment must not touch the recovery shortcut.'
    $transactionId = [Guid]::NewGuid()
    $transactionRequest = New-Request
    $transactionRequest | Add-Member -NotePropertyName UpdaterTransactionId -NotePropertyValue $transactionId.ToString('N')
    $observeTransaction = {
        param($Package, $Destination, $BeforeCommit, $Progress, $AfterActivation, $Scope, $UserSid, $UpdaterTransactionId)
        return [pscustomobject]@{ UpdaterTransactionId = $UpdaterTransactionId; Scope = $Scope }
    }
    $forwarded = Run-WorkerFixture -Request (Clone $transactionRequest) -Deploy $observeTransaction
    Check ($forwarded.Result.Status -ceq 'Succeeded' -and $forwarded.Result.Deployment.Scope -ceq $transactionRequest.Scope -and
        $forwarded.Result.Deployment.UpdaterTransactionId -eq $transactionId) 'The worker did not pass the parsed transaction ID without changing scope.'
    $transactionRequest.UpdaterTransactionId += [char]10
    $ignored = Run-WorkerFixture -Request (Clone $transactionRequest) -Deploy $observeTransaction
    Check ($ignored.Result.Status -ceq 'Succeeded' -and $ignored.Result.Deployment.UpdaterTransactionId -eq [Guid]::Empty) 'Malformed optional transaction metadata affected the worker.'
    $registryDenied = {
        param($Plan, $Payload, $Context)
        $failure = [UnauthorizedAccessException]::new('HKLM needs administrator approval even when the program folder is writable.')
        $failure.Data['LauncherPermissionDenied'] = $true
        throw $failure
    }
    $deniedRegistration = Run-WorkerFixture -RegistrationPrepare $registryDenied
    Check ($deniedRegistration.Result.Status -eq 'PermissionDenied' -and !$deniedRegistration.Events.Contains('stop')) 'Registration permissions were not checked before stopping programs or replacing files.'
    $lateRegistryFailure = { param($Transaction) $Transaction.Applied = $true; throw 'Owned registration publication failed.' }
    $failedRegistration = Run-WorkerFixture -RegistrationWrite $lateRegistryFailure
    Check ($failedRegistration.Result.Status -eq 'Failed' -and $failedRegistration.Events.Contains('registration-restore')) 'Failed registration did not restore its snapshot after payload activation failed.'
    $result = Run-WorkerFixture -Request $multiple
    Check ($result.Result.Status -ceq 'Succeeded' -and ($result.Result.StoppedProcessIds -join ',') -eq '201,202') 'The worker must report both distinct instances stopped from the same executable.'
    $partialStop = {
        param($Found, $Plan, $StoppedIds)
        $StoppedIds.Add([int]$Plan.RestartInstances[0].ProcessId)
        throw 'Fixture second-instance close timed out.'
    }
    $result = Run-WorkerFixture -Request $multiple -StopProcesses $partialStop
    Check ($result.Result.Status -ceq 'Failed' -and ($result.Result.StoppedProcessIds -join ',') -eq '201' -and
        ($result.Reports[$result.Reports.Count - 1].StoppedProcessIds -join ',') -eq '201') 'A failed close must retain the exact partial shutdown receipt for restoring those instances.'
    foreach ($scope in @('AllUsers', 'CurrentUser')) {
        $request = New-Request $scope 'Upgrade'
        $result = Run-WorkerFixture -Request $request -Discovery (New-Discovery @($scope))
        Check ($result.Result.Status -ceq 'Succeeded') "A single $scope installation must upgrade in place."
        $request.Action = 'Install'
        $result = Run-WorkerFixture -Request $request -Discovery (New-Discovery @($scope))
        Check ($result.Result.Status -ceq 'Failed' -and !$result.Events.Contains('stop')) 'An existing installation must prevent a fresh-install action.'
    }
    $both = New-Discovery @('AllUsers', 'CurrentUser')
    Check ((Run-WorkerFixture -Request (New-Request 'CurrentUser' 'Upgrade') -Discovery $both).Result.Status -ceq 'Succeeded') 'Both installations must select the personal upgrade.'
    $result = Run-WorkerFixture -Request (New-Request 'AllUsers' 'Upgrade') -Discovery $both
    Check ($result.Result.Status -ceq 'Failed' -and !$result.Events.Contains('stop')) 'Both installations must leave the machine-wide copy alone.'
    $blocked = New-Discovery
    $blocked.Locations[1].State = 'Foreign'
    Check ((Run-WorkerFixture -Discovery $blocked).Result.Status -ceq 'Failed') 'Unverified destination contents must require review.'

    $markedDenied = {
        param($Package, $Destination, $BeforeCommit, $Progress)
        $denied = [UnauthorizedAccessException]::new('Destination is protected.')
        $denied.Data['LauncherPermissionDenied'] = $true
        throw [IO.IOException]::new('Preparing destination failed.', $denied)
    }
    $result = Run-WorkerFixture -Deploy $markedDenied
    Check ($result.Result.Status -ceq 'PermissionDenied' -and !$result.Events.Contains('stop')) 'Only a marked destination permission failure before commit may request elevation.'
    $sourceDenied = { param($Directory) throw [UnauthorizedAccessException]::new('Source package is unreadable.') }
    Check ((Run-WorkerFixture -PayloadReader $sourceDenied).Result.Status -ceq 'Failed') 'A package read failure must not trigger a destination fallback.'
    $unmarkedDenied = { param($Package, $Destination, $BeforeCommit, $Progress) throw [UnauthorizedAccessException]::new('Source staging failed.') }
    Check ((Run-WorkerFixture -Deploy $unmarkedDenied).Result.Status -ceq 'Failed') 'An unmarked staging permission failure must not trigger fallback.'
    $lateDenied = {
        param($Package, $Destination, $BeforeCommit, $Progress)
        & $BeforeCommit
        $denied = [UnauthorizedAccessException]::new('A later commit write failed.')
        $denied.Data['LauncherPermissionDenied'] = $true
        throw $denied
    }
    Check ((Run-WorkerFixture -Deploy $lateDenied).Result.Status -ceq 'Failed') 'A failure after application shutdown must not trigger a new installation elsewhere.'
    $diskFull = { param($Package, $Destination, $BeforeCommit, $Progress) throw [ComponentModel.Win32Exception]::new(112) }
    Check ((Run-WorkerFixture -Deploy $diskFull).Result.Status -ceq 'Failed') 'Disk-full errors must retain their actual failure.'
    $newProcess = { param($Found, $Plan) throw 'A targeted LauncherDrop started after shutdown; preserve its text and close it.' }
    Check ((Run-WorkerFixture -AssertStopped $newProcess).Result.Status -ceq 'Failed') 'A newly running target must block commit.'
    $changedPayload = New-Request
    $changedPayload.Payload.Files[0].Sha256 = 'F' * 64
    $result = Run-WorkerFixture -Request $changedPayload
    Check ($result.Result.Status -ceq 'Failed' -and !$result.Events.Count) 'Manifest changes must fail before process discovery or shutdown.'

    foreach ($code in @(5, 740, 1223, 1314)) {
        $wrapped = [IO.IOException]::new('Start-Process wrapped a Windows failure.', [ComponentModel.Win32Exception]::new($code))
        Check (Test-LauncherPermissionFailure $wrapped) "Wrapped Windows error $code must be recognized by the wizard."
    }
    Check (!(Test-LauncherPermissionFailure ([ComponentModel.Win32Exception]::new(112)))) 'Disk-full errors must not look like unavailable elevation.'
    Check (Test-LauncherCanFallBackToPersonal (New-Request 'AllUsers' 'Install')) 'Fresh machine installations may fall back to a reviewed personal destination.'
    Check (!(Test-LauncherCanFallBackToPersonal (New-Request 'AllUsers' 'Upgrade'))) 'A machine upgrade must never relocate to a personal installation.'
    Check (!(Test-LauncherCanFallBackToPersonal (New-Request 'CurrentUser' 'Install'))) 'A personal installation cannot fall back to another scope.'

    $transport = Join-Path $fixture ('.launcher-setup-' + [Guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($transport)
    $requestPath = Join-Path $transport 'request.json'
    $resultPath = Join-Path $transport 'result.json'
    [IO.File]::WriteAllText($requestPath, '{}')
    Assert-LauncherWorkerTransport $requestPath $resultPath
    Write-LauncherWorkerResult -ResultPath $resultPath -Result ([pscustomobject]@{ Status = 'Running'; Message = 'Preparing'; Progress = 10; Deployment = $null })
    Write-LauncherWorkerResult -ResultPath $resultPath -Result ([pscustomobject]@{ Status = 'Succeeded'; Message = 'Complete'; Progress = 100; Deployment = @{ ActiveExecutable = 'fixture.exe' } })
    $receipt = [IO.File]::ReadAllText($resultPath) | ConvertFrom-Json
    Check ($receipt.Status -ceq 'Succeeded' -and $receipt.Progress -eq 100 -and $receipt.Deployment.ActiveExecutable -ceq 'fixture.exe') 'Atomic result replacement lost the terminal receipt.'
    Check (@(Get-ChildItem -LiteralPath $transport -Filter '.result-*').Count -eq 0) 'Result staging files were not cleaned up.'
    Must-Throw { Assert-LauncherWorkerTransport $requestPath (Join-Path $fixture 'result.json') } 'A result outside the dedicated request directory was accepted.'
    Must-Throw { Assert-LauncherWorkerTransport $requestPath (Join-Path $transport 'arbitrary.json') } 'An arbitrary elevated result filename was accepted.'

    if ($PackageDirectory) {
        # Windows Sandbox can deny CIM/WMI. Exercise the real discovery and
        # installation paths with both CIM entry points deliberately unavailable.
        foreach ($name in @('Get-CimInstance', 'Invoke-CimMethod')) {
            $prior = Get-Item -LiteralPath ('Function:\' + $name) -ErrorAction SilentlyContinue
            $cimOverrides[$name] = if ($null -ne $prior) { $prior.ScriptBlock } else { $null }
            Set-Item -LiteralPath ('Function:\' + $name) -Value {
                $script:cimQueries++
                throw 'Fixture: CIM/WMI is unavailable.'
            }
        }
        $moduleDirectory = Split-Path -Parent ([IO.Path]::GetFullPath($WorkerScript))
        . (Join-Path $moduleDirectory 'Discovery.ps1')
        . (Join-Path $moduleDirectory 'Deployment.ps1')
        . (Join-Path $moduleDirectory 'Lifecycle.ps1')
        $realPayload = Get-LauncherPayload -PackageDirectory $PackageDirectory
        $liveContext = Get-LauncherWorkerContext
        $script:context.UserSid = $liveContext.UserSid
        $script:context.SessionId = $liveContext.SessionId
        $realRequest = New-Request
        $realRequest.PackageDirectory = $realPayload.PackageDirectory
        $realRequest.Payload = $realPayload
        $registryState = [pscustomobject]@{ Snapshot = [pscustomobject]@{ Exists = $false; Values = [ordered]@{} }; Writes = 0 }
        $registryStore = {
            param($Operation, $Plan, $Snapshot)
            switch ($Operation) {
                'Read' { $registryState.Snapshot }
                'Write' {
                    $values = [ordered]@{}
                    foreach ($key in $Plan.Values.Keys) { $values[$key] = [pscustomobject]@{ Value = $Plan.Values[$key]; Kind = 'String' } }
                    $registryState.Snapshot = [pscustomobject]@{ Exists = $true; Values = $values }; $registryState.Writes++
                }
                'Restore' { $registryState.Snapshot = $Snapshot }
            }
        }.GetNewClosure()
        $registrationPrepare = {
            param($Plan, $Payload, $Context)
            $registration = Get-LauncherRegistrationPlan -Directory $Plan.DestinationDirectory -Scope $Plan.Scope -UserSid $Plan.UserSid -Version $Payload.VersionLabel
            New-LauncherRegistrationTransaction -Plan $registration -Context $Context -Store $registryStore
        }.GetNewClosure()
        $real = Invoke-LauncherInstallWorker -Request $realRequest -ResultPath $resultPath -Context $script:context -RegistrationPrepare $registrationPrepare
        Check ($real.Status -ceq 'Succeeded') ('Integrated worker installation failed: ' + $real.Message)
        Check ([IO.File]::Exists($real.Deployment.ActiveExecutable)) 'Integrated worker did not install its active entry.'
        Check ((Get-FileHash -LiteralPath $real.Deployment.ActiveExecutable -Algorithm SHA256).Hash -ceq
            (Get-FileHash -LiteralPath $realPayload.ExecutablePath -Algorithm SHA256).Hash) 'Integrated worker installed different executable bytes.'
        Check (!(Test-Path -LiteralPath (Join-Path $fixture 'Startup'))) 'Worker wrote an initiating-user Startup folder.'
        Check (!(Test-Path -LiteralPath (Join-Path $script:context.LocalDestination 'Launcher.toml'))) 'Worker invented personal settings in the application folder.'

        $nativeDirectory = Join-Path $fixture 'native'
        [void][IO.Directory]::CreateDirectory($nativeDirectory)
        $nativeConfig = Join-Path $nativeDirectory 'worker.toml'
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
name = "Installer worker fixture"
color = "#BED878"
items = [{ label = "Fixture", icon = { stock = "folder" } }]
[logging]
file = "native.log"
'@, [Text.UTF8Encoding]::new($false))
        $nativeProcess = Start-Process -FilePath $real.Deployment.ActiveExecutable -ArgumentList @('--test-ui', '--config', ('"' + $nativeConfig + '"')) -WindowStyle Hidden -PassThru
        try {
            $nativeLog = Join-Path $nativeDirectory 'native.log'
            $deadline = [Diagnostics.Stopwatch]::StartNew()
            while ($deadline.ElapsedMilliseconds -lt 10000) {
                if ([IO.File]::Exists($nativeLog) -and (Read-WorkerLiveLog $nativeLog).Contains('Drawing initialized')) { break }
                $nativeProcess.Refresh()
                if ($nativeProcess.HasExited) { throw 'The owned worker fixture exited before initialization.' }
                Start-Sleep -Milliseconds 50
            }
            Check ([IO.File]::Exists($nativeLog) -and (Read-WorkerLiveLog $nativeLog).Contains('Drawing initialized')) 'The owned installed fixture must initialize before upgrade.'
            $running = Get-LauncherDiscovery -PackageDirectory $realRequest.PackageDirectory -LocalDestination $script:context.LocalDestination `
                -MachineDestination $script:context.MachineDestination -StartupDirectory $realRequest.StartupDirectory
            $realRequest.RestartInstances = @(Get-LauncherInstallationRestartPlan $running $realRequest.PackageDirectory `
                $realRequest.DestinationDirectory $realRequest.UserSid $realRequest.SessionId)
            Check ($realRequest.RestartInstances.Count -eq 1 -and $realRequest.RestartInstances[0].ProcessId -eq $nativeProcess.Id -and
                $realRequest.RestartInstances[0].Arguments -ceq ('--config "' + $nativeConfig + '" --test-ui')) 'The real cross-process reply must preserve the normalized configuration and UI options.'
            $realRequest.Action = 'Upgrade'
            $upgraded = Invoke-LauncherInstallWorker -Request $realRequest -ResultPath $resultPath -Context $script:context -RegistrationPrepare $registrationPrepare
            Check ($upgraded.Status -ceq 'Succeeded') ('Running-target worker upgrade failed: ' + $upgraded.Message)
            Check ($nativeProcess.WaitForExit(1000)) 'The installer committed without waiting for its owned Launcher process to exit.'
            Check (($upgraded.StoppedProcessIds -join ',') -eq [string]$nativeProcess.Id) 'The worker receipt must identify the real instance it stopped.'
            Check ([IO.File]::ReadAllText($nativeLog).Contains('Graceful close requested.')) 'The worker must close its owned Launcher through the normal graceful path.'
            Check (!$upgraded.Deployment.PreviousExecutable -and
                [IO.File]::Exists($upgraded.Deployment.VersionedExecutable) -and
                (Get-FileHash -LiteralPath $upgraded.Deployment.VersionedExecutable -Algorithm SHA256).Hash -ceq $real.Deployment.Sha256) 'An identical running-target reinstall must remain idempotent without duplicating its executable.'
            Check ($script:cimQueries -eq 0) 'Fresh installation and a running-target upgrade must work without querying CIM/WMI.'
            Check ($registryState.Writes -eq 2 -and $registryState.Snapshot.Values.LauncherInstallScope.Value -eq 'CurrentUser') 'Both integrated file transactions must refresh only their mocked current-user registration.'
        } finally {
            # This handle belongs only to the fixture process created above.
            if (!$nativeProcess.HasExited) { $nativeProcess.Kill(); [void]$nativeProcess.WaitForExit(5000) }
            $nativeProcess.Dispose()
        }

        # Exercise the actual command-line entry as well. Invalid JSON can never
        # reach deployment, and must replace a stale success with a clear failure.
        [IO.File]::WriteAllText($requestPath, '{ invalid request')
        $arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + [IO.Path]::GetFullPath($WorkerScript) +
            '" -RequestPath "' + $requestPath + '" -ResultPath "' + $resultPath + '"'
        $child = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -ArgumentList $arguments -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $fixture 'worker-output.txt') -RedirectStandardError (Join-Path $fixture 'worker-error.txt')
        try {
            Check ($child.WaitForExit(10000)) 'The invalid-request worker did not exit promptly.'
            $receipt = [IO.File]::ReadAllText($resultPath) | ConvertFrom-Json
            Check ($child.ExitCode -ne 0 -and $receipt.Status -ceq 'Failed' -and ![string]::IsNullOrWhiteSpace($receipt.Message)) 'The command-line worker left a stale success after a malformed request.'
        } finally {
            if (!$child.HasExited) { $child.Kill(); $child.WaitForExit() }
            $child.Dispose()
        }

        $setup = Join-Path $PackageDirectory 'Launcher Setup.exe'
        if (Test-Path -LiteralPath $setup -PathType Leaf) {
            # A valid transport with a mismatched package exercises the verified
            # handoff and can never reach installation, even under an admin token.
            [IO.File]::WriteAllText($requestPath, (@{ PackageDirectory = $fixture } | ConvertTo-Json))
            $child = Start-Process -FilePath $setup -ArgumentList ('--worker "{0}" "{1}"' -f $requestPath, $resultPath) -PassThru
            try {
                Check ($child.WaitForExit(10000)) 'The native worker did not exit promptly.'
                $receipt = [IO.File]::ReadAllText($resultPath) | ConvertFrom-Json
                Check ($child.ExitCode -eq 1 -and $receipt.Status -ceq 'Failed' -and
                    $receipt.Message -ceq 'The installation request does not refer to this Setup package.') 'The native worker did not pass its verified package to the script.'
            } finally {
                if (!$child.HasExited) { $child.Kill(); $child.WaitForExit() }
                $child.Dispose()
            }
        }
    }
    Write-Output ('Installer worker passed: {0} checks; personal profiles, elevation and unrelated running applications were untouched.' -f $script:checks)
} finally {
    foreach ($name in $cimOverrides.Keys) {
        if ($null -ne $cimOverrides[$name]) { Set-Item -LiteralPath ('Function:\' + $name) -Value $cimOverrides[$name] }
        else { Remove-Item -LiteralPath ('Function:\' + $name) -ErrorAction SilentlyContinue }
    }
    # Resolve the generated root before recursive removal; never clean a caller's
    # package, profile or installation directory.
    $resolved = [IO.Path]::GetFullPath($fixture).TrimEnd('\')
    if ([IO.Path]::GetDirectoryName($resolved) -ieq $temporaryRoot -and [IO.Path]::GetFileName($resolved) -ceq $fixtureName) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
