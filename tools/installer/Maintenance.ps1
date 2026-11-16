#requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('List', 'Rollback', 'QuickRollback', 'ArchiveRollback', 'CleanupAcceptedRelease', 'RecoverArchiveRestore', 'Repair', 'Uninstall', 'RemoveStartupShortcut')][string]$Action = 'List',
    [string]$InstallDirectory,
    [string]$ExecutableName,
    [string]$SourceDirectory,
    [string]$ConfigurationPath,
    [string]$Scope,
    [string]$UserSid,
    [string]$ExpectedTransactionId,
    [string]$SourceArchive,
    [string]$SourceSignature,
    [string]$CurrentArchive,
    [string]$CurrentSignature,
    [switch]$FileOnly,
    [switch]$NoElevation,
    [switch]$Interactive,
    [int]$SetupProcessId
)

function Show-LauncherMaintenanceMessage {
    param([string]$Message, [switch]$Failure,
        [scriptblock]$Display = {
            param($Text, $IsFailure)
            Add-Type -AssemblyName System.Windows.Forms
            $icon = if ($IsFailure) { [Windows.Forms.MessageBoxIcon]::Error } else { [Windows.Forms.MessageBoxIcon]::Information }
            [Windows.Forms.MessageBox]::Show($Text, 'Launcher uninstall', [Windows.Forms.MessageBoxButtons]::OK, $icon) | Out-Null
        })
    & $Display $Message ([bool]$Failure)
}

# The file-only worker never reads or changes AppData or another account's
# Startup folder. All destructive operations require an explicit install path.
if ($MyInvocation.InvocationName -ne '.') {
    $ErrorActionPreference = 'Stop'
    $env:PSModulePath = [IO.Path]::Combine($PSHOME, 'Modules')
}
try {
    . (Join-Path $PSScriptRoot 'Discovery.ps1')
    . (Join-Path $PSScriptRoot 'Deployment.ps1')
    . (Join-Path $PSScriptRoot 'Lifecycle.ps1')
} catch {
    if ($MyInvocation.InvocationName -ne '.' -and $Interactive) {
        Show-LauncherMaintenanceMessage -Message ('Launcher could not start uninstall.' + "`n`n" + $_.Exception.Message) -Failure
    }
    throw
}

function Assert-LauncherMaintenanceDirectory([string]$Path) {
    $directory = Assert-LauncherSetupDirectory $Path
    if ($directory -ieq [IO.Path]::GetPathRoot($directory).TrimEnd('\') -or !(Test-Path -LiteralPath $directory -PathType Container)) {
        throw 'Maintenance requires an existing dedicated Launcher directory, not a drive root.'
    }
    return $directory
}

function Get-LauncherMaintenanceIdentity {
    param([string]$Path, [scriptblock]$MetadataReader = { param($File) [Diagnostics.FileVersionInfo]::GetVersionInfo($File) })
    $name = [IO.Path]::GetFileName($Path)
    if ($name -notmatch '^Launcher(?:-R\d+\.\d+(?:\.\d+){0,2})?(?:\.[0-9a-z-]+(?:\.[0-9a-z-]+)*\.backup)?\.exe$' -and
        $name -ine 'LauncherDrop.exe' -and $name -ine 'LauncherSvgExtractor.exe' -and $name -notmatch '^Launcher-WIP(?:\.legacy\.[0-9a-f]+\.backup)?\.exe$') { return $null }
    $canonical = if ($name -in @('LauncherDrop.exe', 'LauncherSvgExtractor.exe')) { $name } else { 'Launcher.exe' }
    # Retained backup names intentionally are not discovery candidates. Check
    # their actual bytes using the same product/component metadata criteria.
    $actualReader = { param($Ignored) & $MetadataReader $Path }.GetNewClosure()
    $identity = Get-LauncherExecutableIdentity (Join-Path ([IO.Path]::GetDirectoryName($Path)) $canonical) $actualReader
    if ($identity.State -ne 'Verified') { return $null }
    return $identity
}

function Get-LauncherRetainedExecutables {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Directory,
        [scriptblock]$MetadataReader = { param($File) [Diagnostics.FileVersionInfo]::GetVersionInfo($File) })
    $install = Assert-LauncherMaintenanceDirectory $Directory
    if (!(Test-Path -LiteralPath $install -PathType Container)) { throw "Installation directory is missing: $install" }
    foreach ($file in @(Get-ChildItem -LiteralPath $install -Filter 'Launcher*.exe' -File -Force)) {
        if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
        $identity = Get-LauncherMaintenanceIdentity $file.FullName $MetadataReader
        if (!$identity -or $identity.Component -eq 'LauncherSvgExtractor') { continue }
        [pscustomobject]@{ Name = $file.Name; Path = $file.FullName; Version = $identity.Version;
            Component = $identity.Component; Sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash }
    }
}

function Assert-LauncherMaintenanceStopped {
    param([string]$Directory, [scriptblock]$ProcessReader = { Get-LauncherProcessCandidates })
    $install = [IO.Path]::GetFullPath($Directory).TrimEnd('\')
    foreach ($process in @(& $ProcessReader)) {
        if (!$process.ExecutablePath) { continue }
        if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($process.ExecutablePath)) -ieq $install) {
            throw "Close Launcher and its companion tools in '$install' before maintenance, then retry."
        }
    }
}

function Get-LauncherMaintenanceProcessDiscovery {
    param([string]$Directory, [scriptblock]$ProcessReader, [scriptblock]$MetadataReader)
    $install = Assert-LauncherMaintenanceDirectory $Directory
    $targets = [Collections.Generic.List[object]]::new()
    foreach ($candidate in @(& $ProcessReader)) {
        $path = [string](Get-LauncherDiscoveryProperty $candidate 'ExecutablePath')
        if (!$path) { throw 'A running Launcher process cannot be located. Close it manually and retry maintenance.' }
        $path = ConvertTo-LauncherDiscoveryPath $path
        if ([IO.Path]::GetDirectoryName($path) -ine $install) { continue }
        $identity = Get-LauncherExecutableIdentity $path $MetadataReader
        if ($identity.State -ne 'Verified' -or $identity.Component -notin @('Launcher', 'LauncherDrop', 'LauncherSvgExtractor')) {
            throw "Cannot verify running application '$path'. Close it manually and retry maintenance."
        }
        $targets.Add([pscustomobject]@{
            ProcessId = Get-LauncherDiscoveryProperty $candidate 'ProcessId'; Path = $path
            OwnerSid = Get-LauncherDiscoveryProperty $candidate 'OwnerSid'
            SessionId = Get-LauncherDiscoveryProperty $candidate 'SessionId'
            CreationTimeUtc = Get-LauncherDiscoveryProperty $candidate 'CreationDate'
            Identity = $identity; Component = $identity.Component
        })
    }
    return [pscustomobject]@{ ProcessEnumerationSucceeded = $true; Processes = $targets.ToArray() }
}

