#requires -Version 5.1
[CmdletBinding()]
param([string]$RequestPath, [string]$ResultPath, [switch]$DisableLauncherArchiveSupport)

# The wizard stays in the initiating user's context. This worker only deploys
# program files, scope-correct registration and the product recovery shortcut.
# Startup shortcuts, profile snapshots and launching belong to the coordinator.
Set-StrictMode -Version Latest

function Get-LauncherWorkerContext {
    $publisher = 'D' + [char]0x0142 + 'ugosz'
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    try {
        $sid = $identity.User.Value
        $elevated = [Security.Principal.WindowsPrincipal]::new($identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    } finally { $identity.Dispose() }
    $programFiles = if ($env:ProgramW6432) { $env:ProgramW6432 } else { [Environment]::GetFolderPath('ProgramFiles') }
    return [pscustomobject]@{
        UserSid = $sid
        IsElevated = $elevated
        SessionId = [Diagnostics.Process]::GetCurrentProcess().SessionId
        LocalDestination = [IO.Path]::Combine([Environment]::GetFolderPath('LocalApplicationData'), 'Programs', $publisher, 'Launcher')
        MachineDestination = [IO.Path]::Combine($programFiles, $publisher, 'Launcher')
    }
}

function ConvertTo-LauncherWorkerPath {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or ![IO.Path]::IsPathRooted($Path)) {
        throw 'Setup requires an absolute path.'
    }
    return [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
}

function ConvertTo-LauncherUpdaterTransactionId($Text) {
    $id = [Guid]::Empty
    if ($Text -is [string] -and $Text -cmatch '\A[0-9a-f]{32}\z') {
        $null = [Guid]::TryParseExact($Text, 'N', [ref]$id)
    }
    return $id
}

function Get-LauncherUpdaterStagingDirectory([string]$PackageDirectory) {
    $outer = [IO.Path]::GetDirectoryName((Assert-LauncherSetupDirectory $PackageDirectory))
    $entryPoint = Get-LauncherSetupEntry (Join-Path $outer 'Install.cmd')
    if ($entryPoint -and !$entryPoint.PSIsContainer -and !($entryPoint.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        return $outer
    }
}

function Read-LauncherUpdaterMarker([string]$Directory, [string]$Name) {
    $marker = Get-LauncherSetupEntry (Join-Path $Directory $Name)
    if (!$marker -or $marker.PSIsContainer -or ($marker.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        return [Guid]::Empty
    }
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($marker.FullName))
    try { return ConvertTo-LauncherUpdaterTransactionId ([Text.Encoding]::ASCII.GetString($reader.ReadBytes(33))) }
    finally { $reader.Dispose() }
}

function Get-LauncherUpdaterTransactionId([string]$PackageDirectory) {
    # The wizard reads the optional outer ZIP marker before handing the worker a plain ID.
    try {
        $outer = Get-LauncherUpdaterStagingDirectory $PackageDirectory
        if ($outer) { return Read-LauncherUpdaterMarker $outer '.launcher-updater-transaction' }
    } catch { }
    return [Guid]::Empty
}

function Write-LauncherUpdaterStartupPlan {
    param($Request, $Deployment, [AllowEmptyCollection()][string[]]$RestartArguments)

    # Repairs can retain an earlier receipt. Only a version committed by this run
    # can authorize Updater to own the launch after Finish.
    if (!(Get-LauncherDiscoveryProperty $Deployment 'CommittedUpdate')) { return $false }
    $transaction = ConvertTo-LauncherUpdaterTransactionId (Get-LauncherDiscoveryProperty $Request 'UpdaterTransactionId')
    $receipt = [pscustomobject](Get-LauncherDiscoveryProperty $Deployment 'LastUpdate')
    $current = [pscustomobject](Get-LauncherDiscoveryProperty $receipt 'Current')
    if ($transaction -eq [Guid]::Empty -or $Request.Scope -cnotin @('AllUsers', 'CurrentUser') -or
        (Get-LauncherDiscoveryProperty $receipt 'SchemaVersion') -ne 1 -or
        (Get-LauncherDiscoveryProperty $receipt 'TransactionId') -cne $transaction.ToString('N') -or
        !(Test-LauncherQuickRollbackBuildIdentity $current) -or
        $current.Sha256 -cne (Get-LauncherDiscoveryProperty $Deployment 'Sha256') -or
        (Get-LauncherDiscoveryProperty $Deployment 'ActiveExecutable') -ine (Join-Path $Request.DestinationDirectory 'Launcher.exe')) {
        return $false
    }
    try {
        $outer = Get-LauncherUpdaterStagingDirectory $Request.PackageDirectory
        if (!$outer -or (Read-LauncherUpdaterMarker $outer '.launcher-updater-transaction') -ne $transaction -or
            (Read-LauncherUpdaterMarker $outer '.launcher-updater-watch-startup') -ne $transaction) { return $false }
    } catch { return $false }

    # The application path comes from Updater's independently checked installation
    # receipt. The array distinguishes declining launch from a launch with no arguments.
    # Keep Updater's protocol limits in sync so older direct launch remains available
    # when a launch cannot be represented by the startup plan.
    if ($RestartArguments.Count -gt 128) { return $false }
    foreach ($arguments in $RestartArguments) {
        if ($arguments.Length -gt 30000 -or $arguments.Contains([string][char]0)) { return $false }
    }
    $plan = [ordered]@{
        SchemaVersion = 1; TransactionId = $transaction.ToString('N'); Scope = $Request.Scope
        RestartArguments = @($RestartArguments)
    }
    $json = $plan | ConvertTo-Json -Depth 3 -Compress
    $bytes = [Text.UTF8Encoding]::new($false, $true).GetBytes($json)
    if ($bytes.Length -gt 8MB) { return $false }
    $path = Join-Path $outer '.launcher-updater-startup-plan.json'
    $temporary = Join-Path $outer ('.launcher-updater-startup-plan-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
        # Publish atomically without replacing any existing directory entry.
        [IO.File]::Move($temporary, $path)
    } catch {
        try { [IO.File]::Delete($temporary) } catch { }
        throw
    }
    return $true
}

function Assert-LauncherWorkerRequest {
    param([Parameter(Mandatory)]$Request, $Context = (Get-LauncherWorkerContext))

    foreach ($name in @('PackageDirectory', 'DestinationDirectory', 'Scope', 'Action', 'LocalDestination',
            'MachineDestination', 'UserSid', 'SessionId', 'StartupDirectory', 'Payload', 'RestartInstances')) {
        if ($null -eq $Request.PSObject.Properties[$name] -or $null -eq $Request.$name) {
            throw "The installation request is missing $name."
        }
    }
    if ($Request.Scope -cnotin @('AllUsers', 'CurrentUser') -or $Request.Action -cnotin @('Install', 'Upgrade')) {
        throw 'The installation scope or action is invalid.'
    }
    $quick = $Request.PSObject.Properties['QuickRollbackEligible']
    if ($quick -and ($quick.Value -isnot [bool] -or ($quick.Value -and $Request.Action -cne 'Upgrade'))) {
        throw 'The profile preservation request is invalid.'
    }
    $null = [Security.Principal.SecurityIdentifier]::new([string]$Request.UserSid)
    $session = 0
    if (![int]::TryParse([string]$Request.SessionId, [ref]$session) -or $session -lt 0 -or $session -ne $Context.SessionId) {
        throw 'The installation request belongs to another desktop session.'
    }
    $package = ConvertTo-LauncherWorkerPath $Request.PackageDirectory
    $destination = ConvertTo-LauncherWorkerPath $Request.DestinationDirectory
    $machine = ConvertTo-LauncherWorkerPath $Request.MachineDestination
    $local = ConvertTo-LauncherWorkerPath $Request.LocalDestination
    $null = ConvertTo-LauncherWorkerPath $Request.StartupDirectory
    if ($machine -ine (ConvertTo-LauncherWorkerPath $Context.MachineDestination) -or $machine -ieq $local) {
        throw 'The all-users application folder does not match Program Files.'
    }
    if ($Request.Scope -ceq 'AllUsers') {
        if ($destination -ine $machine) { throw 'The all-users destination does not match Program Files.' }
    } elseif ($Request.UserSid -cne $Context.UserSid -or
        $local -ine (ConvertTo-LauncherWorkerPath $Context.LocalDestination) -or $destination -ine $local) {
        throw 'A personal installation must use the initiating user and that user''s Local Programs folder.'
    }
    if ($package -ieq $destination) { throw 'Extract the download into a separate folder before installing it.' }
    $processIds = [Collections.Generic.HashSet[int]]::new()
    foreach ($instance in @($Request.RestartInstances)) {
        foreach ($name in @('ProcessId', 'Path', 'OwnerSid', 'SessionId', 'CreationTimeUtc', 'Arguments', 'ExecutableHash')) {
            if ($null -eq $instance -or $null -eq $instance.PSObject.Properties[$name] -or $null -eq $instance.$name) {
                throw 'Saved restart information is incomplete.'
            }
        }
        $processId = 0
        if (![int]::TryParse([string]$instance.ProcessId, [ref]$processId) -or $processId -le 0 -or !$processIds.Add($processId) -or
            $instance.OwnerSid -cne $Request.UserSid -or $instance.SessionId -ne $session -or
            $instance.ExecutableHash -cnotmatch '^[0-9A-Fa-f]{64}$' -or
            $instance.Arguments -isnot [string] -or $instance.Arguments.Length -ge 32767 -or $instance.Arguments.Contains([string][char]0)) {
            throw 'Saved restart information is invalid.'
        }
        $instancePath = ConvertTo-LauncherWorkerPath $instance.Path
        if ([IO.Path]::GetDirectoryName($instancePath) -notin @($package, $destination)) {
            throw 'Saved restart information refers to another installation.'
        }
        $null = ([datetime]$instance.CreationTimeUtc).ToUniversalTime()
    }
}

function Assert-LauncherWorkerPayload {
    param([Parameter(Mandatory)]$Expected, [Parameter(Mandatory)]$Actual)

    foreach ($payload in @($Expected, $Actual)) {
        if ($null -eq $payload.PSObject.Properties['PackageDirectory'] -or
            $null -eq $payload.PSObject.Properties['Files']) { throw 'The reviewed package manifest is incomplete.' }
    }
    if ((ConvertTo-LauncherWorkerPath $Expected.PackageDirectory) -ine
        (ConvertTo-LauncherWorkerPath $Actual.PackageDirectory)) { throw 'The reviewed package directory changed.' }
    $expectedFiles = @($Expected.Files)
    $actualFiles = @($Actual.Files)
    if (!$expectedFiles.Count -or $expectedFiles.Count -ne $actualFiles.Count) {
        throw 'The package contents changed after review. Refresh the setup findings.'
    }
    $reviewed = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in $expectedFiles) {
        foreach ($name in @('RelativePath', 'SourcePath', 'Sha256', 'PreserveExisting')) {
            if ($null -eq $file.PSObject.Properties[$name]) { throw 'The reviewed file manifest is incomplete.' }
        }
        if ($reviewed.ContainsKey([string]$file.RelativePath)) { throw 'The reviewed package contains duplicate file entries.' }
        $reviewed.Add([string]$file.RelativePath, $file)
    }
    foreach ($file in $actualFiles) {
        $prior = $null
        if (!$reviewed.TryGetValue([string]$file.RelativePath, [ref]$prior) -or
            [string]$prior.Sha256 -notmatch '^[0-9a-fA-F]{64}$' -or $prior.Sha256 -ine $file.Sha256 -or
            (ConvertTo-LauncherWorkerPath $prior.SourcePath) -ine (ConvertTo-LauncherWorkerPath $file.SourcePath) -or
            $prior.PreserveExisting -isnot [bool] -or $prior.PreserveExisting -ne $file.PreserveExisting) {
            throw 'The package contents changed after review. Refresh the setup findings.'
        }
    }
}

function Assert-LauncherWorkerSelection {
    param([Parameter(Mandatory)]$Request, [Parameter(Mandatory)]$Discovery)

    $selected = @($Discovery.Locations | Where-Object { $_.Scope -ceq $Request.Scope })
    if ($selected.Count -ne 1 -or
        (ConvertTo-LauncherWorkerPath $selected[0].Directory) -ine (ConvertTo-LauncherWorkerPath $Request.DestinationDirectory)) {
        throw 'The selected application folder changed. Refresh the setup findings.'
    }
    $installations = @($Discovery.Installations)
    if ($installations.Count) {
        $target = if ($installations.Count -eq 1) { $installations[0] }
            else { @($installations | Where-Object Scope -CEQ 'CurrentUser')[0] }
        if ($null -eq $target -or $Request.Action -cne 'Upgrade' -or !$selected[0].IsInstalled -or
            !$selected[0].CanUpgrade -or $selected[0].State -cne 'Verified' -or
            (ConvertTo-LauncherWorkerPath $target.Directory) -ine (ConvertTo-LauncherWorkerPath $Request.DestinationDirectory)) {
            throw 'Existing installations require an upgrade in place. Refresh the setup findings.'
        }
    } elseif ($Request.Action -cne 'Install' -or $selected[0].IsInstalled -or
        $selected[0].State -cnotin @('Missing', 'Available')) {
        throw 'The application folder no longer permits a fresh installation. Refresh the setup findings.'
    }
}

function Test-LauncherWorkerPermissionError {
    param([Parameter(Mandatory)]$Exception)
    $destinationFailure = $false
    $permissionFailure = $false
    for ($cause = $Exception; $null -ne $cause; $cause = $cause.InnerException) {
        if ($cause.Data['LauncherPermissionDenied'] -eq $true) { $destinationFailure = $true }
        if ($cause -is [UnauthorizedAccessException] -or
            ($cause -is [ComponentModel.Win32Exception] -and $cause.NativeErrorCode -eq 5)) { $permissionFailure = $true }
    }
    return $destinationFailure -and $permissionFailure
}

function Test-LauncherPermissionFailure {
    param([Parameter(Mandatory)]$Failure)
    $cause = if ($Failure -is [Management.Automation.ErrorRecord]) { $Failure.Exception } else { $Failure }
    for (; $null -ne $cause; $cause = $cause.InnerException) {
        if ($cause -is [UnauthorizedAccessException] -or
            ($cause -is [ComponentModel.Win32Exception] -and $cause.NativeErrorCode -in @(5, 740, 1223, 1314))) {
            return $true
        }
    }
    return $false
}

function Test-LauncherCanFallBackToPersonal {
    param([Parameter(Mandatory)]$Request)
    return $Request.Scope -ceq 'AllUsers' -and $Request.Action -ceq 'Install'
}

function Assert-LauncherWorkerTransport {
    param([Parameter(Mandatory)][string]$RequestPath, [Parameter(Mandatory)][string]$ResultPath)
    $request = ConvertTo-LauncherWorkerPath $RequestPath
    $result = ConvertTo-LauncherWorkerPath $ResultPath
    $directory = [IO.Path]::GetDirectoryName($request)
    if ([IO.Path]::GetFileName($request) -cne 'request.json' -or [IO.Path]::GetFileName($result) -cne 'result.json' -or
        [IO.Path]::GetDirectoryName($result) -ine $directory -or
        [IO.Path]::GetFileName($directory) -cnotmatch '^\.launcher-setup-[0-9a-f]{32}$') {
        throw 'Setup request and result must share their dedicated temporary folder.'
    }
    foreach ($path in @($request, $result)) {
        for ($current = $path; $current; $current = [IO.Path]::GetDirectoryName($current)) {
            $entry = $null
            try { $entry = Get-Item -LiteralPath $current -Force -ErrorAction Stop }
            catch { if ($_.CategoryInfo.Category -ne 'ObjectNotFound') { throw } }
            if ($null -ne $entry -and ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw "Setup's temporary paths must not cross a reparse point: $current"
            }
            if ($current -eq $path -and $null -ne $entry -and $entry.PSIsContainer) {
                throw 'A setup request or result path is a directory.'
            }
        }
    }
    if (![IO.File]::Exists($request)) { throw 'The setup request file is missing.' }
}

function Write-LauncherWorkerResult {
    param([Parameter(Mandatory)][string]$ResultPath, [Parameter(Mandatory)]$Result)
    Assert-LauncherWorkerTransport -RequestPath (Join-Path ([IO.Path]::GetDirectoryName($ResultPath)) 'request.json') -ResultPath $ResultPath
    $temporary = Join-Path ([IO.Path]::GetDirectoryName($ResultPath)) ('.result-' + [Guid]::NewGuid().ToString('N') + '.json')
    try {
        $json = $Result | ConvertTo-Json -Depth 16
        [IO.File]::WriteAllText($temporary, $json, [Text.UTF8Encoding]::new($false))
        if ([IO.File]::Exists($ResultPath)) { [IO.File]::Replace($temporary, $ResultPath, [NullString]::Value) }
        else { [IO.File]::Move($temporary, $ResultPath) }
    } finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}

function Invoke-LauncherInstallWorker {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]$Request, [Parameter(Mandatory)][string]$ResultPath,
        $Context = (Get-LauncherWorkerContext),
        [scriptblock]$PayloadReader = { param($Directory) Get-LauncherPayload -PackageDirectory $Directory },
        [scriptblock]$DiscoveryReader = {
            param($Plan)
            Get-LauncherDiscovery -PackageDirectory $Plan.PackageDirectory -LocalDestination $Plan.LocalDestination `
                -MachineDestination $Plan.MachineDestination -StartupDirectory $Plan.StartupDirectory
        },
        [scriptblock]$Deploy = {
            param($Package, $Destination, $BeforeCommit, $Progress, $AfterActivation, $Scope, $UserSid, $UpdaterTransactionId, $QuickRollbackEligible)
            Install-LauncherPayload -PackageDirectory $Package -InstallDirectory $Destination -BeforeCommit $BeforeCommit -Progress $Progress `
                -AfterActivation $AfterActivation -InstallScope $Scope -InstallUserSid $UserSid -UpdaterTransactionId $UpdaterTransactionId `
                -QuickRollbackEligible:$QuickRollbackEligible
        },
        [scriptblock]$RegistrationPrepare = {
            param($Plan, $Payload, $WorkerContext)
            $bytes = 0L
            foreach ($file in @($Payload.Files)) { $bytes += [IO.FileInfo]::new($file.SourcePath).Length }
            $registration = Get-LauncherRegistrationPlan -Directory $Plan.DestinationDirectory -Scope $Plan.Scope -UserSid $Plan.UserSid `
                -Version $Payload.VersionLabel -EstimatedSize ([int][Math]::Min([int]::MaxValue, [Math]::Ceiling($bytes / 1024.0)))
            New-LauncherRegistrationTransaction -Plan $registration -Context $WorkerContext
        },
        [scriptblock]$RegistrationWrite = { param($Transaction) Set-LauncherRegistrationTransaction $Transaction },
        [scriptblock]$RegistrationRestore = { param($Transaction) Restore-LauncherRegistrationTransaction $Transaction },
        [scriptblock]$StopProcesses = {
            param($Found, $Plan, $StoppedProcessIds)
            Stop-LauncherInstallationProcesses -Discovery $Found -PackageDirectory $Plan.PackageDirectory `
                -DestinationDirectory $Plan.DestinationDirectory -UserSid $Plan.UserSid -SessionId $Plan.SessionId `
                -RestartInstances @($Plan.RestartInstances) -StoppedProcessIds $StoppedProcessIds
        },
        [scriptblock]$AssertStopped = {
            param($Found, $Plan)
            Assert-LauncherInstallationStopped -Discovery $Found -PackageDirectory $Plan.PackageDirectory `
                -DestinationDirectory $Plan.DestinationDirectory -UserSid $Plan.UserSid -SessionId $Plan.SessionId
        },
        [scriptblock]$ResultWriter = { param($Path, $Result) Write-LauncherWorkerResult -ResultPath $Path -Result $Result },
        [scriptblock]$RecoveryShortcut
    )
    $state = [pscustomobject]@{
        DeploymentEntered = $false; CommitEntered = $false; Progress = 0
        StoppedProcessIds = [Collections.Generic.List[int]]::new()
    }
    $registration = $null
    $lock = $null
    try {
        Assert-LauncherWorkerRequest -Request $Request -Context $Context
        $null = & $ResultWriter $ResultPath ([pscustomobject]@{ Status = 'Running'; Message = 'Checking the installation package.'; Progress = 0; Deployment = $null })
        $payload = & $PayloadReader $Request.PackageDirectory
        Assert-LauncherWorkerPayload -Expected $Request.Payload -Actual $payload
        $lock = Enter-LauncherInstallationLock $Request.DestinationDirectory
        if (Get-LauncherSetupEntry (Join-Path $Request.DestinationDirectory '.launcher-archive-restore.json')) {
            if ($DisableLauncherArchiveSupport) {
                throw 'An earlier Launcher archive restore was interrupted. Run its installed recovery maintenance before continuing.'
            }
            $null = & $AssertStopped (& $DiscoveryReader $Request) $Request
            # Read recovery code from this Setup package. The interrupted
            # installation can contain a mixture of the two archive versions.
            foreach ($name in @('ArchiveRecovery.ps1', 'ArchiveRestoreJournal.ps1')) {
                $scriptPath = Join-Path $Request.PackageDirectory $name
                $entry = Get-LauncherSetupEntry $scriptPath
                if (!$entry -or $entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                    throw 'An earlier Launcher archive restore was interrupted. Run its installed recovery maintenance before continuing.'
                }
                . $scriptPath
            }
            Invoke-LauncherArchiveRestoreRecovery -Directory $Request.DestinationDirectory -RegistrationScope $Request -Context $Context
        }
        $found = & $DiscoveryReader $Request
        Assert-LauncherWorkerSelection -Request $Request -Discovery $found
        # Preflight registration independently of application-folder ACLs. A
        # writable Program Files subfolder does not grant permission to HKLM.
        $registration = & $RegistrationPrepare $Request $payload $Context
        $progress = {
            param([int]$Percent, [string]$Message)
            $state.Progress = [Math]::Min(99, [Math]::Max(0, $Percent))
            $null = & $ResultWriter $ResultPath ([pscustomobject]@{ Status = 'Running'; Message = $Message; Progress = $state.Progress; Deployment = $null })
        }
        $beforeCommit = {
            $state.CommitEntered = $true
            $current = & $DiscoveryReader $Request
            Assert-LauncherWorkerSelection -Request $Request -Discovery $current
            $null = & $StopProcesses $current $Request $state.StoppedProcessIds
            $current = & $DiscoveryReader $Request
            Assert-LauncherWorkerSelection -Request $Request -Discovery $current
            $null = & $AssertStopped $current $Request
        }
        $state.DeploymentEntered = $true
        $afterActivation = { param($ActivePath) & $RegistrationWrite $registration | Out-Null }.GetNewClosure()
        $marker = $Request.PSObject.Properties['UpdaterTransactionId']
        $transactionId = if ($null -eq $marker) { [Guid]::Empty } else { ConvertTo-LauncherUpdaterTransactionId $marker.Value }
        $quick = $Request.PSObject.Properties['QuickRollbackEligible']
        $deployment = & $Deploy $Request.PackageDirectory $Request.DestinationDirectory $beforeCommit $progress $afterActivation $Request.Scope $Request.UserSid $transactionId ([bool]($quick -and $quick.Value))
        $shortcutWarning = $null
        if ($RecoveryShortcut) {
            try { $null = & $RecoveryShortcut $registration.Plan $deployment }
            catch { $shortcutWarning = 'Some Launcher Start menu shortcuts could not be updated. ' + $_.Exception.Message }
        }
        $result = [pscustomobject]@{
            Status = 'Succeeded'; Message = 'Launcher was installed successfully.'; Progress = 100; Deployment = $deployment
            StoppedProcessIds = $state.StoppedProcessIds.ToArray(); RecoveryShortcutWarning = $shortcutWarning
        }
    } catch {
        $failure = $_
        $message = $failure.Exception.Message
        if ($registration) {
            try { & $RegistrationRestore $registration | Out-Null }
            catch { $message += ' Programs and Features recovery also failed: ' + $_.Exception.Message }
        }
        $status = if (!$state.CommitEntered -and
            (Test-LauncherWorkerPermissionError $failure.Exception)) { 'PermissionDenied' } else { 'Failed' }
        $result = [pscustomobject]@{
            Status = $status; Message = $message; Progress = $state.Progress; Deployment = $null
            StoppedProcessIds = $state.StoppedProcessIds.ToArray()
        }
    } finally { Exit-LauncherInstallationLock $lock }
    $null = & $ResultWriter $ResultPath $result
    return $result
}

if ($MyInvocation.InvocationName -ne '.') {
    $ErrorActionPreference = 'Stop'
    $transportValidated = $false
    try {
        Assert-LauncherWorkerTransport -RequestPath $RequestPath -ResultPath $ResultPath
        $transportValidated = $true
        . (Join-Path $PSScriptRoot 'Discovery.ps1')
        . (Join-Path $PSScriptRoot 'Deployment.ps1')
        . (Join-Path $PSScriptRoot 'Lifecycle.ps1')
        $request = [IO.File]::ReadAllText($RequestPath) | ConvertFrom-Json
        if ((ConvertTo-LauncherWorkerPath $request.PackageDirectory) -ine (ConvertTo-LauncherWorkerPath $PSScriptRoot)) {
            throw 'The installation request does not refer to this Setup package.'
        }
        $request.PackageDirectory = $PSScriptRoot
        $result = Invoke-LauncherInstallWorker -Request $request -ResultPath $ResultPath -RecoveryShortcut {
            param($Registration, $Deployment)
            Set-LauncherStartMenuShortcuts (Get-LauncherRecoveryShortcutPlan $Registration) (Get-LauncherRecoveryIconLocation $Deployment)
        }
        if ($result.Status -ceq 'Succeeded') { exit 0 }
        exit 1
    } catch {
        $failure = $_
        # Only write after the command-line transport itself was validated.
        if ($transportValidated) {
            Write-LauncherWorkerResult -ResultPath $ResultPath -Result ([pscustomobject]@{
                Status = 'Failed'; Message = $failure.Exception.Message; Progress = 0; Deployment = $null
                StoppedProcessIds = @()
            })
        }
        Write-Error $failure
        exit 1
    }
}
