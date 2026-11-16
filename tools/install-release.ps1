[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $SourceDirectory,
    [string] $InstallDirectory = (Join-Path $env:ProgramFiles ('D' + [char]0x0142 + 'ugosz\Launcher')),
    [switch] $KeepActive,
    [switch] $StartOnLogin,
    [switch] $NoStart
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'installed-launcher.ps1')
. (Join-Path $PSScriptRoot 'installer\Discovery.ps1')
$install = [IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\')
$source = [IO.Path]::GetFullPath($SourceDirectory)
Assert-LauncherDirectory $install
$examples = Join-Path $install 'examples'
if (Get-LauncherEntry $examples) { Assert-LauncherDirectory $examples }
$sourceExe = Find-LauncherSourceExecutable $source
Assert-LauncherNormalBuild $sourceExe
$label = Get-LauncherReleaseLabel $sourceExe
$release = Join-Path $install "Launcher-R$label.exe"
$hash = Copy-LauncherImmutable $sourceExe $release
$active = Join-Path $install 'Launcher.exe'
$activeHash = $null
if (Test-Path -LiteralPath $active -PathType Leaf) {
    $activeHash = (Get-FileHash -LiteralPath $active -Algorithm SHA256).Hash
}
$activate = !$KeepActive -or !(Get-LauncherEntry $active)
$incomingVersion = [version]$label
$cleanupCandidates = @()
$isFinal = [Diagnostics.FileVersionInfo]::GetVersionInfo($sourceExe).Comments -ceq 'Launcher distribution: Release'
if ($activate -and $isFinal) {
    $cleanupCandidates = @(Get-LauncherSupersededExecutables -Directory $install -IncomingVersion $incomingVersion)
}
$prior = $null
$transaction = Join-Path $install ('.launcher-install-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($transaction)
$payload = [Collections.Generic.List[object]]::new()
$stage = $null
$activated = $false
$committed = $false
$createdDirectories = [Collections.Generic.List[string]]::new()
try {
    # Verify every selected supporting file before changing installed entries.
    # Existing editable palettes keep their current contents.
    $supportNames = @('Launcher.default.toml', 'LICENSE.txt', 'THIRD-PARTY-NOTICES.txt', 'README.txt', 'examples\basic_colors.toml', 'examples\extra_colors.toml', 'examples\colors-light.toml', 'examples\colors-dark.toml') + @(Get-LauncherOptionalCompanionFiles $source)
    foreach ($name in $supportNames) {
        $file = Join-Path $source $name
        $destination = Join-Path $install $name
        if (!(Test-Path -LiteralPath $file -PathType Leaf) -or
            ((Test-Path -LiteralPath $destination) -and !($activate -and !$name.StartsWith('examples\')))) { continue }
        if ($name -eq 'LauncherSvgExtractor.exe' -and (Get-LauncherEntry $destination)) { Assert-LauncherBundledExtractor $destination }
        # Fixed payload basenames are unique, so transaction entries stay flat.
        $entry = [pscustomobject]@{
            Destination = $destination
            Stage = Join-Path $transaction ([IO.Path]::GetFileName($name))
            Backup = Join-Path $transaction ([IO.Path]::GetFileName($name) + '.previous')
            Hash = $null
            BackedUp = $false
            Published = $false
        }
        $payload.Add($entry)
        $entry.Hash = Copy-LauncherImmutable $file $entry.Stage
    }
    if ($activate) {
        $stage = Join-Path $install ('.launcher-install-' + [Guid]::NewGuid().ToString('N') + '.staging.exe')
        # Keep the staged link beside its eventual name so its relative target
        # resolves correctly both before and after activation.
        New-LauncherReleaseLink $stage "Launcher-R$label.exe"
        if ((Get-FileHash -LiteralPath $stage).Hash -ne $hash) { throw 'Staged release link failed verification.' }
        Stop-LauncherExecutables @($active, $release, (Join-Path $install 'Launcher-WIP.exe'))
    }
    foreach ($entry in $payload) {
        $parent = [IO.Path]::GetDirectoryName($entry.Destination)
        if ($parent -ne $install) {
            if (!(Get-LauncherEntry $parent)) {
                [void][IO.Directory]::CreateDirectory($parent)
                $createdDirectories.Add($parent)
            }
            Assert-LauncherDirectory $parent
        }
        $existing = Get-LauncherEntry $entry.Destination
        if ($existing) {
            if ($existing.PSIsContainer -or ($existing.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw "Installed supporting file must be a regular file: $($entry.Destination)"
            }
            Move-Item -LiteralPath $entry.Destination -Destination $entry.Backup -ErrorAction Stop
            $entry.BackedUp = $true
        }
        Move-Item -LiteralPath $entry.Stage -Destination $entry.Destination -ErrorAction Stop
        $entry.Published = $true
        if ((Get-FileHash -LiteralPath $entry.Destination).Hash -ne $entry.Hash) {
            throw "Installed file failed verification: $($entry.Destination)"
        }
    }
    if ((Get-FileHash -LiteralPath $release).Hash -ne $hash) { throw 'Preserved release changed during installation.' }
    # Activation is the final payload change. Until it succeeds, all supporting
    # entries retain backups so a later failure can restore the old installation.
    if ($activate) {
        $prior = Set-LauncherActiveEntry $install $stage
        $activated = $true
        if ((Get-FileHash -LiteralPath $active).Hash -ne $hash) { throw 'Active release failed verification.' }
    }
    $committed = $true
} catch {
    $failure = $_
    $rollbackErrors = [Collections.Generic.List[string]]::new()
    if ($activated) {
        try {
            Remove-Item -LiteralPath $active -Force -ErrorAction Stop
            if ($prior) { Move-Item -LiteralPath $prior -Destination $active -ErrorAction Stop }
        } catch { $rollbackErrors.Add($_.Exception.Message) }
    }
    for ($index = $payload.Count - 1; $index -ge 0; --$index) {
        $entry = $payload[$index]
        try {
            if ($entry.Published) { Remove-Item -LiteralPath $entry.Destination -Force -ErrorAction Stop }
            if ($entry.BackedUp) { Move-Item -LiteralPath $entry.Backup -Destination $entry.Destination -ErrorAction Stop }
        } catch { $rollbackErrors.Add($_.Exception.Message) }
    }
    for ($index = $createdDirectories.Count - 1; $index -ge 0; --$index) {
        try { [IO.Directory]::Delete($createdDirectories[$index]) }
        catch { $rollbackErrors.Add($_.Exception.Message) }
    }
    if ($rollbackErrors.Count) {
        throw "Installation failed: $($failure.Exception.Message) Rollback also failed: $($rollbackErrors -join '; '). Recovery files retained in $transaction."
    }
    throw $failure
} finally {
    # Remove only the entries created by this attempt. If rollback could not
    # restore a backup, leave it available for recovery instead of deleting it.
    $cleanup = @($payload | ForEach-Object { $_.Stage; if ($committed) { $_.Backup } })
    if ($stage) { $cleanup += $stage }
    foreach ($path in $cleanup) {
        if (Get-LauncherEntry $path) {
            try { Remove-Item -LiteralPath $path -Force -ErrorAction Stop }
            catch { Write-Warning "Could not remove installation staging entry '$path': $($_.Exception.Message)" }
        }
    }
    try { [IO.Directory]::Delete($transaction) }
    catch { Write-Warning "Installation staging directory retained: $transaction" }
}
if ($activate -and $committed -and $isFinal) {
    # A final release removes all lower versions in this installation, including
    # recognized backups. Recheck metadata and bytes after successful activation.
    try {
        $currentCandidates = @(Get-LauncherSupersededExecutables -Directory $install -IncomingVersion $incomingVersion)
        if ($prior -and $activeHash) {
            # Set-LauncherActiveEntry chooses this name at activation. Its bytes were
            # snapshotted under the old active name before any installation changes.
            $cleanupCandidates += @($currentCandidates | Where-Object { $_.Path -ieq $prior -and $_.Sha256 -eq $activeHash })
        }
        foreach ($candidate in $cleanupCandidates) {
            try {
                $verified = @(Get-LauncherSupersededExecutables -Directory $install -IncomingVersion $incomingVersion |
                    Where-Object { $_.Path -ieq $candidate.Path -and $_.Sha256 -eq $candidate.Sha256 })
                if ($verified.Count -ne 1) { throw 'Executable contents or identity changed after installation began.' }
                [IO.File]::Delete($candidate.Path)
            }
            catch { Write-Warning "The release is active; old executable retained '$($candidate.Path)': $($_.Exception.Message)" }
        }
        if ($prior -and !(Get-LauncherEntry $prior)) { $prior = $null }
    } catch {
        Write-Warning "The release is active; older executable cleanup could not be verified: $($_.Exception.Message)"
    }
}
if ($StartOnLogin) { & (Join-Path $PSScriptRoot 'install-startup-shortcut.ps1') -InstallDirectory $install }
[pscustomobject]@{ ActiveExecutable = $active; ReleaseExecutable = $release; PreviousExecutable = $prior; Sha256 = $hash }
if (!$NoStart) { & (Join-Path $PSScriptRoot 'start-desktop-launcher.ps1') -ExecutablePath $active -WorkingDirectory $install }
