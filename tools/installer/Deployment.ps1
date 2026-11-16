#requires -Version 5.1
# The shipping installer's file transaction. User profiles and process lifetime
# belong to the unelevated coordinator, not to this payload module.

if (-not (Get-Command Get-LauncherExecutableIdentity -ErrorAction SilentlyContinue)) {
    . (Join-Path $PSScriptRoot 'Discovery.ps1')
}
if (-not (Get-Command New-LauncherRegistrationTransaction -ErrorAction SilentlyContinue)) {
    . (Join-Path $PSScriptRoot 'Registration.ps1')
}

function Get-LauncherSetupEntry([string]$Path) {
    Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
}

function Assert-LauncherSetupDirectory([string]$Path) {
    $directory = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $current = $directory
    while ($current) {
        $entry = Get-LauncherSetupEntry $current
        if ($entry -and (!$entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint))) {
            throw "Installation paths must use real directories: $current"
        }
        $parent = [IO.Path]::GetDirectoryName($current)
        if ($parent -eq $current) { break }
        $current = $parent
    }
    return $directory
}

function Enter-LauncherInstallationLock([string]$Directory) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $identity = ([IO.Path]::GetFullPath($Directory).TrimEnd('\')).ToUpperInvariant()
        $digest = [BitConverter]::ToString($algorithm.ComputeHash([Text.Encoding]::Unicode.GetBytes($identity))).Replace('-', '')
    } finally { $algorithm.Dispose() }
    $mutex = [Threading.Mutex]::new($false, ('Global\Dlugosz.Launcher.Installation.' + $digest))
    try {
        try { $acquired = $mutex.WaitOne(0) }
        catch [Threading.AbandonedMutexException] { $acquired = $true }
        if (!$acquired) { throw 'Another Setup or maintenance operation is already using this installation.' }
        return $mutex
    } catch { $mutex.Dispose(); throw }
}

function Exit-LauncherInstallationLock($Mutex) {
    if ($Mutex) {
        try { $Mutex.ReleaseMutex() } finally { $Mutex.Dispose() }
    }
}

function Test-LauncherManagedRelativePath([string]$Path) {
    return $Path -match '\A(?:Launcher(?:-R\d+\.\d+(?:\.\d+){0,2})?\.exe|Launcher Setup\.exe|LauncherDrop\.exe|LauncherSvgExtractor\.exe|Launcher\.(?:installed\.)?default\.toml|LICENSE\.txt|THIRD-PARTY-NOTICES\.txt|README\.txt|Install\.(?:ps1|cmd)|Uninstall\.cmd|Maintenance\.ps1|Registration\.ps1|Discovery\.ps1|Deployment\.ps1|Processes\.ps1|Lifecycle\.ps1|Worker\.ps1|Presentation\.ps1|PortableProfile\.ps1|WindowTextQuery\.cs|ArchiveRecovery\.ps1|ArchiveRestoreJournal\.ps1|ArchiveSigner\.cer|docs\\(?:(?:Launcher|LauncherDrop|Installer|SvgExtractor|advanced-icon-catalog)\.(?:md|html)|(?:FileLocations|Configuration|LaunchTargets|Appearance)\.html)|examples\\(?:basic_colors|extra_colors|colors-light|colors-dark|demo_many_colors)\.toml|examples\\demo_many_colors\.cmd)\z'
}

function Test-LauncherArchiveRestoreEditableFile([string]$Path) {
    return $Path -match '\A(?:docs\\.+|examples\\.+|Launcher\.(?:installed\.)?default\.toml|README\.txt|LICENSE\.txt|THIRD-PARTY-NOTICES\.txt)\z'
}

function Test-LauncherQuickRollbackBuildIdentity($Build) {
    $Build = [pscustomobject]$Build
    $version = [string](Get-LauncherDiscoveryProperty $Build 'Version')
    $hash = [string](Get-LauncherDiscoveryProperty $Build 'Sha256')
    $number = $null
    return $version -cmatch '\A[0-9]+\.[0-9]+(?:\.[0-9]+){0,2}\z' -and
        [version]::TryParse($version, [ref]$number) -and $version -ceq $number.ToString() -and $hash -cmatch '\A[0-9A-F]{64}\z'
}

function Get-LauncherQuickRollbackCompanionHashes($Files, [string]$Version, [string]$Sha256) {
    $all = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in @($Files)) {
        $name = [string](Get-LauncherDiscoveryProperty $file 'RelativePath')
        $hash = [string](Get-LauncherDiscoveryProperty $file 'Sha256')
        if (!(Test-LauncherManagedRelativePath $name) -or $hash -cnotmatch '\A[0-9A-F]{64}\z' -or $all.ContainsKey($name)) {
            throw 'The installation manifest has an invalid or repeated file entry.'
        }
        $all.Add($name, $hash)
    }
    foreach ($name in @('Launcher.exe', "Launcher-R$Version.exe")) {
        if (!$all.ContainsKey($name) -or $all[$name] -cne $Sha256) { throw 'The installation manifest does not identify the active build.' }
        [void]$all.Remove($name)
    }
    if (!$all.Count -or !$all.ContainsKey('Maintenance.ps1') -or !$all.ContainsKey('WindowTextQuery.cs')) {
        throw 'The installation manifest does not record the required maintenance files.'
    }
    return ,$all
}

function Test-LauncherQuickRollbackCompanions {
    param($Saved, $Incoming, $Entries, [string]$Directory, [string]$PreviousVersion, [string]$PreviousHash,
        [string]$CurrentVersion, [string]$CurrentHash)
    if ((Get-LauncherDiscoveryProperty $Saved 'SchemaVersion') -ne 1 -or
        (Get-LauncherDiscoveryProperty $Saved 'ActiveVersionedExecutable') -cne "Launcher-R$PreviousVersion.exe" -or
        (Get-LauncherDiscoveryProperty $Saved 'AliasKind') -cnotin @('HardLink', 'Copy') -or
        (Get-LauncherDiscoveryProperty $Saved 'InstallScope') -cne $Incoming.InstallScope -or
        [string](Get-LauncherDiscoveryProperty $Saved 'InstallUserSid') -cne $Incoming.InstallUserSid) { return $false }
    try {
        $before = Get-LauncherQuickRollbackCompanionHashes $Saved.Files $PreviousVersion $PreviousHash
        $after = Get-LauncherQuickRollbackCompanionHashes $Incoming.Files $CurrentVersion $CurrentHash
    } catch { return $false }
    if ($before.Count -ne $after.Count -or $Entries.Count -ne $after.Count + 2) { return $false }
    foreach ($entry in $Entries) {
        $relative = $entry.Destination.Substring($Directory.Length + 1)
        if ($entry.IsActive -or $relative -ceq "Launcher-R$CurrentVersion.exe") { continue }
        if (!$before.ContainsKey($relative) -or !$after.ContainsKey($relative) -or
            $entry.OriginalHash -cne $entry.Sha256 -or $before[$relative] -cne $entry.Sha256 -or
            $after[$relative] -cne $entry.Sha256) { return $false }
    }
    return $true
}

