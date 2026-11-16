#requires -Version 5.1
[CmdletBinding()]
param([string]$InstallerDirectory, [string]$PackageDirectory,
    [string]$OutputDirectory = [IO.Path]::GetTempPath())
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$InstallerDirectory) { $InstallerDirectory = Join-Path $PSScriptRoot '..\tools\installer' }
. (Join-Path $InstallerDirectory 'Maintenance.ps1')
# This test must never use the native registry, even after a fixture mistake.
function Invoke-LauncherRegistrationStore { throw 'Native registration is disabled in this test.' }
$script:checks = 0
function Check([bool]$Condition, [string]$Message) { $script:checks++; if (!$Condition) { throw $Message } }
function Fails([scriptblock]$Action, [string]$Message) { $failed = $false; try { & $Action | Out-Null } catch { $failed = $true }; Check $failed $Message }
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function New-RegistryFixture {
    $state = [pscustomobject]@{
        Snapshot = [pscustomobject]@{ Exists = $false; Values = [ordered]@{} }
        Events = [Collections.Generic.List[string]]::new(); FailOperation = ''; PartialWrite = $false
    }
    $store = {
        param($Operation, $Plan, $Snapshot)
        $state.Events.Add($Operation + ':' + $Plan.Scope)
        if ($Operation -eq $state.FailOperation) { throw [UnauthorizedAccessException]::new('Owned registry fixture denied ' + $Operation) }
        switch ($Operation) {
            'Read' { return $state.Snapshot }
            'Write' {
                $values = [ordered]@{}
                $values.LauncherRegistrationIdentity = [pscustomobject]@{ Value = $Plan.Values.LauncherRegistrationIdentity; Kind = 'String' }
                $state.Snapshot = [pscustomobject]@{ Exists = $true; Values = $values }
                if ($state.PartialWrite) { throw 'Owned partial registry write.' }
                foreach ($name in $Plan.Values.Keys) { $values[$name] = [pscustomobject]@{ Value = $Plan.Values[$name]; Kind = $(if ($Plan.Values[$name] -is [int]) { 'DWord' } else { 'String' }) } }
            }
            'Restore' { $state.Snapshot = $Snapshot }
            'Remove' { $state.Snapshot = [pscustomobject]@{ Exists = $false; Values = [ordered]@{} } }
        }
    }.GetNewClosure()
    [pscustomobject]@{ State = $state; Store = $store }
}
$root = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('launcher-registration-' + [Guid]::NewGuid().ToString('N'))
$directory = Join-Path $root 'Local Programs %PATH%\Launcher'
[void][IO.Directory]::CreateDirectory($directory)
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$ordinary = [pscustomobject]@{ UserSid = $sid; IsElevated = $false }
$admin = [pscustomobject]@{ UserSid = 'S-1-5-21-9-8-7-1001'; IsElevated = $true }
$personal = Get-LauncherRegistrationPlan -Directory $directory -Scope CurrentUser -UserSid $sid -Version '1.3.0.4' -EstimatedSize 1234
$machine = Get-LauncherRegistrationPlan -Directory $directory -Scope AllUsers -UserSid $sid -Version '1.4'
Check ($personal.SubKey -eq 'Software\Microsoft\Windows\CurrentVersion\Uninstall\Dlugosz.Launcher') 'Registration key is not stable across upgrades.'
Check ($personal.Values.DisplayVersion -ceq '1.3.0.4' -and $personal.Values.EstimatedSize -eq 1234) 'Programs and Features lost the packaged version or size.'
Check ($personal.Values.NoModify -is [int] -and $personal.Values.NoModify -eq 1 -and $personal.Values.NoRepair -eq 1) 'Modify and Repair must be disabled with DWORD values.'
Check ($personal.Values.DisplayIcon -ceq ('"' + (Join-Path $directory 'Launcher.exe') + '",0')) 'The display icon must use the stable executable alias.'
Check ($personal.Values.UninstallString.Contains('WindowsPowerShell\v1.0\powershell.exe"') -and
    $personal.Values.UninstallString.Contains(' -NoProfile -STA ') -and
    $personal.Values.UninstallString.Contains('-File "' + (Join-Path $directory 'Maintenance.ps1') + '"') -and
    $personal.Values.UninstallString.Contains('-InstallDirectory "' + $directory + '" -Scope CurrentUser -Interactive') -and
    !$personal.Values.UninstallString.Contains('cmd.exe')) 'Uninstall does not quote the exact percent-containing path without cmd expansion.'