function Stop-LauncherUninstallProcesses {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Directory,
        $Context = (Get-LauncherRegistrationContext),
        [int]$SessionId = ([Diagnostics.Process]::GetCurrentProcess().SessionId),
        [ValidateRange(1, 30000)][int]$TimeoutMs = 5000,
        [scriptblock]$ProcessReader = { Get-LauncherProcessCandidates },
        [scriptblock]$ProcessPin = { param($Id) Initialize-LauncherLifecycleNative; [LauncherSetup.RunningProcess]::Pin($Id) },
        [scriptblock]$MetadataReader = { param($Path) [Diagnostics.FileVersionInfo]::GetVersionInfo($Path) })
    $install = Assert-LauncherMaintenanceDirectory $Directory
    # The installer already validates every target and pins its process identity
    # before sending WM_CLOSE, then waits for actual process exit. Reuse that
    # path without reading Startup or another profile merely to stop this copy.
    $discovery = Get-LauncherMaintenanceProcessDiscovery $install $ProcessReader $MetadataReader
    $result = Stop-LauncherInstallationProcesses -Discovery $discovery -PackageDirectory $install -DestinationDirectory $install `
        -UserSid $Context.UserSid -SessionId $SessionId -TimeoutMs $TimeoutMs -ProcessPin $ProcessPin -MetadataReader $MetadataReader
    Assert-LauncherMaintenanceStopped $install $ProcessReader
    return $result
}

function New-LauncherMaintenanceAlias {
    param([string]$Path, [string]$Target, [switch]$CopyOnly)
    if (Get-LauncherSetupEntry $Path) { throw "Alias path already exists: $Path" }
    if (!$CopyOnly) {
        try { New-Item -ItemType HardLink -Path $Path -Target $Target -ErrorAction Stop | Out-Null }
        catch { if (Get-LauncherSetupEntry $Path) { throw }; [IO.File]::Copy($Target, $Path, $false) }
    } else { [IO.File]::Copy($Target, $Path, $false) }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash) { throw 'Alias verification failed.' }
}

function Set-LauncherRollbackVersion {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Directory, [Parameter(Mandatory)][string]$Name,
        [scriptblock]$MetadataReader = { param($File) [Diagnostics.FileVersionInfo]::GetVersionInfo($File) },
        [scriptblock]$ProcessReader = { Get-LauncherProcessCandidates }, [switch]$CopyOnly,
        [scriptblock]$VerifyActive = { param($Path, $Sha256)
            if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Sha256) { throw 'Rollback activation failed verification.' }
        })
    $install = Assert-LauncherMaintenanceDirectory $Directory
    if ([IO.Path]::GetFileName($Name) -cne $Name -or $Name -in @('Launcher.exe', 'LauncherDrop.exe', 'Launcher-WIP.exe')) { throw 'Select a retained executable basename from List.' }
    $selected = @(Get-LauncherRetainedExecutables $install $MetadataReader | Where-Object { $_.Name -ceq $Name -and $_.Component -eq 'Launcher' })
    if ($selected.Count -ne 1) { throw 'The selected retained executable is missing or does not identify Launcher.' }
    Assert-LauncherMaintenanceStopped $install $ProcessReader
    Initialize-LauncherSetupFileApi
    $active = Join-Path $install 'Launcher.exe'
    Assert-LauncherSetupActiveEntry $active
    $stage = Join-Path $install ('.launcher-rollback-' + [Guid]::NewGuid().ToString('N') + '.exe')
    $backup = $null
    $published = $false
    try {
        New-LauncherMaintenanceAlias $stage $selected[0].Path -CopyOnly:$CopyOnly
        if (Get-LauncherSetupEntry $active) {
            $backup = Join-Path $install ('Launcher.' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.' + [Guid]::NewGuid().ToString('N') + '.backup.exe')
            # Copy to a fresh path, then rename the new alias over the old entry.
            # Never copy onto Launcher.exe while it may be a hard link.
            [IO.File]::Copy($active, $backup, $false)
            if ((Get-FileHash $active).Hash -ne (Get-FileHash $backup).Hash) { throw 'Previous executable backup failed verification.' }
        }
        Move-LauncherSetupFile $stage $active ($null -ne (Get-LauncherSetupEntry $active))
        $published = $true
        & $VerifyActive $active $selected[0].Sha256 | Out-Null
    } catch {
        $failure = $_
        if ($published) {
            if ($backup) { Move-LauncherSetupFile $backup $active $true }
            elseif (Get-LauncherSetupEntry $active) { [IO.File]::Delete($active) }
        }
        throw $failure
    } finally { if (Get-LauncherSetupEntry $stage) { [IO.File]::Delete($stage) } }
    [pscustomobject]@{ ActiveExecutable = $active; SelectedExecutable = $selected[0].Path; PreviousExecutable = $backup;
        Configuration = 'Existing profile unchanged. Use an explicit --config path to launch a saved configuration.' }
}

function Assert-LauncherQuickRollbackFile {
    param([string]$Path, [string]$Sha256)
    [void](Assert-LauncherSetupDirectory ([IO.Path]::GetDirectoryName($Path)))
    $entry = Get-LauncherSetupEntry $Path
    if (!$entry -or $entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -cne $Sha256) {
        throw "A file required for quick rollback is missing or changed: $Path"
    }
}

function Get-LauncherQuickRollbackPlan {
    param([string]$Directory, $RegistrationScope, [string]$TransactionId)
    if (!$RegistrationScope -or $TransactionId -cnotmatch '\A[0-9a-f]{32}\z' -or $TransactionId -ceq ('0' * 32)) {
        throw 'Quick rollback requires the exact registered installation and update transaction.'
    }
    $install = Assert-LauncherMaintenanceDirectory $Directory
    $manifestPath = Join-Path $install 'Launcher.install-manifest.json'
    $entry = Get-LauncherSetupEntry $manifestPath
    if (!$entry -or $entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        $entry.Length -le 0 -or $entry.Length -gt 1MB) { throw 'The installation manifest could not be verified.' }
    $manifestBytes = [IO.File]::ReadAllBytes($manifestPath)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { $manifestHash = [BitConverter]::ToString($algorithm.ComputeHash($manifestBytes)).Replace('-', '') }
    finally { $algorithm.Dispose() }
    $manifest = [Text.Encoding]::UTF8.GetString($manifestBytes) | ConvertFrom-Json
    $scope = [string](Get-LauncherDiscoveryProperty $manifest 'InstallScope')
    $owner = [string](Get-LauncherDiscoveryProperty $manifest 'InstallUserSid')
    $review = Get-LauncherDiscoveryProperty $manifest 'LastUpdate'
    $marker = Get-LauncherDiscoveryProperty $manifest 'QuickRollback'
    if ((Get-LauncherDiscoveryProperty $manifest 'SchemaVersion') -ne 1 -or
        $scope -cne $RegistrationScope.Scope -or $owner -cne [string]$RegistrationScope.UserSid -or
        (Get-LauncherDiscoveryProperty $manifest 'AliasKind') -cnotin @('HardLink', 'Copy') -or
        (Get-LauncherDiscoveryProperty $review 'SchemaVersion') -ne 1 -or
        (Get-LauncherDiscoveryProperty $review 'TransactionId') -cne $TransactionId -or
        (Get-LauncherDiscoveryProperty $marker 'SchemaVersion') -ne 1 -or
        (Get-LauncherDiscoveryProperty $marker 'TransactionId') -cne $TransactionId) {
        throw 'This installation has no matching verified quick rollback.'
    }
    $previous = Get-LauncherDiscoveryProperty $review 'Previous'
    $current = Get-LauncherDiscoveryProperty $review 'Current'
    foreach ($build in @($previous, $current)) {
        if (!(Test-LauncherQuickRollbackBuildIdentity $build)) {
            throw 'The update receipt does not contain two verified build identities.'
        }
    }
    $active = Join-Path $install 'Launcher.exe'
    $currentName = "Launcher-R$($current.Version).exe"
    $previousName = "Launcher-R$($previous.Version).exe"
    if ($currentName -ceq $previousName -or $current.Sha256 -ceq $previous.Sha256 -or $manifest.ActiveVersionedExecutable -cne $currentName) {
        throw 'The update receipt does not identify the current and previous executable.'
    }
    $retained = Join-Path $install $previousName
    Assert-LauncherQuickRollbackFile $active $current.Sha256
    Assert-LauncherQuickRollbackFile (Join-Path $install $currentName) $current.Sha256
    Assert-LauncherQuickRollbackFile $retained $previous.Sha256
    foreach ($pair in @(@($active, $current.Version), @($retained, $previous.Version))) {
        Assert-LauncherSetupExecutable $pair[0] 'Launcher'
        $metadata = [Diagnostics.FileVersionInfo]::GetVersionInfo($pair[0])
        if ((Get-LauncherVersionLabel $metadata) -cne $pair[1]) { throw 'An executable does not match the version in the update receipt.' }
    }
    $companions = Get-LauncherQuickRollbackCompanionHashes $manifest.Files $current.Version $current.Sha256
    foreach ($relative in $companions.Keys) { Assert-LauncherQuickRollbackFile (Join-Path $install $relative) $companions[$relative] }
    [pscustomobject]@{ Manifest = $manifest; ManifestPath = $manifestPath; ManifestHash = $manifestHash; Active = $active;
        Current = $current; Previous = $previous; CurrentName = $currentName; PreviousName = $previousName; Retained = $retained; Companions = $companions }
}

function Invoke-LauncherQuickRollback {
    [CmdletBinding()]
    param([string]$Directory, $RegistrationScope, [string]$TransactionId, $Context = (Get-LauncherRegistrationContext),
        [scriptblock]$Store = { param($Operation, $Plan, $Snapshot) Invoke-LauncherRegistrationStore $Operation $Plan $Snapshot },
        [scriptblock]$ProcessReader = { Get-LauncherProcessCandidates })
    $install = Assert-LauncherMaintenanceDirectory $Directory
    $lock = Enter-LauncherInstallationLock $install
    $staging = $null
    $keepStaging = $false
    $aliasPublished = $false
    $manifestPublished = $false
    $registration = $null
    try {
        $metadata = { param($Path) [Diagnostics.FileVersionInfo]::GetVersionInfo($Path) }
        if (@((Get-LauncherMaintenanceProcessDiscovery $install $ProcessReader $metadata).Processes).Count) {
            throw 'Close Launcher and its companion tools before quick rollback.'
        }
        $plan = Get-LauncherQuickRollbackPlan $install $RegistrationScope $TransactionId
        $registrationPlan = Get-LauncherRegistrationPlan -Directory $install -Scope $RegistrationScope.Scope -UserSid $RegistrationScope.UserSid -Version $plan.Previous.Version
        $registration = New-LauncherRegistrationTransaction -Plan $registrationPlan -Context $Context -Store $Store
        $oldValues = $registration.Snapshot.Values
        Assert-LauncherQuickRollbackRegistration $registrationPlan $registration.Snapshot $plan.Current.Version
        foreach ($name in @('EstimatedSize', 'InstallDate')) {
            if ($oldValues.Contains($name)) { $registrationPlan.Values[$name] = $oldValues[$name].Value }
        }
        Initialize-LauncherSetupFileApi
        $staging = Join-Path $install ('.launcher-rollback-' + [Guid]::NewGuid().ToString('N'))
        [void][IO.Directory]::CreateDirectory($staging)
        $activeStage = Join-Path $staging 'active.new'
        $activeBackup = Join-Path $staging 'active.backup'
        $manifestStage = Join-Path $staging 'manifest.new'
        $manifestBackup = Join-Path $staging 'manifest.backup'
        $aliasKind = New-LauncherSetupAlias -Path $activeStage -Target $plan.Retained
        Assert-LauncherQuickRollbackFile $activeStage $plan.Previous.Sha256
        Assert-LauncherSetupReplaceAccess $plan.Active
        Assert-LauncherSetupReplaceAccess $plan.ManifestPath
        if ($plan.Manifest.AliasKind -ceq 'HardLink') {
            if ((New-LauncherSetupAlias -Path $activeBackup -Target $plan.Active) -cne 'HardLink') {
                throw 'The current hard-link alias could not be preserved before rollback.'
            }
        } else { [IO.File]::Copy($plan.Active, $activeBackup, $false) }
        [IO.File]::Copy($plan.ManifestPath, $manifestBackup, $false)
        Assert-LauncherQuickRollbackFile $activeBackup $plan.Current.Sha256
        Assert-LauncherQuickRollbackFile $manifestBackup $plan.ManifestHash
        $manifest = $plan.Manifest
        $manifest.Files = @($manifest.Files | Where-Object RelativePath -CNE $plan.CurrentName)
        $activeEntry = $manifest.Files | Where-Object RelativePath -CEQ 'Launcher.exe'
        $activeEntry.Sha256 = $plan.Previous.Sha256
        $manifest.Files += [pscustomobject]@{ RelativePath = $plan.PreviousName; Sha256 = $plan.Previous.Sha256 }
        $manifest.ActiveVersionedExecutable = $plan.PreviousName
        $manifest.AliasKind = $aliasKind
        foreach ($name in @('LastUpdate', 'QuickRollback', 'LastRollback')) { $manifest.PSObject.Properties.Remove($name) }
        $history = Get-LauncherDiscoveryProperty $manifest 'ArchiveHistory'
        if ($history) {
            $previousArchive = Get-LauncherDiscoveryProperty $history 'Previous'
            $manifest.PSObject.Properties.Remove('ArchiveHistory')
            if ($previousArchive -and (Get-LauncherDiscoveryProperty $history 'TransactionId') -ceq $TransactionId) {
                $manifest | Add-Member -NotePropertyName ArchiveHistory -NotePropertyValue ([ordered]@{ SchemaVersion = 1; Current = $previousArchive })
            }
        }
        $manifest | Add-Member -NotePropertyName LastRollback -NotePropertyValue ([ordered]@{
            SchemaVersion = 1; TransactionId = $TransactionId
            Restored = [ordered]@{ Version = $plan.Previous.Version; Sha256 = $plan.Previous.Sha256 }
        })
        [IO.File]::WriteAllText($manifestStage, ($manifest | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
        $restoredManifestHash = (Get-FileHash -LiteralPath $manifestStage -Algorithm SHA256).Hash

        if (@((Get-LauncherMaintenanceProcessDiscovery $install $ProcessReader $metadata).Processes).Count) {
            throw 'Launcher restarted while quick rollback was prepared.'
        }
        $now = Get-LauncherQuickRollbackPlan $install $RegistrationScope $TransactionId
        if ($now.ManifestHash -cne $plan.ManifestHash) { throw 'The installation changed while rollback was prepared.' }
        Assert-LauncherQuickRollbackRegistration $registrationPlan (& $Store 'Read' $registrationPlan $null) $plan.Current.Version
        Move-LauncherSetupFile $activeStage $plan.Active $true
        $aliasPublished = $true
        Assert-LauncherQuickRollbackFile $plan.Active $plan.Previous.Sha256
        Set-LauncherRegistrationTransaction $registration
        $installed = & $Store 'Read' $registrationPlan $null
        Assert-LauncherQuickRollbackRegistration $registrationPlan $installed $plan.Previous.Version
        Move-LauncherSetupFile $manifestStage $plan.ManifestPath $true
        $manifestPublished = $true
        Assert-LauncherQuickRollbackFile $plan.ManifestPath $restoredManifestHash
        Assert-LauncherQuickRollbackFile $plan.Active $plan.Previous.Sha256
        foreach ($relative in $plan.Companions.Keys) {
            Assert-LauncherQuickRollbackFile (Join-Path $install $relative) $plan.Companions[$relative]
        }
    } catch {
        $failure = $_
        $recovery = [Collections.Generic.List[string]]::new()
        if ($manifestPublished) {
            try {
                Assert-LauncherQuickRollbackFile $plan.ManifestPath $restoredManifestHash
                Move-LauncherSetupFile $manifestBackup $plan.ManifestPath $true
            } catch { $recovery.Add($_.Exception.Message) }
        }
        try { Restore-LauncherRegistrationTransaction $registration }
        catch { $recovery.Add($_.Exception.Message) }
        if ($aliasPublished) {
            try {
                Assert-LauncherQuickRollbackFile $plan.Active $plan.Previous.Sha256
                Move-LauncherSetupFile $activeBackup $plan.Active $true
            } catch { $recovery.Add($_.Exception.Message) }
        }
        if ($recovery.Count) {
            $keepStaging = $true
            throw "Quick rollback failed: $($failure.Exception.Message) Recovery files retained in $staging. $($recovery -join ' ')"
        }
        throw $failure
    } finally {
        try {
            if ($staging -and !$keepStaging -and [IO.Directory]::Exists($staging)) {
                foreach ($name in @('active.new', 'active.backup', 'manifest.new', 'manifest.backup')) {
                    try { [IO.File]::Delete((Join-Path $staging $name)) } catch { }
                }
                try { [IO.Directory]::Delete($staging) } catch { }
            }
        } finally { Exit-LauncherInstallationLock $lock }
    }
}

function Invoke-LauncherArchiveRollback {
    [CmdletBinding()]
    param([string]$Directory, $RegistrationScope, [string]$TransactionId, [hashtable]$Archives,
        $Context = (Get-LauncherRegistrationContext),
        [scriptblock]$Store = { param($Operation, $Plan, $Snapshot) Invoke-LauncherRegistrationStore $Operation $Plan $Snapshot },
        [scriptblock]$ProcessReader = { Get-LauncherProcessCandidates })
    $lock = Enter-LauncherInstallationLock $Directory
    $staging = $null
    $registration = $null
    try {
        Invoke-LauncherArchiveRestoreRecovery -Directory $Directory -RegistrationScope $RegistrationScope -Context $Context -Store $Store
        $plan = Get-LauncherArchiveRestoreManifest $Directory $RegistrationScope $TransactionId
        $registrationPlan = Get-LauncherRegistrationPlan -Directory $Directory -Scope $RegistrationScope.Scope -UserSid $RegistrationScope.UserSid -Version $plan.Previous.Version
        $registration = New-LauncherRegistrationTransaction -Plan $registrationPlan -Context $Context -Store $Store
        Assert-LauncherQuickRollbackRegistration $registrationPlan $registration.Snapshot $plan.Current.Version
        foreach ($name in @('EstimatedSize', 'InstallDate')) {
            if ($registration.Snapshot.Values.Contains($name)) { $registrationPlan.Values[$name] = $registration.Snapshot.Values[$name].Value }
        }
        $staging = Join-Path $Directory ('.launcher-restore-source-' + [Guid]::NewGuid().ToString('N'))
        [void][IO.Directory]::CreateDirectory($staging)
        $current = Get-LauncherVerifiedArchive -Archive $Archives.CurrentArchive -Signature $Archives.CurrentSignature `
            -Version $plan.Current.Version -ExpectedIdentity $plan.CurrentArchive
        $previous = Get-LauncherVerifiedArchive -Archive $Archives.SourceArchive -Signature $Archives.SourceSignature `
            -Version $plan.Previous.Version -ExpectedIdentity $plan.PreviousArchive -ExtractDirectory $staging
        Assert-LauncherArchiveRestorePackages $plan $current $previous
        $plan | Add-Member -NotePropertyName PreviousFiles -NotePropertyValue $previous.Files

        $beforeCommit = {
            Assert-LauncherArchiveRestoreManifest $Directory $plan
            Assert-LauncherQuickRollbackRegistration $registrationPlan (& $Store 'Read' $registrationPlan $null) $plan.Current.Version
            $metadata = { param($Path) [Diagnostics.FileVersionInfo]::GetVersionInfo($Path) }
            if (@((Get-LauncherMaintenanceProcessDiscovery $Directory $ProcessReader $metadata).Processes).Count) {
                throw 'Close Launcher and its companion tools before restoring an archived version.'
            }
        }
        $afterActivation = {
            Set-LauncherRegistrationTransaction $registration | Out-Null
            Assert-LauncherQuickRollbackRegistration $registrationPlan (& $Store 'Read' $registrationPlan $null) $plan.Previous.Version
        }
        $null = Install-LauncherPayload -PackageDirectory $staging -InstallDirectory $Directory -InstallScope $RegistrationScope.Scope `
            -InstallUserSid $RegistrationScope.UserSid -BeforeCommit $beforeCommit -AfterActivation $afterActivation -ArchiveRestore $plan
        Remove-LauncherArchiveRestoreJournal -Directory $Directory
    } catch {
        if (Get-LauncherSetupEntry (Join-Path $Directory '.launcher-archive-restore.json')) {
            Invoke-LauncherArchiveRestoreRecovery -Directory $Directory -RegistrationScope $RegistrationScope -Context $Context -Store $Store
        } else { Restore-LauncherRegistrationTransaction $registration }
        throw
    } finally {
        try {
            if ($staging -and (Get-LauncherSetupEntry $staging)) {
                Remove-LauncherArchiveSourceStage $staging
            }
        } finally { Exit-LauncherInstallationLock $lock }
    }
}