function Get-LauncherPayload {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$PackageDirectory)

    $package = Assert-LauncherSetupDirectory $PackageDirectory
    if (!(Test-Path -LiteralPath $package -PathType Container)) { throw "Package directory is missing: $package" }
    $executable = Get-LauncherPackageExecutable -PackageDirectory $package
    $required = @([IO.Path]::GetFileName($executable), 'LauncherDrop.exe', 'LauncherSvgExtractor.exe', 'Launcher.default.toml', 'Launcher.installed.default.toml', 'LICENSE.txt',
        'THIRD-PARTY-NOTICES.txt', 'README.txt', 'docs\Launcher.md', 'docs\LauncherDrop.md', 'docs\Installer.md',
        'docs\SvgExtractor.md', 'docs\advanced-icon-catalog.md',
        'docs\Launcher.html', 'docs\LauncherDrop.html', 'docs\Installer.html', 'docs\SvgExtractor.html',
        'docs\advanced-icon-catalog.html', 'docs\FileLocations.html',
        'examples\basic_colors.toml', 'examples\extra_colors.toml', 'examples\colors-light.toml',
        'examples\colors-dark.toml',
        'Maintenance.ps1', 'Uninstall.cmd', 'Discovery.ps1', 'Processes.ps1', 'Lifecycle.ps1', 'Deployment.ps1', 'Registration.ps1', 'WindowTextQuery.cs')
    # Older archives used Launcher.html for the entire user guide.
    $referenceGuides = @('docs\Configuration.html', 'docs\LaunchTargets.html', 'docs\Appearance.html')
    foreach ($name in $referenceGuides) {
        if (Get-LauncherSetupEntry (Join-Path $package $name)) {
            $required += $referenceGuides
            break
        }
    }
    if (Get-LauncherSetupEntry (Join-Path $package 'Launcher Setup.exe')) { $required += 'Launcher Setup.exe' }
    $archiveEnabled = !(Get-Variable -Name DisableLauncherArchiveSupport -ValueOnly -ErrorAction SilentlyContinue) -and
        $null -ne (Get-LauncherSetupEntry (Join-Path $package 'Launcher.archive.json'))
    if ($archiveEnabled) { $required += @('ArchiveRecovery.ps1', 'ArchiveRestoreJournal.ps1', 'ArchiveSigner.cer') }
    $files = [Collections.Generic.List[object]]::new()
    foreach ($name in $required) {
        $path = Join-Path $package $name
        [void](Assert-LauncherSetupDirectory ([IO.Path]::GetDirectoryName($path)))
        $entry = Get-LauncherSetupEntry $path
        if (!$entry) {
            throw "Bundled file is missing: $name"
        }
        if ($entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Bundled files must be regular files: $path"
        }
        $files.Add([pscustomobject]@{
            RelativePath = $name; SourcePath = $path
            Sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256 -ErrorAction Stop).Hash
            PreserveExisting = $name.StartsWith('examples\', [StringComparison]::OrdinalIgnoreCase)
        })
    }
    $reader = { param($Path) [Diagnostics.FileVersionInfo]::GetVersionInfo($Path) }
    $identity = $null
    foreach ($name in @([IO.Path]::GetFileName($executable), 'LauncherDrop.exe', 'LauncherSvgExtractor.exe')) {
        $component = Get-LauncherExecutableIdentity (Join-Path $package $name) $reader
        $expected = if ($name -ieq 'LauncherDrop.exe') { 'LauncherDrop' }
            elseif ($name -ieq 'LauncherSvgExtractor.exe') { 'LauncherSvgExtractor' } else { 'Launcher' }
        if ($component.State -ne 'Verified' -or $component.Component -ne $expected) {
            throw "The package does not contain an installable $name`: $($component.Reason)"
        }
        if ([Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $package $name)).IsDebug) { throw "A debug executable cannot be installed: $name" }
        if ($expected -eq 'Launcher') { $identity = $component }
    }
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($executable)
    if ($version.IsDebug) { throw 'A debug executable cannot be installed.' }
    $kind = Get-LauncherDistributionKind -Metadata $version
    $label = Get-LauncherVersionLabel -Metadata $version
    return [pscustomobject]@{
        PackageDirectory = $package; ExecutableIdentity = $identity
        ExecutablePath = $executable; VersionLabel = $label; DistributionKind = $kind
        IsRelease = $kind -eq 'Release'; ReleaseLabel = $label; Files = @($files); ArchiveEnabled = $archiveEnabled
    }
}

function Remove-LauncherObsoleteStockDemos {
    param([string]$InstallDirectory)
    # Exact SHA-256 values of the stock files shipped before demos moved into
    # AppData. Filename alone never authorizes deleting an edited working copy.
    $stock = @{
        'demo_many_colors.toml' = @('6236F40880B9467DC54A039EC683B8002C0104EA0223B7B2354F48C4FAE6C8C1', '79EF8C265CFB2424AA778D25AED0DC8A01A101D7608F8076DA30A92D12F83A38')
        'demo_many_colors.cmd' = @('B0F8E11B127017F48978AAF529595BE83083CA7131C7FE867F162DB17EE974CE', 'BD4C107F0F26317525C0FEE82176EF3965B9BA09A77FE4AFCEE37D09F8C750E3', 'DD04B433A0B8D66AE137F5DF67FA0BC645F77554052E41DFCA6D0D539B57FB9E')
    }
    foreach ($name in $stock.Keys) {
        $path = Join-Path $InstallDirectory ('examples\' + $name)
        $entry = Get-LauncherSetupEntry $path
        if (!$entry) { continue }
        try {
            [void](Assert-LauncherSetupDirectory ([IO.Path]::GetDirectoryName($path)))
            if ($entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'The old demo is not a regular file.' }
            if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -notin $stock[$name]) { throw 'The old demo differs from the shipped defaults; it was preserved.' }
            [IO.File]::Delete($path)
        } catch { "Launcher was installed, but an old demo was retained: $path. $($_.Exception.Message)" }
    }
}

function Initialize-LauncherSetupFileApi {
    if ('LauncherSetup.PayloadFiles' -as [type]) { return }
    Add-Type @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
namespace LauncherSetup {
    public static class PayloadFiles {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool MoveFileExW(string source, string destination, uint flags);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool CreateHardLinkW(string path, string target, IntPtr security);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern SafeFileHandle CreateFileW(string path, uint access, uint share,
            IntPtr security, uint creation, uint flags, IntPtr template);
        [StructLayout(LayoutKind.Sequential)]
        public struct FileAttributeTag {
            public uint Attributes;
            public uint ReparseTag;
        }
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GetFileInformationByHandleEx(SafeFileHandle file, int infoClass,
            out FileAttributeTag info, uint size);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool SetFileInformationByHandle(SafeFileHandle file, int infoClass, ref byte info, uint size);
    }
}
'@
}