Check ($machine.Values.UninstallString.EndsWith('-Scope AllUsers -Interactive')) 'The machine entry lost its uninstall scope or interactive feedback.'
$menu = Join-Path $root 'Start menu\Programs'
$programRoot = Join-Path $root 'Local Programs'
[void][IO.Directory]::CreateDirectory($menu)
$recovery = Get-LauncherRecoveryShortcutPlan $personal -ProgramsFolder $menu -ProgramRoot $programRoot
$launch = Get-LauncherStartMenuLaunchPlan $recovery
$oldRecovery = Get-LauncherLegacyRecoveryShortcutPlan $recovery
$publisher = 'D' + [char]0x0142 + 'ugosz'
$vendor = Join-Path $menu $publisher
[void][IO.Directory]::CreateDirectory($vendor)
$otherProduct = Join-Path $vendor 'WindowRecall.lnk'
[IO.File]::WriteAllText($otherProduct, 'Unrelated publisher shortcut fixture')
$flatRecovery = Join-Path $menu 'Launcher Update History and Recovery.lnk'
[IO.File]::WriteAllText($flatRecovery, 'Legacy MSI-owned link fixture')
$active = Join-Path $directory 'Launcher.exe'
[IO.File]::WriteAllText($active, 'Disposable Launcher executable fixture')
$icon = (Join-Path $directory 'Launcher-R1.1.exe') + ',-20000'
Set-LauncherStartMenuShortcut $oldRecovery $icon
Set-LauncherStartMenuShortcuts $recovery $icon
Check ($launch.Path -ceq (Join-Path $vendor 'Launcher.lnk') -and
    $recovery.Path -ceq (Join-Path $vendor 'Launcher Update History and Recovery.lnk') -and
    $oldRecovery.Path -ceq (Join-Path $menu ($publisher + ' Launcher\Launcher Update History and Recovery.lnk'))) `
    'Setup must place both Launcher shortcuts in the shared publisher folder and recognize the prior recovery path.'
$launcherLink = Read-LauncherStartMenuShortcut $launch.Path
Check ($launcherLink.TargetPath -ceq $active -and $launcherLink.WorkingDirectory -ceq $directory -and
    !$launcherLink.Arguments -and $launcherLink.IconLocation -ceq ($active + ',0')) `
    'The Start menu launch link must use the stable Launcher alias, normal icon, and default launch arguments.'
Check (![IO.File]::Exists($oldRecovery.Path) -and
    [IO.File]::ReadAllText($otherProduct) -ceq 'Unrelated publisher shortcut fixture') `
    'Setup must migrate the owned recovery link without touching another product in the publisher folder.'
$link = Read-LauncherStartMenuShortcut $recovery.Path
Check ($link.Arguments -ceq 'launcher --recovery --scope user' -and $link.TargetPath -ceq $recovery.TargetPath -and
    $link.WorkingDirectory -ceq [IO.Path]::GetDirectoryName($link.TargetPath) -and $link.IconLocation -ceq $icon -and
    ![IO.File]::Exists($link.TargetPath)) `
    'The personal shortcut must retain its exact scope, icon and standard Updater target before Updater is installed.'