function Remove-LauncherAcceptedReleaseFallback {
    [CmdletBinding()]
    param([string]$Directory, $RegistrationScope, [string]$TransactionId, [hashtable]$Archives,
        $Context = (Get-LauncherRegistrationContext),
        [scriptblock]$Store = { param($Operation, $Plan, $Snapshot) Invoke-LauncherRegistrationStore $Operation $Plan $Snapshot })

    Assert-LauncherRegistrationContext -Scope $RegistrationScope.Scope -UserSid $RegistrationScope.UserSid -Context $Context
    $lock = Enter-LauncherInstallationLock $Directory
    try {
        if (Get-LauncherSetupEntry (Join-Path $Directory '.launcher-archive-restore.json')) {
            throw 'An archived revert was interrupted. Finish recovery before cleaning up the previous Launcher executable.'
        }
        $plan = Get-LauncherArchiveRestoreManifest $Directory $RegistrationScope $TransactionId
        $registration = Get-LauncherRegistrationPlan -Directory $Directory -Scope $RegistrationScope.Scope -UserSid $RegistrationScope.UserSid
        Assert-LauncherQuickRollbackRegistration $registration (& $Store 'Read' $registration $null) $plan.Current.Version
        $active = Join-Path $Directory 'Launcher.exe'
        $metadata = [Diagnostics.FileVersionInfo]::GetVersionInfo($active)
        if ((Get-LauncherDistributionKind $metadata) -cne 'Release') {
            throw 'Only an accepted Launcher Release can remove its retained executable.'
        }
        $incoming = Get-LauncherExactVersion $metadata -RequireFixed
        if ([version]$plan.Previous.Version -ge $incoming) { return }

        $candidates = @(Get-LauncherSupersededExecutables -Directory $Directory -IncomingVersion $incoming |
            Where-Object { $_.Sha256 -ceq $plan.Previous.Sha256 -and [IO.Path]::GetFileName($_.Path) -ine 'Launcher-WIP.exe' })
        if (!$candidates.Count) { return }
        if ($candidates.Count -ne 1) { throw 'More than one copy of the previous Launcher executable was found. Both were retained.' }
        $retained = Get-LauncherReleaseReviewFallback $plan.Review $candidates ''
        if (!$retained) { throw 'The retained Launcher executable could not be matched to the update receipt.' }

        $current = Get-LauncherVerifiedArchive -Archive $Archives.CurrentArchive -Signature $Archives.CurrentSignature `
            -Version $plan.Current.Version -ExpectedIdentity $plan.CurrentArchive
        $previous = Get-LauncherVerifiedArchive -Archive $Archives.SourceArchive -Signature $Archives.SourceSignature `
            -Version $plan.Previous.Version -ExpectedIdentity $plan.PreviousArchive
        Assert-LauncherArchiveRestorePackages $plan $current $previous

        # Apply the same ownership policy as archive Revert. Installed managed
        # program files must match the current archive, and files present only
        # in the older package must be safe to create or already match it.
        Assert-LauncherQuickRollbackFile (Join-Path $Directory "Launcher-R$($plan.Current.Version).exe") $plan.Current.Sha256
        foreach ($relative in $plan.Owned.Keys) {
            if (!(Test-LauncherArchiveRestoreEditableFile $relative)) {
                Assert-LauncherQuickRollbackFile (Join-Path $Directory $relative) $plan.Owned[$relative]
            }
        }
        foreach ($relative in $previous.Files.Keys) {
            if ($relative -in @('Launcher.archive.json', 'Launcher.exe')) { continue }
            $path = Join-Path $Directory $relative
            [void](Assert-LauncherSetupDirectory ([IO.Path]::GetDirectoryName($path)))
            $entry = Get-LauncherSetupEntry $path
            if (!$entry) { continue }
            if ($entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw "The archived Launcher package cannot safely restore this path: $path"
            }
            if (!$plan.Owned.ContainsKey($relative) -and !(Test-LauncherArchiveRestoreEditableFile $relative)) {
                Assert-LauncherQuickRollbackFile $path $previous.Files[$relative]
            }
        }
        Assert-LauncherArchiveRestoreManifest $Directory $plan
        Assert-LauncherQuickRollbackRegistration $registration (& $Store 'Read' $registration $null) $plan.Current.Version
        Remove-LauncherVerifiedExecutable $retained $plan.Previous.Sha256
        # LastUpdate and ArchiveHistory still identify both signed packages.
        # Quick rollback checks the retained file before it can be offered.
    } finally { Exit-LauncherInstallationLock $lock }
}