function New-LauncherSetupAlias {
    param([string]$Path, [string]$Target, [scriptblock]$HardLinkCreator)
    if (Get-LauncherSetupEntry $Path) { throw "The new alias entry already exists: $Path" }
    $linked = if ($HardLinkCreator) { & $HardLinkCreator $Path $Target }
        else { [LauncherSetup.PayloadFiles]::CreateHardLinkW($Path, $Target, [IntPtr]::Zero) }
    if ($linked) { return 'HardLink' }
    # Always copy to a fresh entry. Copying over an existing hard/symbolic link
    # can modify its preserved target instead of replacing the alias itself.
    [IO.File]::Copy($Target, $Path, $false)
    return 'Copy'
}

function Get-LauncherSetupPreviousVersion {
    param([string]$InstallDirectory, [string]$ActiveHash)
    $active = Join-Path $InstallDirectory 'Launcher.exe'
    $entry = Get-LauncherSetupEntry $active
    $names = @()
    if ($entry -and $entry.LinkType -eq 'SymbolicLink') { $names = @($entry.Target) }
    $manifest = Join-Path $InstallDirectory 'Launcher.install-manifest.json'
    $manifestEntry = Get-LauncherSetupEntry $manifest
    if ($manifestEntry -and !$manifestEntry.PSIsContainer -and !($manifestEntry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        try {
            $saved = [IO.File]::ReadAllText($manifest) | ConvertFrom-Json
            $name = [string](Get-LauncherDiscoveryProperty $saved 'ActiveVersionedExecutable')
            if ($name) { $names = @($name) + $names }
        } catch { }
    }
    $preferred = @($names)
    $matching = [Collections.Generic.List[string]]::new()
    $names += @(Get-ChildItem -LiteralPath $InstallDirectory -Filter 'Launcher-R*.exe' -File -Force | ForEach-Object Name)
    foreach ($name in ($names | Select-Object -Unique)) {
        if ($name -notmatch '^Launcher-R\d+\.\d+(?:\.\d+){0,2}\.exe$') { continue }
        $path = Join-Path $InstallDirectory $name
        $candidate = Get-LauncherSetupEntry $path
        if (!$candidate -or $candidate.PSIsContainer -or ($candidate.Attributes -band [IO.FileAttributes]::ReparsePoint)) { continue }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256 -ErrorAction Stop).Hash -ne $ActiveHash) { continue }
        Assert-LauncherSetupExecutable $path 'Launcher'
        if ($name -in $preferred) { return $path }
        $matching.Add($path)
    }
    # An unversioned legacy copy may match more than one archive. Preserve a
    # separate copy rather than guessing which unrelated archive to remove.
    if ($matching.Count -eq 1) { return $matching[0] }
    return $null
}

function Get-LauncherReleaseReviewFallback {
    param($LastUpdate, [object[]]$Candidates, [string]$PreviousPath)

    $receipt = [pscustomobject]$LastUpdate
    if ($null -eq $receipt -or (($receipt.PSObject.Properties.Name | Sort-Object) -join '|') -cne
        'CommittedUtc|Current|Previous|SchemaVersion|TransactionId') { return }
    $schema = Get-LauncherDiscoveryProperty $receipt 'SchemaVersion'
    $transaction = Get-LauncherDiscoveryProperty $receipt 'TransactionId'
    if (($schema -isnot [int] -and $schema -isnot [long]) -or $schema -ne 1 -or
        $transaction -isnot [string] -or $transaction -cnotmatch '\A[0-9a-f]{32}\z' -or $transaction -ceq ('0' * 32)) { return }

    $time = Get-LauncherDiscoveryProperty $receipt 'CommittedUtc'
    if ($time -is [datetime]) {
        if ($time.Kind -ne [DateTimeKind]::Utc) { return }
    } else {
        $utc = [DateTimeOffset]::MinValue
        if ($time -isnot [string] -or ![DateTimeOffset]::TryParseExact($time, 'O', [Globalization.CultureInfo]::InvariantCulture,
                [Globalization.DateTimeStyles]::None, [ref]$utc) -or $utc.Offset -ne [TimeSpan]::Zero) { return }
    }

    $prior = [pscustomobject](Get-LauncherDiscoveryProperty $receipt 'Previous')
    $current = [pscustomobject](Get-LauncherDiscoveryProperty $receipt 'Current')
    foreach ($build in @($prior, $current)) {
        if ($null -eq $build -or (($build.PSObject.Properties.Name | Sort-Object) -join '|') -cne 'Sha256|Version' -or
            $build.Version -isnot [string] -or $build.Sha256 -isnot [string]) { return }
    }
    $versionText = $prior.Version
    $hash = $prior.Sha256
    $version = $null
    if ($versionText.Length -gt 23 -or ![version]::TryParse($versionText, [ref]$version) -or
        $versionText -cne $version.ToString() -or $hash -cnotmatch '\A[0-9A-F]{64}\z' -or $hash -ceq $current.Sha256) { return }

    $numeric = [version]::new($version.Major, $version.Minor, [Math]::Max(0, $version.Build), [Math]::Max(0, $version.Revision))
    $matching = @($Candidates | Where-Object {
        $_.Version -eq $numeric -and $_.Sha256 -ceq $hash -and [IO.Path]::GetFileName($_.Path) -ine 'Launcher-WIP.exe'
    })
    # Cleanup discovery already checked the parent, filename, PE metadata and bytes.
    # Prefer Setup's prior path, then the canonical numbered file, then an exact backup.
    $matching | Sort-Object @{ Expression = { $_.Path -ine $PreviousPath } },
        @{ Expression = { [IO.Path]::GetFileName($_.Path) -ine "Launcher-R$versionText.exe" } }, Path |
        Select-Object -First 1 -ExpandProperty Path
}

function Invoke-LauncherSetupDestinationAction([scriptblock]$Action) {
    try { & $Action }
    catch {
        $cause = $_.Exception
        while ($cause) {
            if ($cause -is [UnauthorizedAccessException] -or
                ($cause -is [ComponentModel.Win32Exception] -and $cause.NativeErrorCode -eq 5)) {
                $_.Exception.Data['LauncherPermissionDenied'] = $true
                break
            }
            $cause = $cause.InnerException
        }
        throw
    }
}

function Move-LauncherSetupFile([string]$Source, [string]$Destination, [bool]$Replace) {
    $flags = [uint32]8 # Same-volume rename; no copy fallback or reboot scheduling.
    if ($Replace) { $flags = $flags -bor 1 }
    if (![LauncherSetup.PayloadFiles]::MoveFileExW($Source, $Destination, $flags)) {
        $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw [ComponentModel.Win32Exception]::new($code, "Cannot publish '$Destination'.")
    }
}