$nextIcon = (Join-Path $directory 'Launcher-R1.2.exe') + ',-20000'
Set-LauncherStartMenuShortcuts $recovery $nextIcon
Check ((Read-LauncherStartMenuShortcut $recovery.Path).IconLocation -ceq $nextIcon) 'Upgrade did not refresh its own recovery icon.'
$otherInstall = $recovery.PSObject.Copy()
$otherInstall.InstallDirectory = Join-Path $root 'another installation'
Fails { Remove-LauncherStartMenuShortcut $otherInstall } 'Another installation in the same scope claimed the link.'
[IO.File]::Delete($active)
Remove-LauncherStartMenuShortcuts $recovery
Check (![IO.File]::Exists($launch.Path) -and ![IO.File]::Exists($recovery.Path) -and
    [IO.File]::ReadAllText($flatRecovery) -ceq 'Legacy MSI-owned link fixture' -and
    [IO.File]::ReadAllText($otherProduct) -ceq 'Unrelated publisher shortcut fixture') `
    'Removing Launcher links must preserve the legacy Updater MSI link and unrelated publisher shortcuts.'
$foreign = $recovery.PSObject.Copy()
$foreign.Description = 'Edited shortcut'
Set-LauncherStartMenuShortcut $foreign $icon
$foreignHash = Hash $foreign.Path
Fails { Set-LauncherStartMenuShortcut $recovery $nextIcon } 'Setup overwrote a customized recovery shortcut.'
Fails { Remove-LauncherStartMenuShortcut $recovery } 'Uninstall removed a customized recovery shortcut.'
Check ((Hash $foreign.Path) -ceq $foreignHash) 'A shortcut collision changed the existing file.'

# A collision in the old location leaves the edited link available while the
# independent launch and recovery entries are still created in the publisher folder.
Remove-LauncherStartMenuShortcut $foreign
[IO.File]::WriteAllText($active, 'Disposable Launcher executable fixture')
$editedLegacy = $oldRecovery.PSObject.Copy()
$editedLegacy.Description = 'Edited legacy shortcut'
Set-LauncherStartMenuShortcut $editedLegacy $icon
$editedLegacyHash = Hash $editedLegacy.Path
Fails { Set-LauncherStartMenuShortcuts $recovery $nextIcon } 'Setup did not report the legacy shortcut collision.'
Check ([IO.File]::Exists($launch.Path) -and (Read-LauncherStartMenuShortcut $recovery.Path).IconLocation -ceq $nextIcon -and
    (Hash $editedLegacy.Path) -ceq $editedLegacyHash) 'Setup did not preserve the edited legacy link while creating its own links.'
Fails { Remove-LauncherStartMenuShortcuts $recovery } 'Uninstall did not report the legacy shortcut collision.'
Check (![IO.File]::Exists($launch.Path) -and ![IO.File]::Exists($recovery.Path) -and
    (Hash $editedLegacy.Path) -ceq $editedLegacyHash -and
    [IO.File]::ReadAllText($otherProduct) -ceq 'Unrelated publisher shortcut fixture') `
    'A legacy collision must leave other owned shortcuts removable and unrelated shortcuts untouched.'