function Start-LauncherReviewRollback {
    param([string]$Directory, $RegistrationScope, [string]$TransactionId, $Context, [switch]$NoElevation, [hashtable]$Archives)
    $review = if ($Archives) { Get-LauncherArchiveRestoreManifest $Directory $RegistrationScope $TransactionId }
        else { Get-LauncherQuickRollbackPlan $Directory $RegistrationScope $TransactionId }
    if ($RegistrationScope.Scope -ceq 'CurrentUser') {
        Assert-LauncherRegistrationContext -Scope CurrentUser -UserSid $RegistrationScope.UserSid -Context $Context
    }
    if ($NoElevation) {
        Assert-LauncherRegistrationContext -Scope $RegistrationScope.Scope -UserSid $RegistrationScope.UserSid -Context $Context
    }
    $registration = Get-LauncherRegistrationPlan -Directory $Directory -Scope $RegistrationScope.Scope -UserSid $RegistrationScope.UserSid
    Assert-LauncherQuickRollbackRegistration $registration (Invoke-LauncherRegistrationStore 'Read' $registration $null) $review.Current.Version
    if ($Archives) {
        $current = Get-LauncherVerifiedArchive -Archive $Archives.CurrentArchive -Signature $Archives.CurrentSignature `
            -Version $review.Current.Version -ExpectedIdentity $review.CurrentArchive
        $previous = Get-LauncherVerifiedArchive -Archive $Archives.SourceArchive -Signature $Archives.SourceSignature `
            -Version $review.Previous.Version -ExpectedIdentity $review.PreviousArchive
        Assert-LauncherArchiveRestorePackages $review $current $previous
    }
    $session = [Diagnostics.Process]::GetCurrentProcess().SessionId
    $read = { Get-LauncherProcessCandidates }
    $metadata = { param($Path) [Diagnostics.FileVersionInfo]::GetVersionInfo($Path) }
    $discovery = Get-LauncherMaintenanceProcessDiscovery $Directory $read $metadata
    $instances = @(Get-LauncherInstallationRestartPlan -Discovery $discovery -PackageDirectory $Directory -DestinationDirectory $Directory -UserSid $Context.UserSid -SessionId $session)
    $stopped = [Collections.Generic.List[int]]::new()
    try {
        $null = Stop-LauncherInstallationProcesses -Discovery $discovery -PackageDirectory $Directory -DestinationDirectory $Directory -UserSid $Context.UserSid -SessionId $session -RestartInstances $instances -StoppedProcessIds $stopped
        if ($RegistrationScope.Scope -ceq 'AllUsers' -and !$Context.IsElevated) {
            if ($Archives) {
                Start-LauncherElevatedMaintenance -Action ArchiveRollback -Directory $Directory -TransactionId $TransactionId @Archives
            } else { Start-LauncherElevatedMaintenance -Action QuickRollback -Directory $Directory -TransactionId $TransactionId }
        } elseif ($Archives) {
            Invoke-LauncherArchiveRollback -Directory $Directory -RegistrationScope $RegistrationScope -TransactionId $TransactionId -Context $Context -Archives $Archives
        } else {
            Invoke-LauncherQuickRollback -Directory $Directory -RegistrationScope $RegistrationScope -TransactionId $TransactionId -Context $Context
        }
    } catch {
        if (!$Archives -or !(Get-LauncherSetupEntry (Join-Path $Directory '.launcher-archive-restore.json'))) {
            foreach ($warning in @(Restore-LauncherInstallationInstances -Instances $instances -StoppedProcessIds $stopped.ToArray())) {
                [Console]::Error.WriteLine($warning)
            }
        }
        throw
    }
    return [pscustomobject]@{ RestartArguments = @($instances | Where-Object { $_.ProcessId -in $stopped } | ForEach-Object Arguments) }
}