function Assert-LauncherSetupReplaceAccess([string]$Path) {
    # Opening the entry with DELETE access tests replacement without changing it.
    # OPEN_REPARSE_POINT prevents following the permitted active release link.
    $handle = [LauncherSetup.PayloadFiles]::CreateFileW($Path, 0x10000, 7, [IntPtr]::Zero, 3, 0x200000, [IntPtr]::Zero)
    if ($handle.IsInvalid) {
        $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        $handle.Dispose()
        throw [ComponentModel.Win32Exception]::new($code, "Cannot replace '$Path'. Close any application using this file and try again.")
    }
    $handle.Dispose()
}

function Remove-LauncherVerifiedExecutable([string]$Path, [string]$Sha256) {
    [void](Assert-LauncherSetupDirectory ([IO.Path]::GetDirectoryName($Path)))
    Initialize-LauncherSetupFileApi
    # Pin the actual directory entry with read and DELETE access. Hash and
    # remove through the same handle, with no competing write or rename.
    $handle = [LauncherSetup.PayloadFiles]::CreateFileW($Path, [uint32]0x80010000L, 1, [IntPtr]::Zero, 3, 0x200000, [IntPtr]::Zero)
    try {
        if ($handle.IsInvalid) {
            $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            if ($code -eq 2) { return } # A repeated cleanup has nothing to remove.
            throw [ComponentModel.Win32Exception]::new($code, "Cannot open the retained Launcher executable: $Path")
        }
        $info = [LauncherSetup.PayloadFiles+FileAttributeTag]::new()
        if (![LauncherSetup.PayloadFiles]::GetFileInformationByHandleEx($handle, 9, [ref]$info, 8) -or
            ($info.Attributes -band ([IO.FileAttributes]::Directory -bor [IO.FileAttributes]::ReparsePoint))) {
            throw "The retained Launcher executable must be a regular file: $Path"
        }
        $stream = [IO.FileStream]::new($handle, [IO.FileAccess]::Read)
        try {
            $algorithm = [Security.Cryptography.SHA256]::Create()
            try { $actual = [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '') }
            finally { $algorithm.Dispose() }
            if ($actual -cne $Sha256) { throw "The retained Launcher executable changed: $Path" }
            [byte]$remove = 1
            if (![LauncherSetup.PayloadFiles]::SetFileInformationByHandle($handle, 4, [ref]$remove, 1)) {
                $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
                throw [ComponentModel.Win32Exception]::new($code, "Cannot remove the retained Launcher executable: $Path")
            }
        } finally { $stream.Dispose() }
    } finally { $handle.Dispose() }
}

function Assert-LauncherSetupExecutable([string]$Path, [string]$Component) {
    $identity = Get-LauncherExecutableIdentity $Path { param($File) [Diagnostics.FileVersionInfo]::GetVersionInfo($File) }
    if ($identity.State -ne 'Verified' -or $identity.Component -ne $Component) {
        throw "The existing $Component executable needs manual review: $Path. $($identity.Reason)"
    }
}

function Assert-LauncherSetupActiveEntry([string]$Path) {
    $entry = Get-LauncherSetupEntry $Path
    if (!$entry) { return }
    if ($entry.PSIsContainer) { throw "The active Launcher entry is a directory: $Path" }
    if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        $targets = @($entry.Target)
        if ($entry.LinkType -ne 'SymbolicLink' -or $targets.Count -ne 1 -or
            $targets[0] -notmatch '^Launcher-R\d+\.\d+(?:\.\d+){0,2}\.exe$') {
            throw "The active Launcher link needs manual review: $Path"
        }
        $release = Get-LauncherSetupEntry (Join-Path $entry.DirectoryName $targets[0])
        if (!$release -or $release.PSIsContainer -or ($release.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "The active Launcher link must target a regular preserved release: $Path"
        }
    }
    Assert-LauncherSetupExecutable $Path 'Launcher'
}