$fixture = New-RegistryFixture
Fails { New-LauncherRegistrationTransaction -Plan $machine -Context $ordinary -Store $fixture.Store } 'Writable files incorrectly bypassed HKLM elevation.'
Check ($fixture.State.Events.Count -eq 0) 'Machine permission failure reached registry mutation.'
Fails { New-LauncherRegistrationTransaction -Plan $personal -Context $admin -Store $fixture.Store } 'An alternate administrator account was allowed to use its own HKCU.'
Check ($fixture.State.Events.Count -eq 0) 'Personal account mismatch reached a registry backend.'
$transaction = New-LauncherRegistrationTransaction -Plan $personal -Context $ordinary -Store $fixture.Store
Check (!$fixture.State.Snapshot.Exists -and ($fixture.State.Events -join ',') -eq 'AssertWritable:CurrentUser,Read:CurrentUser') 'Registration preflight changed the registry.'
Set-LauncherRegistrationTransaction $transaction
Check ($fixture.State.Snapshot.Exists -and $fixture.State.Snapshot.Values.DisplayVersion.Value -eq '1.3.0.4') 'Personal registration failed to save metadata.'
Restore-LauncherRegistrationTransaction $transaction
Check (!$fixture.State.Snapshot.Exists) 'Failed fresh activation did not restore absent registration.'
$seed = New-LauncherRegistrationTransaction -Plan $personal -Context $ordinary -Store $fixture.Store
Set-LauncherRegistrationTransaction $seed
$next = Get-LauncherRegistrationPlan -Directory $directory -Scope CurrentUser -UserSid $sid -Version '1.4'
$upgrade = New-LauncherRegistrationTransaction -Plan $next -Context $ordinary -Store $fixture.Store
Set-LauncherRegistrationTransaction $upgrade
Check ($fixture.State.Snapshot.Values.DisplayVersion.Value -eq '1.4') 'Upgrade did not refresh DisplayVersion.'
Restore-LauncherRegistrationTransaction $upgrade
Check ($fixture.State.Snapshot.Values.DisplayVersion.Value -eq '1.3.0.4') 'A later failure did not restore previous registration.'
$partial = New-LauncherRegistrationTransaction -Plan $next -Context $ordinary -Store $fixture.Store
$fixture.State.PartialWrite = $true
Fails { Set-LauncherRegistrationTransaction $partial } 'A partial registry write was treated as successful.'
Restore-LauncherRegistrationTransaction $partial
$fixture.State.PartialWrite = $false
Check ($fixture.State.Snapshot.Values.DisplayVersion.Value -eq '1.3.0.4') 'Partial registration recovery lost the earlier entry.'
$other = Get-LauncherRegistrationPlan -Directory (Join-Path $root 'another install') -Scope CurrentUser -UserSid $sid -Version '1.4'
Fails { New-LauncherRegistrationTransaction -Plan $other -Context $ordinary -Store $fixture.Store } 'Registration overwrote another installation in the same scope.'
$machineFixture = New-RegistryFixture
$machineTx = New-LauncherRegistrationTransaction -Plan $machine -Context $admin -Store $machineFixture.Store
Set-LauncherRegistrationTransaction $machineTx
Check ($machineFixture.State.Snapshot.Exists -and $machineFixture.State.Events.Contains('Write:AllUsers') -and
    !$machineFixture.State.Events.Contains('Write:CurrentUser')) 'Machine registration fell back to administrator HKCU.'

$manifestPath = Join-Path $directory 'Launcher.install-manifest.json'
[IO.File]::WriteAllText($manifestPath, ([pscustomobject]@{ InstallScope = 'CurrentUser'; InstallUserSid = $sid } | ConvertTo-Json))
$savedRegistrationScope = Get-LauncherMaintenanceRegistrationScope -Directory $directory
Check ($savedRegistrationScope.Scope -ceq 'CurrentUser' -and $savedRegistrationScope.UserSid -ceq $sid) 'Maintenance lost the original hive/account.'
Fails { Get-LauncherMaintenanceRegistrationScope -Directory $directory -Scope AllUsers } 'Maintenance accepted a mismatched requested scope.'
$fixture.State.Events.Clear()
Fails { Invoke-LauncherRegisteredMaintenance -Action Uninstall -Directory $directory -RegistrationScope $savedRegistrationScope -Context $ordinary -Store $fixture.Store `
    -RemoveFiles { throw 'Owned locked payload fixture.' } } 'A failed file uninstall was treated as complete.'
Check ($fixture.State.Snapshot.Exists -and !$fixture.State.Events.Contains('Remove:CurrentUser')) 'A failed file removal deleted the Programs and Features entry.'
$fixture.State.FailOperation = 'Remove'
Fails { Invoke-LauncherRegisteredMaintenance -Action Uninstall -Directory $directory -RegistrationScope $savedRegistrationScope -Context $ordinary -Store $fixture.Store `
    -RemoveFiles { param($Install, $AfterFilesRemoved) & $AfterFilesRemoved } } 'A denied registry deletion was hidden.'
