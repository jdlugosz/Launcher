# Shared file operations for release installation and development acceptance.

. (Join-Path $PSScriptRoot 'installer\Discovery.ps1')

function Assert-LauncherDirectory([string] $Path) {
    if ($Path.TrimEnd('\') -eq [IO.Path]::GetPathRoot($Path).TrimEnd('\')) {
        throw 'Use a dedicated Launcher directory, not a drive root.'
    }
    $entry = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if (!$entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Launcher directory must be a real directory: $Path"
    }
}

function Get-LauncherEntry([string] $Path) {
    Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
}

function Assert-LauncherNormalBuild([string] $Executable) {
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($Executable)
    # Build provenance sets VS_FF_SPECIALBUILD and SpecialBuild for scenario,
    # coverage and other instrumentation. Inspect the resource, not the name:
    # renaming a test executable must not make it eligible for installation.
    if ($version.IsSpecialBuild -or $version.IsPrivateBuild -or
        ![string]::IsNullOrWhiteSpace($version.SpecialBuild) -or
        ![string]::IsNullOrWhiteSpace($version.PrivateBuild)) {
        throw "Instrumented or special Launcher builds cannot be installed or accepted for normal use: $Executable"
    }
}

function New-LauncherReleaseLink([string] $Path, [string] $Target) {
    # Hard links need no symbolic-link privilege. Always create a new entry;
    # copying over an existing alias could overwrite its retained target.
    if (Get-LauncherEntry $Path) { throw "The alias path already exists: $Path" }
    $targetPath = if ([IO.Path]::IsPathRooted($Target)) { $Target } else { Join-Path ([IO.Path]::GetDirectoryName($Path)) $Target }
    $targetEntry = Get-Item -LiteralPath $targetPath -Force -ErrorAction Stop
    if ($targetEntry.PSIsContainer -or ($targetEntry.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Alias target must be a regular executable.' }
    $hash = (Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash
    if (!('LauncherInstall.Native' -as [type])) {
        Add-Type @'
using System.Runtime.InteropServices;
namespace LauncherInstall {
    public static class Native {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool CreateHardLinkW(string path, string target, System.IntPtr reserved);
    }
}
'@
    }
    if (![LauncherInstall.Native]::CreateHardLinkW($Path, $targetPath, [IntPtr]::Zero)) {
        [IO.File]::Copy($targetPath, $Path, $false)
    }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $hash -or
        (Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash -ne $hash) { throw 'Active alias failed SHA-256 verification.' }
}

function Get-LauncherExecutableVersionLabel([string] $Executable) {
    Assert-LauncherNormalBuild $Executable
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($Executable)
    if ($version.IsDebug) { throw "A normal optimized executable is required: $Executable" }
    return Get-LauncherVersionLabel $version
}

function Test-LauncherFinalRelease([string] $Executable) {
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($Executable)
    if ($version.Comments -match '^Launcher distribution: (Release|Prerelease|Special)$') {
        return $version.Comments -ceq 'Launcher distribution: Release'
    }
    # Releases installed before explicit distribution metadata remain usable.
    return !$version.IsDebug -and !$version.IsPreRelease -and ($version.ProductMinorPart % 2 -eq 0)
}

function Get-LauncherReleaseLabel([string] $Executable) {
    $label = Get-LauncherExecutableVersionLabel $Executable
    if (!(Test-LauncherFinalRelease $Executable)) { throw "A release executable is required: $Executable" }
    return $label
}

function Find-LauncherSourceExecutable([string] $Directory) {
    $normal = Join-Path $Directory 'Launcher.exe'
    if (Test-Path -LiteralPath $normal -PathType Leaf) { return $normal }
    $files = @(Get-ChildItem -LiteralPath $Directory -Filter 'Launcher-R*.exe' -File | Where-Object { $_.Name -match '^Launcher-R\d+\.\d+(?:\.\d+){0,2}\.exe$' })
    if ($files.Count -ne 1) { throw "Expected one Launcher executable in $Directory" }
    return $files[0].FullName
}

function Assert-LauncherBundledExtractor([string]$Executable) {
    Assert-LauncherNormalBuild $Executable
    if ([Diagnostics.FileVersionInfo]::GetVersionInfo($Executable).IsDebug) { throw "A normal optimized SVG Extractor is required: $Executable" }
    $identity = Get-LauncherExecutableIdentity $Executable { param($Path) [Diagnostics.FileVersionInfo]::GetVersionInfo($Path) }
    if ($identity.State -ne 'Verified' -or $identity.Component -ne 'LauncherSvgExtractor') {
        throw "The bundled SVG Extractor needs manual review: $Executable. $($identity.Reason)"
    }
}

function Get-LauncherOptionalCompanionFiles([string]$SourceDirectory) {
    foreach ($name in @('Launcher.installed.default.toml', 'LauncherSvgExtractor.exe', 'docs\Launcher.md', 'docs\LauncherDrop.md',
        'docs\Installer.md', 'docs\SvgExtractor.md', 'docs\advanced-icon-catalog.md',
        'docs\Launcher.html', 'docs\LauncherDrop.html', 'docs\Installer.html', 'docs\SvgExtractor.html',
        'docs\advanced-icon-catalog.html', 'docs\FileLocations.html',
        'docs\Configuration.html', 'docs\LaunchTargets.html', 'docs\Appearance.html',
        'Maintenance.ps1', 'Uninstall.cmd', 'Discovery.ps1', 'Processes.ps1', 'Lifecycle.ps1', 'Deployment.ps1', 'Registration.ps1', 'WindowTextQuery.cs')) {
        $path = Join-Path $SourceDirectory $name
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        if ($name -eq 'LauncherSvgExtractor.exe') { Assert-LauncherBundledExtractor $path }
        $name
    }
}

function Publish-LauncherDevelopmentVersion([string] $Source, [string] $Directory) {
    $label = Get-LauncherExecutableVersionLabel $Source
    $destination = Join-Path $Directory "Launcher-R$label.exe"
    $hash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    $retained = $null
    $existing = Get-LauncherEntry $destination
    if ($existing -and (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $hash) {
        if ($existing.PSIsContainer -or ($existing.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
            (Test-LauncherFinalRelease $destination) -or (Test-LauncherFinalRelease $Source)) {
            throw "A preserved release already exists with different contents: $destination"
        }
        $retained = Join-Path $Directory ("Launcher-R$label." + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.' + [Guid]::NewGuid().ToString('N') + '.backup.exe')
        [IO.File]::Move($destination, $retained)
    }
    try { $null = Copy-LauncherImmutable $Source $destination }
    catch {
        if ($retained -and !(Get-LauncherEntry $destination)) { [IO.File]::Move($retained, $destination) }
        throw
    }
    return [pscustomobject]@{ Path = $destination; Label = $label; Hash = $hash; RetainedExecutable = $retained }
}

function Copy-LauncherImmutable([string] $Source, [string] $Destination) {
    $hash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    $existing = Get-LauncherEntry $Destination
    if ($existing) {
        if ($existing.PSIsContainer -or ($existing.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
            (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash -ne $hash) {
            throw "Preserved file already exists with different contents: $Destination"
        }
    } else { [IO.File]::Copy($Source, $Destination, $false) }
    if ((Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash -ne $hash) {
        throw "Copied file failed verification: $Destination"
    }
    return $hash
}

function Get-LauncherInstalledRelease([string] $Directory) {
    $active = Join-Path $Directory 'Launcher.exe'
    $releases = @(Get-ChildItem -LiteralPath $Directory -Filter 'Launcher-R*.exe' -File | ForEach-Object {
        if ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Preserved release must be a regular file: $($_.FullName)" }
        if ($_.Name -notmatch '^Launcher-R\d+\.\d+(?:\.\d+){0,2}\.exe$' -or !(Test-LauncherFinalRelease $_.FullName)) { return }
        $label = Get-LauncherReleaseLabel $_.FullName
        if ($_.Name -ne "Launcher-R$label.exe") { throw "Release filename does not match its resource version: $($_.Name)" }
        [pscustomobject]@{ Path = $_.FullName; Label = $label; Version = [version]$label;
            Hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    } | Sort-Object Version -Descending)
    if (!$releases.Count) {
        # Migrate the former convention, where Launcher.exe was the release.
        $entry = Get-LauncherEntry $active
        if (!$entry -or $entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            return $null
        }
        if (!(Test-LauncherFinalRelease $active)) { return $null }
        $label = Get-LauncherReleaseLabel $active
        $releasePath = Join-Path $Directory "Launcher-R$label.exe"
        $hash = Copy-LauncherImmutable $active $releasePath
        return [pscustomobject]@{ Path = $releasePath; Label = $label; Version = [version]$label; Hash = $hash }
    }
    if (Test-Path -LiteralPath $active -PathType Leaf) {
        $activeHash = (Get-FileHash -LiteralPath $active -Algorithm SHA256).Hash
        $matching = @($releases | Where-Object { $_.Hash -eq $activeHash })
        if ($matching.Count) { return $matching[0] }
    }
    return $releases[0]
}

function Stop-LauncherExecutables([string[]] $Paths) {
    $candidates = @($Paths | Where-Object { $_ } | ForEach-Object { [IO.Path]::GetFullPath($_) } | Select-Object -Unique)
    $directories = @($candidates | ForEach-Object { [IO.Path]::GetDirectoryName($_) } | Select-Object -Unique)
    # Do not start an executable just to discover that no copy was running.
    $running = @(Get-CimInstance Win32_Process -Filter "Name like 'Launcher%.exe'" -ErrorAction Stop)
    foreach ($process in $running) {
        if ($process.ExecutablePath -and [IO.Path]::GetFileName($process.ExecutablePath) -ieq 'LauncherSvgExtractor.exe' -and
            $directories -icontains [IO.Path]::GetDirectoryName($process.ExecutablePath)) {
            Assert-LauncherBundledExtractor $process.ExecutablePath
            throw "Close SVG Extractor in '$([IO.Path]::GetDirectoryName($process.ExecutablePath))' before updating Launcher."
        }
    }
    foreach ($process in $running) {
        if (!$process.ExecutablePath -or $candidates -notcontains $process.ExecutablePath) { continue }
        $stopped = Start-Process -FilePath $process.ExecutablePath -ArgumentList '--stop' -WindowStyle Hidden -Wait -PassThru
        if ($stopped.ExitCode -ne 0) { throw "Launcher could not close cleanly: $($process.ExecutablePath)" }
    }
}

function Backup-LauncherConfiguration([string] $Directory, [string] $ReleaseLabel) {
    $source = Join-Path $Directory 'Launcher.toml'
    $backup = Join-Path $Directory "Launcher-R$ReleaseLabel.toml"
    $existing = Get-LauncherEntry $backup
    if ($existing) {
        if ($existing.PSIsContainer -or ($existing.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Configuration snapshot must be a regular file: $backup"
        }
        return $backup # This release's first snapshot is never refreshed.
    }
    if (!(Test-Path -LiteralPath $source -PathType Leaf)) { return $null }
    $stage = Join-Path $Directory ('.launcher-config-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    $hash = Copy-LauncherImmutable $source $stage
    if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $hash) {
        throw "Configuration changed while making the snapshot; staged copy retained: $stage"
    }
    # A versioned configuration uses a sibling runtime file. Preserve both so
    # explicitly launching this snapshot never applies today's saved controls.
    $runtime = Join-Path $Directory 'Launcher.runtime.toml'
    $runtimeBackup = Join-Path $Directory "Launcher-R$ReleaseLabel.runtime.toml"
    if (Test-Path -LiteralPath $runtime -PathType Leaf) { $null = Copy-LauncherImmutable $runtime $runtimeBackup }
    [IO.File]::Move($stage, $backup) # Never replace a prior/concurrent snapshot.
    return $backup
}

function Set-LauncherActiveEntry([string] $Directory, [string] $Stage) {
    $active = Join-Path $Directory 'Launcher.exe'
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Stage)) -ne $Directory) {
        throw 'The staged entry must be directly inside the resolved install directory.'
    }
    $backup = $null
    if (Get-LauncherEntry $active) {
        $backup = Join-Path $Directory ('Launcher.' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.' + [Guid]::NewGuid().ToString('N') + '.backup.exe')
        # Rename the entry itself. Copying over Launcher.exe writes through a link.
        Move-Item -LiteralPath $active -Destination $backup -ErrorAction Stop
    }
    try { Move-Item -LiteralPath $Stage -Destination $active -ErrorAction Stop }
    catch {
        if ($backup -and !(Get-LauncherEntry $active)) { Move-Item -LiteralPath $backup -Destination $active -ErrorAction Stop }
        throw
    }
    return $backup
}

function Install-LauncherAcceptedBuild {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Directory, [Parameter(Mandatory)][string]$SourceDirectory,
        [Parameter(Mandatory)][string]$VersionedExecutable, [Parameter(Mandatory)][string]$Sha256,
        [scriptblock]$VerifyActive = {})

    $install = [IO.Path]::GetFullPath($Directory).TrimEnd('\')
    Assert-LauncherDirectory $install
    $active = Join-Path $install 'Launcher.exe'
    $examples = Join-Path $install 'examples'
    $transaction = Join-Path $install ('.launcher-accept-' + [Guid]::NewGuid().ToString('N'))
    $alias = Join-Path $install ('.launcher-accept-' + [Guid]::NewGuid().ToString('N') + '.staging.exe')
    $payload = [Collections.Generic.List[object]]::new()
    $prior = $null
    $activated = $false
    $committed = $false
    $createdDirectories = [Collections.Generic.List[string]]::new()
    [void][IO.Directory]::CreateDirectory($transaction)
    try {
        $supportNames = @('Launcher.default.toml', 'LICENSE.txt', 'THIRD-PARTY-NOTICES.txt',
            'examples\basic_colors.toml', 'examples\extra_colors.toml', 'examples\colors-light.toml',
            'examples\colors-dark.toml') + @(Get-LauncherOptionalCompanionFiles $SourceDirectory)
        foreach ($name in $supportNames) {
            $destination = Join-Path $install $name
            $existing = Get-LauncherEntry $destination
            if ($existing -and $name.StartsWith('examples\')) { continue }
            if ($existing -and ($existing.PSIsContainer -or ($existing.Attributes -band [IO.FileAttributes]::ReparsePoint))) {
                throw "Installed supporting file must be a regular file: $destination"
            }
            if ($existing -and $name -eq 'LauncherSvgExtractor.exe') { Assert-LauncherBundledExtractor $destination }
            $entry = [pscustomobject]@{ Destination = $destination; Stage = Join-Path $transaction ([IO.Path]::GetFileName($name));
                Backup = Join-Path $transaction ([IO.Path]::GetFileName($name) + '.previous');
                Hash = $null; BackedUp = $false; Published = $false }
            $payload.Add($entry)
            $entry.Hash = Copy-LauncherImmutable (Join-Path $SourceDirectory $name) $entry.Stage
        }
        New-LauncherReleaseLink $alias $VersionedExecutable
        if ((Get-FileHash -LiteralPath $alias -Algorithm SHA256).Hash -ne $Sha256) { throw 'Staged development alias failed verification.' }
        foreach ($entry in $payload) {
            $parent = [IO.Path]::GetDirectoryName($entry.Destination)
            if ($parent -ne $install) {
                if (!(Get-LauncherEntry $parent)) { [void][IO.Directory]::CreateDirectory($parent); $createdDirectories.Add($parent) }
                Assert-LauncherDirectory $parent
            }
            if (Get-LauncherEntry $entry.Destination) {
                Move-Item -LiteralPath $entry.Destination -Destination $entry.Backup -ErrorAction Stop
                $entry.BackedUp = $true
            }
            Move-Item -LiteralPath $entry.Stage -Destination $entry.Destination -ErrorAction Stop
            $entry.Published = $true
            if ((Get-FileHash -LiteralPath $entry.Destination -Algorithm SHA256).Hash -ne $entry.Hash) {
                throw "Installed supporting file failed verification: $($entry.Destination)"
            }
        }
        $prior = Set-LauncherActiveEntry $install $alias
        $activated = $true
        if ((Get-FileHash -LiteralPath $active -Algorithm SHA256).Hash -ne $Sha256) { throw 'Installed development executable failed verification.' }
        & $VerifyActive $active | Out-Null
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
            try { [IO.Directory]::Delete($createdDirectories[$index]) } catch { $rollbackErrors.Add($_.Exception.Message) }
        }
        if ($rollbackErrors.Count) {
            throw "Build acceptance failed: $($failure.Exception.Message) Recovery files retained in $transaction. Rollback: $($rollbackErrors -join '; ')"
        }
        throw $failure
    } finally {
        foreach ($path in @($alias) + @($payload | ForEach-Object { $_.Stage; if ($committed) { $_.Backup } })) {
            if (Get-LauncherEntry $path) {
                try { Remove-Item -LiteralPath $path -Force -ErrorAction Stop }
                catch { Write-Warning "Acceptance staging entry retained: $path" }
            }
        }
        try { [IO.Directory]::Delete($transaction) } catch { Write-Warning "Acceptance recovery directory retained: $transaction" }
    }
    return $prior
}