function Install-LauncherPayload {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$PackageDirectory,
        [Parameter(Mandatory)][string]$InstallDirectory,
        [scriptblock]$BeforeCommit,
        [scriptblock]$Progress,
        [scriptblock]$AliasHardLinkCreator,
        [scriptblock]$AfterActivation,
        [ValidateSet('AllUsers', 'CurrentUser')][string]$InstallScope,
        [string]$InstallUserSid,
        [Guid]$UpdaterTransactionId = [Guid]::Empty,
        [switch]$QuickRollbackEligible,
        $ArchiveRestore
    )

    $payload = Get-LauncherPayload $PackageDirectory
    $install = Assert-LauncherSetupDirectory $InstallDirectory
    $sourceTree = $payload.PackageDirectory + '\'
    $installTree = $install + '\'
    $archiveStage = $ArchiveRestore -and [IO.Path]::GetDirectoryName($payload.PackageDirectory) -ieq $install -and
        [IO.Path]::GetFileName($payload.PackageDirectory) -cmatch '\A\.launcher-restore-source-[0-9a-f]{32}\z'
    if ($install -eq [IO.Path]::GetPathRoot($install).TrimEnd('\') -or
        $installTree.StartsWith($sourceTree, [StringComparison]::OrdinalIgnoreCase) -or
        ($sourceTree.StartsWith($installTree, [StringComparison]::OrdinalIgnoreCase) -and !$archiveStage)) {
        throw 'Package and installation must be separate, non-overlapping dedicated directories.'
    }
    Initialize-LauncherSetupFileApi
    $createdDirectories = [Collections.Generic.List[string]]::new()
    $entries = [Collections.Generic.List[object]]::new()
    $transaction = Join-Path $install ('.launcher-setup-' + [Guid]::NewGuid().ToString('N'))
    $previous = $null
    $releasePath = Join-Path $install ("Launcher-R$($payload.VersionLabel).exe")
    $aliasKind = $null
    $cleanupWarnings = [Collections.Generic.List[string]]::new()
    $previousHash = $null
    $backupActive = $null
    $cleanupCandidates = @()
    $completed = $false
    $recoveryNeeded = $false
    $journaled = $false
    $lock = Enter-LauncherInstallationLock $install
    try {
        if (Get-LauncherSetupEntry (Join-Path $install '.launcher-archive-restore.json')) {
            throw 'An archived revert was interrupted. Finish recovery before changing this Launcher installation.'
        }
        $archiveIdentity = $null
        if ($ArchiveRestore) {
            Assert-LauncherArchiveRestoreManifest $install $ArchiveRestore
            if (!$archiveStage -or $payload.VersionLabel -cne $ArchiveRestore.Previous.Version -or
                $payload.Files[0].Sha256 -cne $ArchiveRestore.Previous.Sha256) { throw 'The archived restore payload changed after verification.' }
            foreach ($file in $payload.Files) {
                if (!$ArchiveRestore.PreviousFiles.ContainsKey($file.RelativePath) -or
                    $ArchiveRestore.PreviousFiles[$file.RelativePath] -cne $file.Sha256) { throw 'An archived restore file changed after verification.' }
            }
            $archiveIdentity = $ArchiveRestore.PreviousArchive
        } elseif ($InstallScope -and $AfterActivation -and $payload.ArchiveEnabled) {
            . (Join-Path $payload.PackageDirectory 'ArchiveRecovery.ps1')
            $archiveIdentity = Get-LauncherInstallArchiveIdentity $payload $UpdaterTransactionId
        }
        if ($Progress) { & $Progress 5 'Preparing the application folder' | Out-Null }
        # Track each missing ancestor so a failed fresh install can leave no
        # empty application/vendor directories behind. Never delete recursively.
        $missing = [Collections.Generic.List[string]]::new()
        $current = $install
        while (!(Get-LauncherSetupEntry $current)) {
            $missing.Add($current)
            $current = [IO.Path]::GetDirectoryName($current)
        }
        for ($index = $missing.Count - 1; $index -ge 0; --$index) {
            Invoke-LauncherSetupDestinationAction { [void][IO.Directory]::CreateDirectory($missing[$index]) }
            $createdDirectories.Add($missing[$index])
        }
        Invoke-LauncherSetupDestinationAction { [void][IO.Directory]::CreateDirectory($transaction) }
        $active = Join-Path $install 'Launcher.exe'
        Assert-LauncherSetupActiveEntry $active
        if (Get-LauncherSetupEntry $active) {
            $previousHash = (Get-FileHash -LiteralPath $active -Algorithm SHA256 -ErrorAction Stop).Hash
            if ($previousHash -ne $payload.Files[0].Sha256) {
                $previous = Get-LauncherSetupPreviousVersion -InstallDirectory $install -ActiveHash $previousHash
            }
        }
        $companions = @{ 'LauncherDrop.exe' = 'LauncherDrop'; 'LauncherSvgExtractor.exe' = 'LauncherSvgExtractor' }
        foreach ($companion in $companions.Keys) {
            $path = Join-Path $install $companion
            if (Get-LauncherSetupEntry $path) { Assert-LauncherSetupExecutable $path $companions[$companion] }
        }
        $plans = @(
            [pscustomobject]@{
                RelativePath = [IO.Path]::GetFileName($releasePath)
                SourcePath = $payload.ExecutablePath
                Sha256 = $payload.Files[0].Sha256; PreserveExisting = $true
            }
        ) + @($payload.Files | Select-Object -Skip 1)
        # The alias is staged only after its versioned target has been verified.
        $plans += [pscustomobject]@{
            RelativePath = 'Launcher.exe'; SourcePath = $null
            Sha256 = $payload.Files[0].Sha256; PreserveExisting = $false
        }
        $number = 0
        foreach ($file in $plans) {
            $destination = Join-Path $install $file.RelativePath
            $parent = [IO.Path]::GetDirectoryName($destination)
            [void](Assert-LauncherSetupDirectory $parent)
            if (!(Get-LauncherSetupEntry $parent)) {
                Invoke-LauncherSetupDestinationAction { [void][IO.Directory]::CreateDirectory($parent) }
                $createdDirectories.Add($parent)
            }
            # A new staged file in every destination parent tests new-file and
            # delete rights before the coordinator closes any running program.
            $probe = Join-Path $parent ('.launcher-access-' + [Guid]::NewGuid().ToString('N') + '.tmp')
            try { Invoke-LauncherSetupDestinationAction { [IO.File]::WriteAllBytes($probe, [byte[]]@()); [IO.File]::Delete($probe) } }
            finally { if (Get-LauncherSetupEntry $probe) { [IO.File]::Delete($probe) } }
            $existing = Get-LauncherSetupEntry $destination
            if ($existing -and $destination -ne $active -and
                ($existing.PSIsContainer -or ($existing.Attributes -band [IO.FileAttributes]::ReparsePoint))) {
                throw "Installed supporting files must be regular files: $destination"
            }
            $originalHash = if ($existing) { (Get-FileHash -LiteralPath $destination -Algorithm SHA256 -ErrorAction Stop).Hash } else { $null }
            if ($destination -eq $releasePath -and $existing -and $originalHash -ne $file.Sha256) {
                throw "A versioned executable already exists with different contents: $destination. Package a new revision before installing."
            }
            $replaceMarkedRelease = $existing -and $destination -eq $releasePath -and
                (Get-Item -LiteralPath $destination -Stream Zone.Identifier -ErrorAction SilentlyContinue)
            $skip = $existing -and $file.PreserveExisting -and !$replaceMarkedRelease
            if ($ArchiveRestore -and $existing -and $destination -ine $active -and $destination -ine $releasePath -and
                $originalHash -ceq $file.Sha256) { $skip = $true }
            if ($ArchiveRestore -and $existing -and !$skip -and $destination -ine $active -and $destination -ine $releasePath -and
                $originalHash -cne $file.Sha256 -and (!$ArchiveRestore.Owned.ContainsKey($file.RelativePath) -or
                    $ArchiveRestore.Owned[$file.RelativePath] -cne $originalHash)) {
                if (Test-LauncherArchiveRestoreEditableFile $file.RelativePath) {
                    $skip = $true
                } else { throw "A changed or unowned program file prevents restoring this package: $destination" }
            }
            $stage = $null
            if (!$ArchiveRestore -or !$skip) {
                $stage = Join-Path $transaction ('stage-' + $number)
                if ($destination -eq $active) {
                    $versionEntry = $entries | Where-Object Destination -eq $releasePath | Select-Object -First 1
                    $target = if ($versionEntry.Skip) { $releasePath } else { $versionEntry.Stage }
                    $aliasKind = New-LauncherSetupAlias -Path $stage -Target $target -HardLinkCreator $AliasHardLinkCreator
                } else {
                    $copySource = if ($replaceMarkedRelease) { $destination } else { $file.SourcePath }
                    [IO.File]::Copy($copySource, $stage, $false)
                }
                if ((Get-FileHash -LiteralPath $stage -Algorithm SHA256 -ErrorAction Stop).Hash -ne $file.Sha256) {
                    throw "Bundled file changed while staging: $($file.RelativePath)"
                }
                if ($destination -eq $releasePath) {
                    # The active alias is staged from this verified copy on the next pass.
                    Unblock-File -LiteralPath $stage -ErrorAction Stop
                }
            }
            $backup = $null
            if ($existing -and !$skip) {
                Invoke-LauncherSetupDestinationAction { Assert-LauncherSetupReplaceAccess $destination }
                $backup = Join-Path $transaction ('backup-' + $number)
                if ($destination -eq $active) {
                    # Only legacy active files need a durable standalone copy.
                    # A known versioned target already preserves the old bytes.
                    if (!$ArchiveRestore -and !$previous -and $previousHash -ne $file.Sha256) {
                        $backup = Join-Path $install ('Launcher.' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.' + [Guid]::NewGuid().ToString('N') + '.backup.exe')
                        $previous = $backup
                    }
                    $backupActive = $backup
                }
                # Copying the active link preserves its verified runnable bytes;
                # replacing the link itself never writes into its release target.
                if ($ArchiveRestore -and $destination -eq $active -and $ArchiveRestore.AliasKind -ceq 'HardLink') {
                    if ((New-LauncherSetupAlias -Path $backup -Target $destination) -cne 'HardLink') {
                        throw 'The current hard-link alias could not be preserved before restoring the archive.'
                    }
                } else { Invoke-LauncherSetupDestinationAction { [IO.File]::Copy($destination, $backup, $false) } }
                if ((Get-FileHash -LiteralPath $backup -Algorithm SHA256 -ErrorAction Stop).Hash -ne $originalHash -or
                    (Get-FileHash -LiteralPath $destination -Algorithm SHA256 -ErrorAction Stop).Hash -ne $originalHash) {
                    throw "Installed file changed while making its backup: $destination"
                }
            }
            $entries.Add([pscustomobject]@{
                Destination = $destination; Stage = $stage; Backup = $backup
                Sha256 = $file.Sha256; OriginalHash = $originalHash; Skip = $skip
                Published = $false; IsActive = $destination -eq $active; IsRemoval = $false
            })
            $number++
        }
        # Only files matching this package enter the ownership receipt. Preserved
        # edited examples and personal portable/profile files remain unowned.
        $owned = @($entries | Where-Object { !$_.Skip -or $_.OriginalHash -eq $_.Sha256 } | ForEach-Object {
            [pscustomobject]@{ RelativePath = $_.Destination.Substring($install.Length + 1); Sha256 = $_.Sha256 }
        })
        $manifestStage = Join-Path $transaction 'manifest.json'
        $manifestPath = Join-Path $install 'Launcher.install-manifest.json'
        $manifestEntry = Get-LauncherSetupEntry $manifestPath
        if ($manifestEntry -and ($manifestEntry.PSIsContainer -or ($manifestEntry.Attributes -band [IO.FileAttributes]::ReparsePoint))) {
            throw 'The installation ownership manifest must be a regular file.'
        }
        $manifestOriginal = if ($manifestEntry) { (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256 -ErrorAction Stop).Hash } else { $null }
        $manifestData = [ordered]@{
            SchemaVersion = 1; Files = $owned; ActiveVersionedExecutable = [IO.Path]::GetFileName($releasePath); AliasKind = $aliasKind
        }
        $saved = $null
        if ($InstallScope) {
            $manifestData.InstallScope = $InstallScope
            $manifestData.InstallUserSid = if ($InstallScope -eq 'CurrentUser') { $InstallUserSid } else { '' }
            $manifestData.SetupGeneration = [Guid]::NewGuid().ToString('N')
        } elseif ($manifestEntry) {
            # File-only development updates do not change registration, but must
            # preserve its account/hive context for a later normal uninstall.
            $saved = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
            $savedScope = [string](Get-LauncherDiscoveryProperty $saved 'InstallScope')
            if ($savedScope) {
                if ($savedScope -cnotin @('AllUsers', 'CurrentUser')) { throw 'The saved installation scope is invalid.' }
                $manifestData.InstallScope = $savedScope
                $manifestData.InstallUserSid = [string](Get-LauncherDiscoveryProperty $saved 'InstallUserSid')
                if ($savedScope -eq 'CurrentUser') { $null = [Security.Principal.SecurityIdentifier]::new($manifestData.InstallUserSid) }
            }
            if ((Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256 -ErrorAction Stop).Hash -ne $manifestOriginal) { throw 'The installation context changed while preparing its update.' }
        }
        $activePlan = $entries | Where-Object IsActive | Select-Object -First 1
        $committedUpdate = $false
        if ($InstallScope -and $AfterActivation -and $activePlan.OriginalHash -and $activePlan.OriginalHash -cne $activePlan.Sha256) {
            # Read the previous version from the verified backup of the exact
            # active bytes. A retained executable or catalog version is no proof
            # of which build was running here.
            $previousMetadata = [Diagnostics.FileVersionInfo]::GetVersionInfo($activePlan.Backup)
            $previousVersion = $null
            try { $previousVersion = Get-LauncherVersionLabel $previousMetadata }
            catch { Write-Verbose ('The previous executable has no verifiable version for recovery: ' + $_.Exception.Message) }
            if ($previousVersion -and $previousVersion -cne $payload.VersionLabel) {
                if ($UpdaterTransactionId -eq [Guid]::Empty) { $UpdaterTransactionId = [Guid]::NewGuid() }
                $manifestData.LastUpdate = [ordered]@{
                    SchemaVersion = 1
                    TransactionId = $UpdaterTransactionId.ToString('N')
                    CommittedUtc = [DateTime]::UtcNow.ToString('O', [Globalization.CultureInfo]::InvariantCulture)
                    Previous = [ordered]@{ Version = $previousVersion; Sha256 = $activePlan.OriginalHash }
                    Current = [ordered]@{ Version = $payload.VersionLabel; Sha256 = $activePlan.Sha256 }
                }
                $committedUpdate = $true
            }
        } elseif ($manifestEntry -and $manifestData['InstallScope'] -and $activePlan.OriginalHash -ceq $activePlan.Sha256) {
            # A same-byte repair can keep the receipt for this scope. An
            # unregistered file replacement must never create a new receipt.
            if (!$saved) {
                try { $saved = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json } catch { }
            }
            $lastUpdate = Get-LauncherDiscoveryProperty $saved 'LastUpdate'
            $currentReceipt = Get-LauncherDiscoveryProperty $lastUpdate 'Current'
            if ((Get-LauncherDiscoveryProperty $lastUpdate 'SchemaVersion') -eq 1 -and
                (Get-LauncherDiscoveryProperty $saved 'InstallScope') -ceq $manifestData.InstallScope -and
                [string](Get-LauncherDiscoveryProperty $saved 'InstallUserSid') -ceq $manifestData.InstallUserSid -and
                (Get-LauncherDiscoveryProperty $currentReceipt 'Version') -ceq $payload.VersionLabel -and
                (Get-LauncherDiscoveryProperty $currentReceipt 'Sha256') -ceq $activePlan.Sha256) {
                $manifestData.LastUpdate = $lastUpdate
            }
            $lastRollback = Get-LauncherDiscoveryProperty $saved 'LastRollback'
            $restored = Get-LauncherDiscoveryProperty $lastRollback 'Restored'
            if (!$manifestData.Contains('LastUpdate') -and (Get-LauncherDiscoveryProperty $lastRollback 'SchemaVersion') -eq 1 -and
                (Get-LauncherDiscoveryProperty $lastRollback 'TransactionId') -cmatch '\A[0-9a-f]{32}\z' -and
                (Get-LauncherDiscoveryProperty $saved 'InstallScope') -ceq $manifestData.InstallScope -and
                [string](Get-LauncherDiscoveryProperty $saved 'InstallUserSid') -ceq $manifestData.InstallUserSid -and
                (Get-LauncherDiscoveryProperty $restored 'Version') -ceq $payload.VersionLabel -and
                (Get-LauncherDiscoveryProperty $restored 'Sha256') -ceq $activePlan.Sha256) {
                $manifestData.LastRollback = $lastRollback
            }
        }
        $review = [pscustomobject]$manifestData['LastUpdate']
        if (!$archiveIdentity -and !$ArchiveRestore -and $InstallScope -and $AfterActivation -and $payload.ArchiveEnabled -and
            $activePlan.OriginalHash -ceq $activePlan.Sha256) {
            $archiveIdentity = Get-LauncherRepairArchiveIdentity $saved $manifestData $payload.VersionLabel $activePlan.Sha256
        }
        if ($ArchiveRestore) {
            $manifestData.Remove('LastUpdate')
            $committedUpdate = $false
            $manifestData.LastRollback = [ordered]@{
                SchemaVersion = 1; TransactionId = $ArchiveRestore.TransactionId
                Restored = [ordered]@{ Version = $payload.VersionLabel; Sha256 = $activePlan.Sha256 }
            }
            foreach ($relative in $ArchiveRestore.Owned.Keys) {
                if ($relative -match '\ALauncher(?:-R[0-9.]+)?\.exe\z' -or $relative -in $plans.RelativePath) { continue }
                $destination = Join-Path $install $relative
                [void](Assert-LauncherSetupDirectory ([IO.Path]::GetDirectoryName($destination)))
                $original = Get-LauncherSetupEntry $destination
                if (!$original -or $original.PSIsContainer -or ($original.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
                    (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -cne $ArchiveRestore.Owned[$relative]) { continue }
                Assert-LauncherSetupReplaceAccess $destination
                $entries.Add([pscustomobject]@{
                    Destination = $destination; Stage = $null; Backup = (Join-Path $transaction ('removed-' + $number))
                    Sha256 = $null; OriginalHash = $ArchiveRestore.Owned[$relative]; Skip = $false
                    Published = $false; IsActive = $false; IsRemoval = $true
                })
                $number++
            }
        }
        if ($archiveIdentity) {
            $manifestData.ArchiveHistory = [ordered]@{ SchemaVersion = 1; Current = $archiveIdentity }
            if (!$ArchiveRestore -and $review) {
                if ($manifestEntry -and !$saved) {
                    try { $saved = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json -ErrorAction Stop } catch { }
                }
                $previousArchive = Get-LauncherPreviousArchiveIdentity $saved $manifestData $review $install $committedUpdate
                if ($previousArchive) {
                    $manifestData.ArchiveHistory.TransactionId = $review.TransactionId
                    $manifestData.ArchiveHistory.Previous = $previousArchive
                }
            }
        }
        if ($QuickRollbackEligible -and $manifestEntry -and
            (Test-LauncherQuickRollbackBuildIdentity (Get-LauncherDiscoveryProperty $review 'Previous')) -and
            (Get-LauncherDiscoveryProperty $review 'TransactionId') -cmatch '\A[0-9a-f]{32}\z' -and
            (Get-LauncherDiscoveryProperty $review 'TransactionId') -cne ('0' * 32) -and
            !(Get-LauncherSetupEntry (Join-Path $install 'examples\demo_many_colors.toml')) -and
            !(Get-LauncherSetupEntry (Join-Path $install 'examples\demo_many_colors.cmd'))) {
            if (!$saved) {
                try { $saved = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json } catch { }
            }
            $retained = Join-Path $install ("Launcher-R$($review.Previous.Version).exe")
            $retainedEntry = Get-LauncherSetupEntry $retained
            $changingVersion = $activePlan.OriginalHash -cne $activePlan.Sha256
            $priorMarker = Get-LauncherDiscoveryProperty $saved 'QuickRollback'
            $previousBuild = if ($changingVersion) { $review.Previous } else { $review.Current }
            if ($retainedEntry -and !$retainedEntry.PSIsContainer -and
                !($retainedEntry.Attributes -band [IO.FileAttributes]::ReparsePoint) -and
                (Get-FileHash -LiteralPath $retained -Algorithm SHA256).Hash -ceq $review.Previous.Sha256 -and
                ($changingVersion -or ((Get-LauncherDiscoveryProperty $priorMarker 'SchemaVersion') -eq 1 -and
                    (Get-LauncherDiscoveryProperty $priorMarker 'TransactionId') -ceq $review.TransactionId)) -and
                (Test-LauncherQuickRollbackCompanions $saved $manifestData $entries $install $previousBuild.Version $previousBuild.Sha256 $review.Current.Version $review.Current.Sha256)) {
                $manifestData.QuickRollback = [ordered]@{ SchemaVersion = 1; TransactionId = $review.TransactionId }
            }
        }
        $manifestJson = $manifestData | ConvertTo-Json -Depth 8
        [IO.File]::WriteAllText($manifestStage, $manifestJson, [Text.UTF8Encoding]::new($false))
        $manifestBackup = $null
        if ($manifestEntry) {
            Invoke-LauncherSetupDestinationAction { Assert-LauncherSetupReplaceAccess $manifestPath }
            $manifestBackup = Join-Path $transaction 'manifest.backup'
            Invoke-LauncherSetupDestinationAction { [IO.File]::Copy($manifestPath, $manifestBackup, $false) }
        }
        $entries.Add([pscustomobject]@{
            Destination = $manifestPath; Stage = $manifestStage; Backup = $manifestBackup
            Sha256 = (Get-FileHash -LiteralPath $manifestStage -Algorithm SHA256 -ErrorAction Stop).Hash
            OriginalHash = $manifestOriginal; Skip = $false; Published = $false; IsActive = $false; IsRemoval = $false
        })
        # Include any legacy active backup created above, but never files that
        # appear after this pre-activation snapshot. Equal/newer versions are
        # excluded even when the selected installation is being downgraded.
        if ($payload.IsRelease -and !$ArchiveRestore) {
            $cleanupCandidates = @(Get-LauncherSupersededExecutables -Directory $install -IncomingVersion ([version]$payload.VersionLabel))
        }
        if ($Progress) { & $Progress 40 'All files verified; preparing to update Launcher' | Out-Null }
        if ($BeforeCommit) { & $BeforeCommit | Out-Null }
        # Parent directories and prior contents are rechecked after the callback:
        # discovery and graceful shutdown can take time and pump the desktop UI.
        foreach ($entry in $entries) {
            [void](Assert-LauncherSetupDirectory ([IO.Path]::GetDirectoryName($entry.Destination)))
            $now = Get-LauncherSetupEntry $entry.Destination
            if (($null -eq $entry.OriginalHash) -ne ($null -eq $now)) {
                throw "Installation contents changed during setup: $($entry.Destination)"
            }
            if ($now -and (Get-FileHash -LiteralPath $entry.Destination -Algorithm SHA256 -ErrorAction Stop).Hash -ne $entry.OriginalHash) {
                throw "Installed file changed during setup: $($entry.Destination)"
            }
            if ($entry.IsActive) { Assert-LauncherSetupActiveEntry $entry.Destination }
            elseif ($now -and ($now.PSIsContainer -or ($now.Attributes -band [IO.FileAttributes]::ReparsePoint))) {
                throw "Installed supporting files must be regular files: $($entry.Destination)"
            }
            $companionName = [IO.Path]::GetFileName($entry.Destination)
            if ($now -and $companions.ContainsKey($companionName)) { Assert-LauncherSetupExecutable $entry.Destination $companions[$companionName] }
        }
        if ($Progress) { & $Progress 60 'Installing the verified program files' | Out-Null }
        if ($ArchiveRestore) {
            Write-LauncherArchiveRestoreJournal -Directory $install -TransactionDirectory $transaction -Entries ($entries.ToArray()) -Plan $ArchiveRestore
            $journaled = $true
        }
        # Publish the active entry last. Each same-volume rename replaces its
        # directory entry atomically; the whole multi-file update is recoverable,
        # rather than a filesystem-wide atomic transaction.
        foreach ($entry in @($entries | Where-Object { !$_.IsActive }) + @($entries | Where-Object IsActive)) {
            if ($entry.Skip) { continue }
            if ($entry.IsRemoval) {
                Move-LauncherSetupFile $entry.Destination $entry.Backup $false
                $entry.Published = $true
                Assert-LauncherQuickRollbackFile $entry.Backup $entry.OriginalHash
                continue
            }
            Move-LauncherSetupFile $entry.Stage $entry.Destination ($null -ne $entry.OriginalHash)
            $entry.Published = $true
            if ((Get-FileHash -LiteralPath $entry.Destination -Algorithm SHA256 -ErrorAction Stop).Hash -ne $entry.Sha256) {
                throw "Installed file failed verification: $($entry.Destination)"
            }
        }
        if ($AfterActivation) { & $AfterActivation $active | Out-Null }
        if ($Progress) { & $Progress 100 'Launcher program files installed' | Out-Null }
        $completed = $true
        if (!$ArchiveRestore) {
            foreach ($warning in @(Remove-LauncherObsoleteStockDemos -InstallDirectory $install)) { $cleanupWarnings.Add($warning) }
        }
        if ($payload.IsRelease -and !$ArchiveRestore) {
            try {
                $currentCandidates = @(Get-LauncherSupersededExecutables -Directory $install -IncomingVersion ([version]$payload.VersionLabel))
                $reviewFallback = if ($InstallScope -and $AfterActivation -and $manifestData.Contains('LastUpdate')) {
                    Get-LauncherReleaseReviewFallback $manifestData.LastUpdate $currentCandidates $previous
                }
                foreach ($candidate in $cleanupCandidates) {
                    try {
                        [void](Assert-LauncherSetupDirectory ([IO.Path]::GetDirectoryName($candidate.Path)))
                        $current = @($currentCandidates | Where-Object { $_.Path -ieq $candidate.Path })
                        $old = Get-LauncherSetupEntry $candidate.Path
                        if ($current.Count -ne 1 -or $current[0].Sha256 -ne $candidate.Sha256 -or $current[0].Version -ne $candidate.Version -or
                            !$old -or $old.PSIsContainer -or ($old.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
                            (Get-FileHash -LiteralPath $candidate.Path -Algorithm SHA256 -ErrorAction Stop).Hash -ne $candidate.Sha256) {
                            throw 'The executable changed or could no longer be verified after activation.'
                        }
                        if ($candidate.Path -ieq $reviewFallback) { continue }
                        [IO.File]::Delete($candidate.Path)
                        if ($previous -ieq $candidate.Path) { $previous = $null }
                    } catch {
                        $cleanupWarnings.Add("Launcher was installed, but an older executable could not be removed: $($candidate.Path). $($_.Exception.Message)")
                    }
                }
            } catch {
                $cleanupWarnings.Add("Launcher was installed, but older executable cleanup could not be verified. $($_.Exception.Message)")
            }
        }
    } catch {
        $failure = $_
        if ($journaled) { throw $failure }
        $rollbackErrors = [Collections.Generic.List[string]]::new()
        for ($index = $entries.Count - 1; $index -ge 0; --$index) {
            $entry = $entries[$index]
            if (!$entry.Published) { continue }
            try {
                if ($entry.IsRemoval) { Move-LauncherSetupFile $entry.Backup $entry.Destination $false }
                elseif ($entry.Backup) { Move-LauncherSetupFile $entry.Backup $entry.Destination $true }
                else {
                    if ((Get-FileHash -LiteralPath $entry.Destination -Algorithm SHA256 -ErrorAction Stop).Hash -ne $entry.Sha256) {
                        throw "A newly installed file changed before rollback: $($entry.Destination)"
                    }
                    [IO.File]::Delete($entry.Destination)
                }
            } catch { $rollbackErrors.Add($_.Exception.Message) }
        }
        if ($rollbackErrors.Count) {
            $recoveryNeeded = $true
            throw "Installation failed: $($failure.Exception.Message) Recovery files retained in $transaction. Rollback: $($rollbackErrors -join '; ')"
        }
        throw $failure
    } finally {
        try { if (!$recoveryNeeded -and !$journaled) {
            foreach ($entry in $entries) {
                foreach ($path in @($entry.Stage, $entry.Backup)) {
                    if ($path -and (!($completed -and $path -eq $previous)) -and (Get-LauncherSetupEntry $path)) {
                        try { [IO.File]::Delete($path) } catch { Write-Warning "Installation recovery entry retained: $path" }
                    }
                }
            }
            # A copy failure can precede adding its entry to the plan. Only this
            # unique transaction directory is enumerated, and only regular files
            # created here are eligible for cleanup.
            if (Get-LauncherSetupEntry $transaction) {
                foreach ($file in @(Get-ChildItem -LiteralPath $transaction -Force -File)) {
                    if (!($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                        try { [IO.File]::Delete($file.FullName) } catch { Write-Warning "Installation staging entry retained: $($file.FullName)" }
                    }
                }
                try { [IO.Directory]::Delete($transaction) } catch { Write-Warning "Installation staging directory retained: $transaction" }
            }
            if (!$completed) {
                if ($previous -and $previous -eq $backupActive -and (Get-LauncherSetupEntry $previous)) {
                    try { [IO.File]::Delete($previous) } catch { Write-Warning "Previous executable retained: $previous" }
                }
                for ($index = $createdDirectories.Count - 1; $index -ge 0; --$index) {
                    try { [IO.Directory]::Delete($createdDirectories[$index]) } catch { }
                }
            }
        } } finally { Exit-LauncherInstallationLock $lock }
    }
    return [pscustomobject]@{
        ActiveExecutable = Join-Path $install 'Launcher.exe'; PreviousExecutable = $previous
        ReleaseExecutable = $releasePath; VersionedExecutable = $releasePath; Sha256 = $payload.Files[0].Sha256
        AliasKind = $aliasKind; LastUpdate = $manifestData['LastUpdate']; CommittedUpdate = $committedUpdate
        CleanupWarnings = @($cleanupWarnings); DistributionKind = $payload.DistributionKind
        FilesInstalled = @($entries | Where-Object { $_.Published }).Count; IsRelease = $payload.IsRelease
    }
}