function Get-LauncherUninstallFiles {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Directory,
        [scriptblock]$MetadataReader = { param($File) [Diagnostics.FileVersionInfo]::GetVersionInfo($File) })
    $install = Assert-LauncherMaintenanceDirectory $Directory
    $manifestPath = Join-Path $install 'Launcher.install-manifest.json'
    $manifestEntry = Get-LauncherSetupEntry $manifestPath
    if (!$manifestEntry -or $manifestEntry.PSIsContainer -or ($manifestEntry.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'A regular Launcher.install-manifest.json is required. Repair from a matching extracted package first.' }
    $manifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
    if ($manifest.SchemaVersion -ne 1 -or !$manifest.Files) { throw 'The install manifest is invalid. Repair this installation first.' }
    $paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in @($manifest.Files)) {
        $relative = [string]$file.RelativePath
        if (!(Test-LauncherManagedRelativePath $relative) -or [string]$file.Sha256 -notmatch '^[0-9a-fA-F]{64}$') { throw "Unexpected install manifest entry: $relative" }
        $path = Join-Path $install $relative
        [void](Assert-LauncherSetupDirectory ([IO.Path]::GetDirectoryName($path)))
        $entry = Get-LauncherSetupEntry $path
        if (!$entry) { continue }
        if ($entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) { continue }
        if ($entry.Extension -ieq '.exe' -and !(Get-LauncherMaintenanceIdentity $path $MetadataReader)) { continue }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -eq $file.Sha256) { [void]$paths.Add($path) }
    }
    # Include only independently identified executable names in this directory.
    foreach ($executable in @(Get-LauncherRetainedExecutables $install $MetadataReader)) { [void]$paths.Add($executable.Path) }
    # The companion is independently owned, but intentionally excluded from the
    # main executable's rollback list. Development acceptance can refresh it
    # after the last package receipt without changing its component identity.
    $extractorPath = Join-Path $install 'LauncherSvgExtractor.exe'
    $extractorEntry = Get-LauncherSetupEntry $extractorPath
    if ($extractorEntry -and !$extractorEntry.PSIsContainer -and !($extractorEntry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        $extractorIdentity = Get-LauncherMaintenanceIdentity $extractorPath $MetadataReader
        if ($extractorIdentity -and $extractorIdentity.Component -eq 'LauncherSvgExtractor') { [void]$paths.Add($extractorPath) }
    }
    foreach ($path in $paths) { [pscustomobject]@{ Path = $path; Sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash } }
    [pscustomobject]@{ Path = $manifestPath; Sha256 = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash }
}