Check ($fixture.State.Snapshot.Exists -and $fixture.State.Snapshot.Values.DisplayVersion.Value -eq '1.3.0.4') 'A failed registry removal lost its retryable entry.'
$fixture.State.FailOperation = ''
$fixture.State.Events.Clear()
$remove = { param($Install, $AfterFilesRemoved) $fixture.State.Events.Add('files'); & $AfterFilesRemoved; $fixture.State.Events.Add('helpers'); 'removed' }.GetNewClosure()
$null = Invoke-LauncherRegisteredMaintenance -Action Uninstall -Directory $directory -RegistrationScope $savedRegistrationScope -Context $ordinary -Store $fixture.Store -RemoveFiles $remove
Check (!$fixture.State.Snapshot.Exists -and ($fixture.State.Events -join ',') -eq 'AssertWritable:CurrentUser,Read:CurrentUser,files,Remove:CurrentUser,helpers') 'Successful uninstall did not remove the matching entry before bootstrap cleanup.'

# A real file-only fixture proves the removal callback runs while all bootstrap
# files and its receipt remain available; registry failure can be retried.
$bootstrap = Join-Path $root 'bootstrap fixture'
[void][IO.Directory]::CreateDirectory($bootstrap)
$owned = @()
foreach ($name in @('LICENSE.txt', 'Maintenance.ps1', 'Registration.ps1', 'Uninstall.cmd')) {
    $path = Join-Path $bootstrap $name
    [IO.File]::WriteAllText($path, 'owned fixture ' + $name)
    $owned += [pscustomobject]@{ RelativePath = $name; Sha256 = Hash $path }
}
$receipt = Join-Path $bootstrap 'Launcher.install-manifest.json'
[IO.File]::WriteAllText($receipt, ([pscustomobject]@{ SchemaVersion = 1; Files = $owned } | ConvertTo-Json -Depth 5))
Fails { Remove-LauncherInstallationFiles -Directory $bootstrap -ProcessReader { @() } -AfterFilesRemoved {
    Check (Test-Path -LiteralPath (Join-Path $bootstrap 'Maintenance.ps1')) 'Bootstrap was deleted before registration removal.'
    Check (Test-Path -LiteralPath $receipt) 'The ownership receipt disappeared before registration removal.'
    throw 'Owned failed registry removal.'
} } 'A failed registration callback was ignored.'
Check (!(Test-Path -LiteralPath (Join-Path $bootstrap 'LICENSE.txt')) -and
    (Test-Path -LiteralPath (Join-Path $bootstrap 'Registration.ps1'))) 'Uninstall did not retain retryable bootstrap files after callback failure.'
$null = Remove-LauncherInstallationFiles -Directory $bootstrap -ProcessReader { @() }
Check (!(Test-Path -LiteralPath $bootstrap)) 'A successful retry left owned bootstrap files behind.'

foreach ($blockedOperation in @('AssertWritable', 'Read')) {
    $blocked = New-RegistryFixture
    $blocked.State.FailOperation = $blockedOperation
    $failure = $null
    try { New-LauncherRegistrationTransaction -Plan $personal -Context $ordinary -Store $blocked.Store | Out-Null } catch { $failure = $_ }
    Check ($null -ne $failure -and $failure.Exception.Data['LauncherPermissionDenied'] -eq $true -and !$blocked.State.Snapshot.Exists) 'A registration preflight denial lost its permission marker or mutated registry storage.'
}

