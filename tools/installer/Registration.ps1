#requires -Version 5.1
# Programs and Features registration. Current-user operations must remain in
# the initiating account; machine registration uses the elevated file worker.

function Get-LauncherRegistrationContext {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    try {
        [pscustomobject]@{
            UserSid = $identity.User.Value
            IsElevated = [Security.Principal.WindowsPrincipal]::new($identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
        }
    } finally { $identity.Dispose() }
}

function Assert-LauncherRegistrationContext {
    param([ValidateSet('AllUsers', 'CurrentUser')][string]$Scope, [string]$UserSid,
        $Context = (Get-LauncherRegistrationContext), [switch]$ReadOnly)
    if ($Scope -eq 'CurrentUser' -and (!$UserSid -or $UserSid -cne $Context.UserSid)) {
        throw 'Personal installation registration must run in the original user account; another administrator account cannot use its own HKCU.'
    }
    if ($Scope -eq 'AllUsers' -and !$ReadOnly -and !$Context.IsElevated) {
        $failure = [UnauthorizedAccessException]::new('Administrator approval is required to register or remove an all-users installation in Windows Programs and Features.')
        $failure.Data['LauncherPermissionDenied'] = $true
        throw $failure
    }
}

function Get-LauncherUninstallCommand([string]$Directory, [string]$Scope) {
    $setup = Join-Path $Directory 'Launcher Setup.exe'
    $entry = Get-Item -LiteralPath $setup -Force -ErrorAction SilentlyContinue
    if ($entry -and !$entry.PSIsContainer -and !($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        return '"{0}" --uninstall {1}' -f $setup, $Scope
    }
    $powershell = Join-Path ([Environment]::GetFolderPath('Windows')) 'System32\WindowsPowerShell\v1.0\powershell.exe'
    return '"{0}" -NoLogo -NoProfile -STA -ExecutionPolicy Bypass -File "{1}" -Action Uninstall -InstallDirectory "{2}" -Scope {3} -Interactive' -f
        $powershell, (Join-Path $Directory 'Maintenance.ps1'), $Directory, $Scope
}

function Get-LauncherRegistrationPlan {
    param([Parameter(Mandatory)][string]$Directory,
        [Parameter(Mandatory)][ValidateSet('AllUsers', 'CurrentUser')][string]$Scope,
        [string]$UserSid, [string]$Version = '', [int]$EstimatedSize = 0)
    $install = [IO.Path]::GetFullPath($Directory).TrimEnd('\', '/')
    if ($install -ieq [IO.Path]::GetPathRoot($install).TrimEnd('\', '/') -or $install.Contains('"') -or
        $install.Contains([string][char]0) -or $install.Contains("`r") -or $install.Contains("`n")) {
        throw 'Programs and Features requires an exact dedicated installation directory.'
    }
    if ($Scope -eq 'CurrentUser') { $null = [Security.Principal.SecurityIdentifier]::new($UserSid) }
    if ($Version) { $null = [version]::Parse($Version) }
    $publisher = 'D' + [char]0x0142 + 'ugosz'
    $values = [ordered]@{
        DisplayName = $publisher + ' Launcher'; DisplayVersion = $Version; Publisher = 'John ' + $publisher
        InstallLocation = $install; DisplayIcon = '"' + (Join-Path $install 'Launcher.exe') + '",0'
        UninstallString = Get-LauncherUninstallCommand $install $Scope
        NoModify = 1; NoRepair = 1; EstimatedSize = [Math]::Max(0, $EstimatedSize)
        InstallDate = [DateTime]::Now.ToString('yyyyMMdd')
        LauncherRegistrationSchema = 1; LauncherInstallScope = $Scope
        LauncherRegistrationIdentity = $Scope + '|' + $(if ($Scope -eq 'CurrentUser') { $UserSid } else { '' }) + '|' + $install
    }
    if ($Scope -eq 'CurrentUser') { $values.LauncherUserSid = $UserSid }
    [pscustomobject]@{
        Directory = $install; Scope = $Scope; UserSid = $UserSid
        SubKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\Dlugosz.Launcher'
        Values = $values
    }
}

function Assert-LauncherRegistrationOwned {
    param($Plan, $Snapshot)
    if (!$Snapshot.Exists) { return }
    $values = $Snapshot.Values
    if (!$values.Contains('LauncherRegistrationIdentity') -or
        [string]$values['LauncherRegistrationIdentity'].Value -ine $Plan.Values.LauncherRegistrationIdentity) {
        throw 'The Programs and Features entry belongs to another or unrecognized installation; it was left unchanged.'
    }
}

function Assert-LauncherQuickRollbackRegistration {
    param($Plan, $Snapshot, [string]$Version)
    Assert-LauncherRegistrationOwned $Plan $Snapshot
    if (!$Snapshot.Exists) { throw 'The registered Launcher installation no longer exists.' }
    $required = @('DisplayName', 'InstallLocation', 'LauncherRegistrationSchema', 'LauncherInstallScope')
    if ($Plan.Scope -ceq 'CurrentUser') { $required += 'LauncherUserSid' }
    foreach ($name in $required) {
        if (!$Snapshot.Values.Contains($name) -or $Snapshot.Values[$name].Value -ine $Plan.Values[$name]) {
            throw 'The installed Launcher registration no longer matches this scope and directory.'
        }
    }
    if (!$Snapshot.Values.Contains('DisplayVersion') -or [string]$Snapshot.Values['DisplayVersion'].Value -cne $Version) {
        throw 'The registered Launcher version changed before rollback could be verified.'
    }
}

function Invoke-LauncherRegistrationStore {
    param([ValidateSet('Read', 'AssertWritable', 'Write', 'Restore', 'Remove')][string]$Operation, $Plan, $Snapshot)
    Assert-LauncherRegistrationContext -Scope $Plan.Scope -UserSid $Plan.UserSid -ReadOnly:($Operation -eq 'Read')
    $hive = if ($Plan.Scope -eq 'AllUsers') { [Microsoft.Win32.RegistryHive]::LocalMachine } else { [Microsoft.Win32.RegistryHive]::CurrentUser }
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, [Microsoft.Win32.RegistryView]::Registry64)
    $key = $null
    try {
        $key = $base.OpenSubKey($Plan.SubKey, $Operation -ne 'Read')
        $before = [pscustomobject]@{ Exists = $null -ne $key; Values = [ordered]@{} }
        if ($key) {
            if ($key.SubKeyCount -ne 0) { throw 'The Launcher registration contains unexpected subkeys; it was left unchanged.' }
            foreach ($name in $key.GetValueNames()) {
                $before.Values[$name] = [pscustomobject]@{
                    Value = $key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                    Kind = $key.GetValueKind($name)
                }
            }
        }
        if (!($Operation -eq 'Restore' -and !$Snapshot.Exists -and $before.Values.Count -eq 0)) {
            Assert-LauncherRegistrationOwned $Plan $before
        }
        if ($Operation -eq 'Read') { return $before }
        if ($Operation -eq 'AssertWritable') {
            if (!$key) {
                $parentPath = 'Software\Microsoft\Windows\CurrentVersion\Uninstall'
                $parent = $null
                while ($parentPath -and !$parent) {
                    $parent = $base.OpenSubKey($parentPath, $true)
                    $parentPath = if ($parentPath.Contains('\')) { $parentPath.Substring(0, $parentPath.LastIndexOf('\')) } else { '' }
                }
                if (!$parent) { throw 'The Windows uninstall registration parent key is unavailable.' }
                $parent.Dispose()
            }
            return
        }
        if ($key) { $key.Dispose(); $key = $null }
        if ($Operation -eq 'Remove' -or ($Operation -eq 'Restore' -and !$Snapshot.Exists)) {
            $base.DeleteSubKey($Plan.SubKey, $false)
            return
        }
        if ($Operation -eq 'Restore') { $base.DeleteSubKey($Plan.SubKey, $false) }
        $key = $base.CreateSubKey($Plan.SubKey, $true)
        if ($Operation -eq 'Restore') {
            foreach ($name in $Snapshot.Values.Keys) { $key.SetValue($name, $Snapshot.Values[$name].Value, $Snapshot.Values[$name].Kind) }
        } else {
            # Establish ownership in one value before writing the remaining
            # metadata, so a partial update can safely restore its snapshot.
            $key.SetValue('LauncherRegistrationIdentity', $Plan.Values.LauncherRegistrationIdentity, [Microsoft.Win32.RegistryValueKind]::String)
            foreach ($name in $Plan.Values.Keys) {
                $kind = if ($Plan.Values[$name] -is [int]) { [Microsoft.Win32.RegistryValueKind]::DWord } else { [Microsoft.Win32.RegistryValueKind]::String }
                $key.SetValue($name, $Plan.Values[$name], $kind)
            }
        }
        $key.Flush()
    } finally {
        if ($key) { $key.Dispose() }
        $base.Dispose()
    }
}

function New-LauncherRegistrationTransaction {
    param([Parameter(Mandatory)]$Plan, $Context = (Get-LauncherRegistrationContext),
        [scriptblock]$Store = { param($Operation, $Registration, $Snapshot) Invoke-LauncherRegistrationStore $Operation $Registration $Snapshot })
    Assert-LauncherRegistrationContext -Scope $Plan.Scope -UserSid $Plan.UserSid -Context $Context
    try {
        & $Store 'AssertWritable' $Plan $null | Out-Null
        $snapshot = & $Store 'Read' $Plan $null
    } catch {
        for ($cause = $_.Exception; $cause; $cause = $cause.InnerException) {
            if ($cause -is [UnauthorizedAccessException]) { $_.Exception.Data['LauncherPermissionDenied'] = $true }
        }
        throw
    }
    Assert-LauncherRegistrationOwned $Plan $snapshot
    [pscustomobject]@{ Plan = $Plan; Snapshot = $snapshot; Store = $Store; Applied = $false }
}

function Set-LauncherRegistrationTransaction {
    param([Parameter(Mandatory)]$Transaction)
    # Activation may have installed Setup or restored a package that predates it.
    $plan = $Transaction.Plan
    $plan.Values.UninstallString = Get-LauncherUninstallCommand $plan.Directory $plan.Scope
    $Transaction.Applied = $true
    & $Transaction.Store 'Write' $Transaction.Plan $null | Out-Null
}

function Restore-LauncherRegistrationTransaction {
    param($Transaction)
    if ($Transaction -and $Transaction.Applied) {
        & $Transaction.Store 'Restore' $Transaction.Plan $Transaction.Snapshot | Out-Null
        $Transaction.Applied = $false
    }
}

function Remove-LauncherRegistrationTransaction {
    param([Parameter(Mandatory)]$Transaction)
    $Transaction.Applied = $true
    & $Transaction.Store 'Remove' $Transaction.Plan $null | Out-Null
}