function Remove-LauncherInstallationFiles {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Directory,
        [scriptblock]$MetadataReader = { param($File) [Diagnostics.FileVersionInfo]::GetVersionInfo($File) },
        [scriptblock]$ProcessReader = { Get-LauncherProcessCandidates },
        [scriptblock]$AfterFilesRemoved)
    $install = Assert-LauncherMaintenanceDirectory $Directory
    $files = @(Get-LauncherUninstallFiles $install $MetadataReader)
    Assert-LauncherMaintenanceStopped $install $ProcessReader
    Initialize-LauncherSetupFileApi
    # Test replacement rights for the whole plan before deleting the first file.
    foreach ($file in $files) { Assert-LauncherSetupReplaceAccess $file.Path }
    $bootstrap = @($files | Where-Object { [IO.Path]::GetExtension($_.Path) -in @('.ps1', '.cmd', '.cs') -or
        [IO.Path]::GetFileName($_.Path) -in @('Launcher Setup.exe', 'Launcher.install-manifest.json') })
    $ordinary = @($files | Where-Object { $_ -notin $bootstrap })
    foreach ($file in $ordinary) {
        if ((Get-FileHash -LiteralPath $file.Path -Algorithm SHA256).Hash -ne $file.Sha256) { throw "File changed during uninstall: $($file.Path)" }
        [IO.File]::Delete($file.Path)
    }
    # Keep the executable uninstaller, its dependencies and ownership receipt
    # until actual registry deletion succeeds. Failed removal remains retryable.
    if ($AfterFilesRemoved) { & $AfterFilesRemoved | Out-Null }
    $warnings = [Collections.Generic.List[string]]::new()
    foreach ($file in $bootstrap) {
        try {
            if ((Get-FileHash -LiteralPath $file.Path -Algorithm SHA256).Hash -ne $file.Sha256) { throw 'The helper changed during removal.' }
            [IO.File]::Delete($file.Path)
        } catch { $warnings.Add("Program files were removed, but a cleanup helper remains: $($file.Path). $($_.Exception.Message)") }
    }
    foreach ($relative in @('examples', 'docs', '')) {
        $path = if ($relative) { Join-Path $install $relative } else { $install }
        if (Test-Path -LiteralPath $path -PathType Container) {
            try { [IO.Directory]::Delete($path) } catch { } # Keep any user/foreign files.
        }
    }
    [pscustomobject]@{ Installation = $install; FilesRemoved = $files.Count - $warnings.Count; Profiles = 'Retained'; CleanupWarnings = @($warnings) }
}

function Remove-LauncherMatchingStartupShortcut {
    param([string]$Directory, [string]$StartupDirectory = [Environment]::GetFolderPath('Startup'))
    $install = [IO.Path]::GetFullPath($Directory).TrimEnd('\')
    $path = Join-Path $StartupDirectory 'Launcher.lnk'
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return }
    $entry = Get-Item -LiteralPath $path -Force
    if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Startup shortcut is a reparse point; left untouched.' }
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $null
    try {
        $shortcut = $shell.CreateShortcut($path)
        $target = [IO.Path]::GetFullPath($shortcut.TargetPath)
        if ([IO.Path]::GetDirectoryName($target) -ieq $install -and
            [IO.Path]::GetFileName($target) -match '^Launcher(?:-R\d+\.\d+(?:\.\d+){0,2})?\.exe$') {
            [IO.File]::Delete($path)
        }
    } finally {
        if ($shortcut -and [Runtime.InteropServices.Marshal]::IsComObject($shortcut)) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut) }
        if ([Runtime.InteropServices.Marshal]::IsComObject($shell)) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) }
    }
}

function Get-LauncherMaintenanceRegistrationScope {
    param([string]$Directory, [string]$Scope, [string]$UserSid)
    if ($Scope -and $Scope -cnotin @('AllUsers', 'CurrentUser')) { throw 'The requested installation scope is invalid.' }
    $manifestPath = Join-Path $Directory 'Launcher.install-manifest.json'
    $entry = Get-LauncherSetupEntry $manifestPath
    if ($entry -and ($entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint))) {
        throw 'The installation manifest must be a regular file before changing registration.'
    }
    $savedScope = ''; $savedSid = ''
    if ($entry) {
        $manifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
        $savedScope = [string](Get-LauncherDiscoveryProperty $manifest 'InstallScope')
        $savedSid = [string](Get-LauncherDiscoveryProperty $manifest 'InstallUserSid')
    }
    if ($savedScope -and $savedScope -cnotin @('AllUsers', 'CurrentUser')) { throw 'The saved installation scope is invalid.' }
    if ($Scope -and $savedScope -and $Scope -cne $savedScope) { throw 'The requested scope does not match this installation.' }
    if (!$Scope) { $Scope = $savedScope }
    if (!$Scope) { return $null } # Legacy file-only installs have no registration.
    if ($Scope -eq 'CurrentUser') {
        if ($UserSid -and $savedSid -and $UserSid -cne $savedSid) { throw 'The requested user does not match this personal installation.' }
        if (!$UserSid) { $UserSid = $savedSid }
        if (!$UserSid) { throw 'The original personal installation account is unavailable; repair it from that account before changing registration.' }
    }
    [pscustomobject]@{ Scope = $Scope; UserSid = $UserSid }
}

function Invoke-LauncherRegisteredMaintenance {
    [CmdletBinding()]
    param([ValidateSet('Rollback', 'Repair', 'Uninstall')][string]$Action,
        [Parameter(Mandatory)][string]$Directory, [Parameter(Mandatory)]$RegistrationScope,
        [string]$SourceDirectory, [string]$ExecutableName,
        $Context = (Get-LauncherRegistrationContext),
        [scriptblock]$Store = { param($Operation, $Plan, $Snapshot) Invoke-LauncherRegistrationStore $Operation $Plan $Snapshot },
        [scriptblock]$RemoveFiles = { param($Install, $AfterFilesRemoved) Remove-LauncherInstallationFiles $Install -AfterFilesRemoved $AfterFilesRemoved })
    $lock = Enter-LauncherInstallationLock $Directory
    try {
    $plan = Get-LauncherRegistrationPlan -Directory $Directory -Scope $RegistrationScope.Scope -UserSid $RegistrationScope.UserSid
    if ($Action -eq 'Repair') {
        $payload = Get-LauncherPayload $SourceDirectory
        $bytes = 0L
        foreach ($file in @($payload.Files)) { $bytes += [IO.FileInfo]::new($file.SourcePath).Length }
        $plan.Values.EstimatedSize = [int][Math]::Min([int]::MaxValue, [Math]::Ceiling($bytes / 1024.0))
    }
    $transaction = New-LauncherRegistrationTransaction -Plan $plan -Context $Context -Store $Store
    # Preserve the existing size estimate for an executable-only rollback.
    if ($Action -ne 'Repair' -and $transaction.Snapshot.Exists -and $transaction.Snapshot.Values.Contains('EstimatedSize')) {
        $plan.Values.EstimatedSize = [int]$transaction.Snapshot.Values['EstimatedSize'].Value
    }
    # A closure's temporary module cannot resolve script-local functions by name
    # when another PowerShell script calls Maintenance. Capture the functions here.
    $versionLabel = ${function:Get-LauncherVersionLabel}
    $publishRegistration = ${function:Set-LauncherRegistrationTransaction}
    $register = {
        param($Active, $Sha256)
        if ($Sha256 -and (Get-FileHash -LiteralPath $Active -Algorithm SHA256).Hash -ne $Sha256) { throw 'Rollback activation failed verification.' }
        $metadata = [Diagnostics.FileVersionInfo]::GetVersionInfo($Active)
        $plan.Values.DisplayVersion = & $versionLabel $metadata
        & $publishRegistration $transaction
    }.GetNewClosure()
    try {
        switch ($Action) {
            'Rollback' { Set-LauncherRollbackVersion -Directory $Directory -Name $ExecutableName -VerifyActive $register }
            'Repair' {
                Install-LauncherPayload -PackageDirectory $SourceDirectory -InstallDirectory $Directory `
                    -BeforeCommit { Assert-LauncherMaintenanceStopped $Directory } -AfterActivation $register `
                    -InstallScope $RegistrationScope.Scope -InstallUserSid $RegistrationScope.UserSid -QuickRollbackEligible
            }
            'Uninstall' {
                $removeTransaction = ${function:Remove-LauncherRegistrationTransaction}
                $removeRegistration = { & $removeTransaction $transaction }.GetNewClosure()
                $result = & $RemoveFiles $Directory $removeRegistration
                # A failed or blocked file removal must leave its Programs and
                # Features entry available for retry. HKCU is never used by an
                # alternate-account machine worker.
                $result
            }
        }
    } catch {
        $failure = $_
        try { Restore-LauncherRegistrationTransaction $transaction }
        catch { throw "Maintenance failed: $($failure.Exception.Message) Programs and Features recovery also failed: $($_.Exception.Message)" }
        throw $failure
    }
    } finally { Exit-LauncherInstallationLock $lock }
}