if ($PackageDirectory) {
    $realInstall = Join-Path $root 'registered payload'
    $installed = Install-LauncherPayload -PackageDirectory $PackageDirectory -InstallDirectory $realInstall -InstallScope CurrentUser -InstallUserSid $sid
    $originalHash = Hash $installed.ActiveExecutable
    $retained = Join-Path $realInstall ('Launcher.20261023-120000.' + [Guid]::NewGuid().ToString('N') + '.backup.exe')
    [IO.File]::Copy($installed.VersionedExecutable, $retained, $false)
    [IO.File]::AppendAllText($retained, 'owned retained rollback variant')
    $retainedHash = Hash $retained
    $integration = New-RegistryFixture
    $integrationPlan = Get-LauncherRegistrationPlan -Directory $realInstall -Scope CurrentUser -UserSid $sid -Version '9.9'
    $seed = New-LauncherRegistrationTransaction -Plan $integrationPlan -Context $ordinary -Store $integration.Store
    Set-LauncherRegistrationTransaction $seed
    $setup = Join-Path $realInstall 'Launcher Setup.exe'
    if (Test-Path -LiteralPath $setup -PathType Leaf) {
        Check ($integration.State.Snapshot.Values.UninstallString.Value -ceq ('"{0}" --uninstall CurrentUser' -f $setup)) 'A package with Launcher Setup did not register its native uninstall entry.'
    }
    $installedScope = Get-LauncherMaintenanceRegistrationScope -Directory $realInstall
    $integration.State.PartialWrite = $true
    Fails { Invoke-LauncherRegisteredMaintenance -Action Rollback -Directory $realInstall -RegistrationScope $installedScope `
        -ExecutableName ([IO.Path]::GetFileName($retained)) -Context $ordinary -Store $integration.Store } 'Registered rollback ignored failed registry publication.'
    Check ((Hash $installed.ActiveExecutable) -eq $originalHash -and (Hash $retained) -eq $retainedHash -and
        $integration.State.Snapshot.Values.DisplayVersion.Value -eq '9.9') 'Failed registered rollback did not restore both alias bytes and Programs and Features metadata.'
    $integration.State.PartialWrite = $false
    $null = Invoke-LauncherRegisteredMaintenance -Action Rollback -Directory $realInstall -RegistrationScope $installedScope `
        -ExecutableName ([IO.Path]::GetFileName($retained)) -Context $ordinary -Store $integration.Store
    $actualVersion = Get-LauncherVersionLabel ([Diagnostics.FileVersionInfo]::GetVersionInfo($retained))
    Check ((Hash $installed.ActiveExecutable) -eq $retainedHash -and $integration.State.Snapshot.Values.DisplayVersion.Value -eq $actualVersion) 'Registered rollback did not publish the selected executable version to Programs and Features.'
    $readme = Join-Path $realInstall 'README.txt'
    [IO.File]::WriteAllText($readme, 'owned pre-repair documentation bytes')
    $oldReadme = Hash $readme
    $integration.State.Snapshot.Values.DisplayVersion.Value = '8.8'
    $integration.State.PartialWrite = $true
    Fails { Invoke-LauncherRegisteredMaintenance -Action Repair -Directory $realInstall -RegistrationScope $installedScope `
        -SourceDirectory $PackageDirectory -Context $ordinary -Store $integration.Store } 'Registered repair ignored failed registry publication.'
    Check ((Hash $installed.ActiveExecutable) -eq $retainedHash -and (Hash $readme) -eq $oldReadme -and
        $integration.State.Snapshot.Values.DisplayVersion.Value -eq '8.8') 'Failed registered repair did not restore payload, alias and Programs and Features metadata.'
    $integration.State.PartialWrite = $false
    $null = Invoke-LauncherRegisteredMaintenance -Action Repair -Directory $realInstall -RegistrationScope $installedScope `
        -SourceDirectory $PackageDirectory -Context $ordinary -Store $integration.Store
    $sourcePayload = Get-LauncherPayload $PackageDirectory
    Check ((Hash $installed.ActiveExecutable) -eq $originalHash -and
        (Hash $readme) -eq (Hash (Join-Path $PackageDirectory 'README.txt')) -and
        $integration.State.Snapshot.Values.DisplayVersion.Value -eq $sourcePayload.VersionLabel) 'Registered repair did not refresh its executable, documentation and displayed version.'
    $repairedScope = Get-LauncherMaintenanceRegistrationScope -Directory $realInstall
    Check ($repairedScope.Scope -eq 'CurrentUser' -and $repairedScope.UserSid -eq $sid) 'Registered repair lost its original account/hive context.'
}
Write-Output "Installer registration passed: $script:checks checks; native HKLM/HKCU were never accessed. Fixtures: $root"