function Start-LauncherElevatedMaintenance {
    param([string]$Action, [string]$Directory, [string]$SourceDirectory, [string]$ExecutableName, [string]$TransactionId, [switch]$Interactive,
        [string]$SourceArchive, [string]$SourceSignature, [string]$CurrentArchive, [string]$CurrentSignature)
    foreach ($value in @($PSCommandPath, $Directory, $SourceDirectory, $ExecutableName, $SourceArchive, $SourceSignature, $CurrentArchive, $CurrentSignature)) {
        if ($value -and ($value.Contains('"') -or $value.Contains("`r") -or $value.Contains("`n"))) { throw 'Maintenance arguments cannot contain a quote or newline.' }
    }
    $arguments = '-NoLogo -NoProfile -STA -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Action ' + $Action +
        ' -InstallDirectory "' + $Directory + '" -Scope AllUsers -FileOnly -NoElevation'
    if ($Interactive) { $arguments += ' -Interactive' }
    if ($SourceDirectory) { $arguments += ' -SourceDirectory "' + [IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\') + '"' }
    if ($ExecutableName) { $arguments += ' -ExecutableName "' + $ExecutableName + '"' }
    if ($Action -in @('ArchiveRollback', 'CleanupAcceptedRelease')) {
        $arguments += ' -SourceArchive "' + $SourceArchive + '" -SourceSignature "' + $SourceSignature +
            '" -CurrentArchive "' + $CurrentArchive + '" -CurrentSignature "' + $CurrentSignature + '"'
    }
    if ($TransactionId) {
        if ($TransactionId -cnotmatch '\A[0-9a-f]{32}\z') { throw 'The requested rollback transaction is invalid.' }
        $arguments += ' -ExpectedTransactionId ' + $TransactionId
    }
    $powershell = Join-Path ([Environment]::GetFolderPath('Windows')) 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if (![Environment]::Is64BitProcess) { $powershell = Join-Path ([Environment]::GetFolderPath('Windows')) 'Sysnative\WindowsPowerShell\v1.0\powershell.exe' }
    $worker = Start-Process -FilePath $powershell -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -Wait -PassThru
    try {
        if ($worker.ExitCode -ne 0) {
            $failure = [InvalidOperationException]::new("The elevated maintenance did not finish (exit $($worker.ExitCode)). Remaining files and profiles were retained.")
            $failure.Data['LauncherInteractiveMessageShown'] = [bool]$Interactive
            throw $failure
        }
    }
    finally { $worker.Dispose() }
}

if ($MyInvocation.InvocationName -ne '.') {
    Set-StrictMode -Version Latest
    $ErrorActionPreference = 'Stop'
    try {
        if ($SetupProcessId -gt 0) {
            $setupProcess = Get-Process -Id $SetupProcessId -ErrorAction SilentlyContinue
            if ($setupProcess) {
                try {
                    if (!$setupProcess.WaitForExit(15000)) { throw 'Launcher Setup is still running. Retry uninstall after it closes.' }
                } finally { $setupProcess.Dispose() }
            }
        }
        if ($Interactive -and $Action -ne 'Uninstall') { throw 'Interactive maintenance is available only for Uninstall.' }
        if (!$InstallDirectory) { throw 'Specify -InstallDirectory with the exact installation to maintain.' }
        $install = Assert-LauncherMaintenanceDirectory $InstallDirectory
        if ($Action -eq 'Rollback' -and $ConfigurationPath -and
            (![IO.Path]::IsPathRooted($ConfigurationPath) -or !(Test-Path -LiteralPath $ConfigurationPath -PathType Leaf))) {
            throw 'ConfigurationPath must name an existing absolute configuration path.'
        }
        $registrationScope = Get-LauncherMaintenanceRegistrationScope -Directory $install -Scope $Scope -UserSid $UserSid
        $context = Get-LauncherRegistrationContext
        $pendingArchiveRestore = Get-LauncherSetupEntry (Join-Path $install '.launcher-archive-restore.json')
        if ($pendingArchiveRestore -and $Action -in @('QuickRollback', 'Rollback', 'Repair', 'Uninstall', 'CleanupAcceptedRelease')) {
            throw 'An archived revert was interrupted. Open Launcher Update History and Recovery before changing this installation.'
        }
        if ($Action -in @('ArchiveRollback', 'RecoverArchiveRestore', 'CleanupAcceptedRelease')) {
            $archiveScript = Join-Path $install 'ArchiveRecovery.ps1'
            $journalScript = Join-Path $install 'ArchiveRestoreJournal.ps1'
            if (!$Scope -or !$registrationScope -or $PSCommandPath -ine (Join-Path $install 'Maintenance.ps1')) {
                throw 'Archive maintenance requires the maintenance code of the registered installation and its explicit scope.'
            }
            $registration = Get-LauncherRegistrationPlan -Directory $install -Scope $registrationScope.Scope -UserSid $registrationScope.UserSid
            $snapshot = Invoke-LauncherRegistrationStore 'Read' $registration $null
            $version = if ($snapshot.Values.Contains('DisplayVersion')) { [string]$snapshot.Values['DisplayVersion'].Value } else { '' }
            Assert-LauncherQuickRollbackRegistration $registration $snapshot $version
            foreach ($script in @($archiveScript, $journalScript)) {
                $entry = Get-LauncherSetupEntry $script
                if (!$entry -or $entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                    throw 'The registered archive maintenance files could not be opened safely.'
                }
                . $script
            }
            if ($pendingArchiveRestore) {
                if ($registrationScope.Scope -ceq 'AllUsers' -and !$context.IsElevated -and !$FileOnly -and !$NoElevation) {
                    Start-LauncherElevatedMaintenance -Action RecoverArchiveRestore -Directory $install
                } else {
                    Assert-LauncherRegistrationContext -Scope $registrationScope.Scope -UserSid $registrationScope.UserSid -Context $context
                    $restoreLock = Enter-LauncherInstallationLock $install
                    try {
                        Assert-LauncherMaintenanceStopped $install
                        Invoke-LauncherArchiveRestoreRecovery -Directory $install -RegistrationScope $registrationScope -Context $context
                    }
                    finally { Exit-LauncherInstallationLock $restoreLock }
                }
            }
            if ($Action -ceq 'RecoverArchiveRestore') { return }
        }
        if ($Action -in @('QuickRollback', 'ArchiveRollback', 'CleanupAcceptedRelease')) {
            if (!$Scope -or !$registrationScope -or $SourceDirectory -or $ExecutableName -or $ConfigurationPath) {
                throw 'Revert requires an explicit scope and selects the previous version from the verified receipt.'
            }
            $archives = $null
            if ($Action -in @('ArchiveRollback', 'CleanupAcceptedRelease')) {
                foreach ($path in @($SourceArchive, $SourceSignature, $CurrentArchive, $CurrentSignature)) {
                    if (!$path -or $path -notmatch '\A[A-Za-z]:\\' -or [IO.Path]::GetFullPath($path) -ine $path) {
                        throw 'Archive maintenance requires absolute paths to both signed ZIPs and detached signatures.'
                    }
                }
                $archives = @{ SourceArchive = $SourceArchive; SourceSignature = $SourceSignature; CurrentArchive = $CurrentArchive; CurrentSignature = $CurrentSignature }
            }
            if ($Action -ceq 'CleanupAcceptedRelease') {
                if ($registrationScope.Scope -ceq 'AllUsers' -and !$context.IsElevated -and !$FileOnly -and !$NoElevation) {
                    Start-LauncherElevatedMaintenance -Action $Action -Directory $install -TransactionId $ExpectedTransactionId @archives
                } else {
                    Remove-LauncherAcceptedReleaseFallback -Directory $install -RegistrationScope $registrationScope `
                        -TransactionId $ExpectedTransactionId -Context $context -Archives $archives
                }
            } elseif ($FileOnly) {
                Assert-LauncherRegistrationContext -Scope $registrationScope.Scope -UserSid $registrationScope.UserSid -Context $context
                if ($archives) {
                    Invoke-LauncherArchiveRollback -Directory $install -RegistrationScope $registrationScope -TransactionId $ExpectedTransactionId -Context $context -Archives $archives
                } else { Invoke-LauncherQuickRollback -Directory $install -RegistrationScope $registrationScope -TransactionId $ExpectedTransactionId -Context $context }
            } else {
                $result = Start-LauncherReviewRollback -Directory $install -RegistrationScope $registrationScope -TransactionId $ExpectedTransactionId -Context $context -NoElevation:$NoElevation -Archives $archives
                $output = [Console]::OpenStandardOutput()
                $bytes = [Text.Encoding]::UTF8.GetBytes(($result | ConvertTo-Json -Compress) + [Environment]::NewLine)
                $output.Write($bytes, 0, $bytes.Length)
                $output.Flush()
            }
            return
        }
        if ($Action -eq 'Uninstall' -and !$FileOnly) {
            if ($registrationScope -and $registrationScope.Scope -eq 'CurrentUser') {
                Assert-LauncherRegistrationContext -Scope CurrentUser -UserSid $registrationScope.UserSid -Context $context
            }
            $null = @(Get-LauncherUninstallFiles $install)
            $null = Stop-LauncherUninstallProcesses -Directory $install -Context $context
        }
        if ($registrationScope -and $Action -in @('Rollback', 'Repair', 'Uninstall')) {
            if ($registrationScope.Scope -eq 'AllUsers' -and !$context.IsElevated) {
                if ($FileOnly -or $NoElevation) { Assert-LauncherRegistrationContext -Scope AllUsers -UserSid '' -Context $context }
                Start-LauncherElevatedMaintenance -Action $Action -Directory $install -SourceDirectory $SourceDirectory -ExecutableName $ExecutableName -Interactive:$Interactive
                if ($Action -eq 'Uninstall') { Remove-LauncherMatchingStartupShortcut $install }
                elseif ($Action -eq 'Rollback' -and $ConfigurationPath) {
                    Write-Output ('Launch with: "' + (Join-Path $install 'Launcher.exe') + '" --config "' + $ConfigurationPath + '"')
                }
                return
            }
            Assert-LauncherRegistrationContext -Scope $registrationScope.Scope -UserSid $registrationScope.UserSid -Context $context
        }
        switch ($Action) {
            'List' { Get-LauncherRetainedExecutables $install | Format-Table Name, Version, Sha256 -AutoSize }
            'Rollback' {
                if (!$ExecutableName) { throw 'Specify -ExecutableName using a retained filename shown by List.' }
                if ($ConfigurationPath -and (![IO.Path]::IsPathRooted($ConfigurationPath) -or !(Test-Path -LiteralPath $ConfigurationPath -PathType Leaf))) {
                    throw 'ConfigurationPath must name an existing absolute configuration path.'
                }
                if ($registrationScope) { Invoke-LauncherRegisteredMaintenance -Action Rollback -Directory $install -RegistrationScope $registrationScope -ExecutableName $ExecutableName -Context $context }
                else { Set-LauncherRollbackVersion $install $ExecutableName }
                if ($ConfigurationPath) {
                    Write-Output ('Launch with: "' + (Join-Path $install 'Launcher.exe') + '" --config "' + $ConfigurationPath + '"')
                }
            }
            'Repair' {
                if (!$SourceDirectory) { throw 'Specify -SourceDirectory with an extracted matching Launcher package.' }
                if ($registrationScope) { Invoke-LauncherRegisteredMaintenance -Action Repair -Directory $install -RegistrationScope $registrationScope -SourceDirectory $SourceDirectory -Context $context }
                else { Install-LauncherPayload -PackageDirectory $SourceDirectory -InstallDirectory $install -BeforeCommit { Assert-LauncherMaintenanceStopped $install } }
            }
            'RemoveStartupShortcut' { Remove-LauncherMatchingStartupShortcut $install }
            'Uninstall' {
                $elevated = $context.IsElevated
                $interactiveWorkerCompleted = $false
                try {
                    $removed = if ($registrationScope) { Invoke-LauncherRegisteredMaintenance -Action Uninstall -Directory $install -RegistrationScope $registrationScope -Context $context }
                    else { Remove-LauncherInstallationFiles $install }
                    $removed
                }
                catch {
                    $denied = $false
                    for ($cause = $_.Exception; $cause; $cause = $cause.InnerException) {
                        if ($cause -is [UnauthorizedAccessException] -or ($cause -is [ComponentModel.Win32Exception] -and $cause.NativeErrorCode -eq 5)) { $denied = $true }
                    }
                    if ($registrationScope -or !$denied -or $elevated -or $FileOnly -or $NoElevation) { throw }
                    if ($PSCommandPath.Contains('"') -or $install.Contains('"')) { throw 'Maintenance paths cannot contain a quote.' }
                    $arguments = '-NoProfile -STA -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Action Uninstall -InstallDirectory "' + $install + '" -FileOnly -NoElevation'
                    if ($Interactive) { $arguments += ' -Interactive' }
                    $workerPowerShell = Join-Path ([Environment]::GetFolderPath('System')) 'WindowsPowerShell\v1.0\powershell.exe'
                    $worker = Start-Process -FilePath $workerPowerShell -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -Wait -PassThru
                    try {
                        if ($worker.ExitCode -ne 0) {
                            $failure = [InvalidOperationException]::new("The elevated uninstall did not finish (exit $($worker.ExitCode)). Remaining files and profiles were retained.")
                            $failure.Data['LauncherInteractiveMessageShown'] = [bool]$Interactive
                            throw $failure
                        }
                        $interactiveWorkerCompleted = [bool]$Interactive
                    } finally { $worker.Dispose() }
                }
                # Elevation does not change a same-account coordinator's profile.
                # Only the file worker must avoid Startup; an alternate-account
                # worker leaves this step to the initiating coordinator.
                if (!$FileOnly) { Remove-LauncherMatchingStartupShortcut $install }
                $shortcutWarning = $null
                if ($registrationScope -and (!$FileOnly -or $registrationScope.Scope -ceq 'AllUsers')) {
                    try {
                        $plan = Get-LauncherRegistrationPlan -Directory $install -Scope $registrationScope.Scope -UserSid $registrationScope.UserSid
                        Remove-LauncherStartMenuShortcuts (Get-LauncherRecoveryShortcutPlan $plan)
                    } catch {
                        $shortcutWarning = 'Some Launcher Start menu shortcuts were left in place. ' + $_.Exception.Message
                        Write-Warning $shortcutWarning
                    }
                }
                Write-Output 'Launcher program files removed. Personal profiles and edited or unrelated files were retained.'
                if ($Interactive -and !$interactiveWorkerCompleted) {
                    $message = 'Launcher program files removed.' + "`n`n" + 'Personal profiles and edited or unrelated files were retained.'
                    $warnings = @($removed | ForEach-Object { Get-LauncherDiscoveryProperty $_ 'CleanupWarnings' })
                    if ($shortcutWarning) { $warnings += $shortcutWarning }
                    if ($warnings.Count) { $message += "`n`n" + ($warnings -join "`n") }
                    Show-LauncherMaintenanceMessage -Message $message
                }
            }
        }
    } catch {
        $reported = $false
        for ($cause = $_.Exception; $cause; $cause = $cause.InnerException) {
            if ($Action -in @('QuickRollback', 'ArchiveRollback', 'RecoverArchiveRestore', 'CleanupAcceptedRelease') -and
                $cause -is [ComponentModel.Win32Exception] -and $cause.NativeErrorCode -eq 1223) {
                [Console]::Error.WriteLine('Launcher maintenance was canceled.')
                exit 2
            }
            if ($cause.Data['LauncherInteractiveMessageShown']) { $reported = $true }
        }
        if ($Interactive -and !$reported) {
            Show-LauncherMaintenanceMessage -Message ('Launcher could not finish uninstall.' + "`n`n" + $_.Exception.Message) -Failure
        }
        Write-Error $_
        exit 1
    }
}
