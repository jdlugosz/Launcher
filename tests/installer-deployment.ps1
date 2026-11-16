#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][Alias('SourceDirectory')][string]$PackageDirectory,
    [string]$OutputDirectory = [IO.Path]::GetTempPath(),
    [string]$DeploymentScript
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (!$DeploymentScript) { $DeploymentScript = Join-Path $PSScriptRoot '..\tools\installer\Deployment.ps1' }
. $DeploymentScript
. (Join-Path (Split-Path $DeploymentScript -Parent) 'Worker.ps1')
$script:checks = 0
function Check([bool]$Condition, [string]$Message) {
    $script:checks++
    if (!$Condition) { throw $Message }
}
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function WriteText([string]$Path, [string]$Text) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}
function ExpectFailure([scriptblock]$Action, [string]$Message) {
    $failed = $false
    try { & $Action | Out-Null } catch { $failed = $true }
    Check $failed $Message
}
function NoStaging([string]$Directory) {
    Check (@(Get-ChildItem -LiteralPath $Directory -Force -Filter '.launcher-*' -ErrorAction SilentlyContinue).Count -eq 0) 'Installation left transaction/access staging entries.'
}

# Modify only disposable owned PE resources. This lets the deployment backend
# exercise the real metadata reader, revision naming and release policy.
if (!('LauncherSetup.DeploymentVersionFixture' -as [type])) {
    Add-Type @'
using System;
using System.IO;
using System.Text;
using System.Diagnostics;
using System.Runtime.InteropServices;
namespace LauncherSetup {
    public static class DeploymentVersionFixture {
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr BeginUpdateResourceW(string path, bool deleteExisting);
        [DllImport("kernel32.dll", SetLastError=true)] static extern bool UpdateResourceW(IntPtr update, IntPtr type, IntPtr name, ushort language, byte[] bytes, uint size);
        [DllImport("kernel32.dll", SetLastError=true)] static extern bool EndUpdateResourceW(IntPtr update, bool discard);
        static byte[] Node(string key, ushort count, ushort type, byte[] value, params byte[][] children) {
            using (var stream = new MemoryStream()) using (var writer = new BinaryWriter(stream)) {
                writer.Write((ushort)0); writer.Write(count); writer.Write(type);
                writer.Write(Encoding.Unicode.GetBytes(key + "\0"));
                while (stream.Length % 4 != 0) writer.Write((byte)0);
                writer.Write(value);
                foreach (var child in children) {
                    while (stream.Length % 4 != 0) writer.Write((byte)0);
                    writer.Write(child);
                }
                writer.Seek(0, SeekOrigin.Begin); writer.Write(checked((ushort)stream.Length));
                return stream.ToArray();
            }
        }
        static byte[] Text(string key, string value) {
            return Node(key, checked((ushort)(value.Length + 1)), 1, Encoding.Unicode.GetBytes(value + "\0"));
        }
        public static void Configure(string path, string numericVersion, string kind) {
            var old = FileVersionInfo.GetVersionInfo(path);
            var version = Version.Parse(numericVersion);
            byte[] fixedInfo;
            using (var stream = new MemoryStream()) using (var writer = new BinaryWriter(stream)) {
                writer.Write(0xfeef04bdu); writer.Write(0x00010000u);
                uint high = ((uint)version.Major << 16) | (uint)version.Minor;
                uint low = ((uint)Math.Max(0, version.Build) << 16) | (uint)Math.Max(0, version.Revision);
                writer.Write(high); writer.Write(low); writer.Write(high); writer.Write(low);
                writer.Write(0x3fu); writer.Write(kind == "Release" ? 0u : 2u);
                writer.Write(0x00040004u); writer.Write(1u); writer.Write(0u); writer.Write(0u); writer.Write(0u);
                fixedInfo = stream.ToArray();
            }
            var strings = Node("StringFileInfo", 0, 1, new byte[0],
                Node("040904B0", 0, 1, new byte[0],
                    Text("ProductName", old.ProductName), Text("LegalCopyright", old.LegalCopyright),
                    Text("InternalName", old.InternalName), Text("OriginalFilename", old.OriginalFilename),
                    Text("ProductVersion", numericVersion), Text("FileVersion", numericVersion),
                    Text("Comments", "Launcher distribution: " + kind)));
            var translation = Node("VarFileInfo", 0, 1, new byte[0], Node("Translation", 4, 0, BitConverter.GetBytes(0x04b00409u)));
            var bytes = Node("VS_VERSION_INFO", 52, 0, fixedInfo, strings, translation);
            var update = BeginUpdateResourceW(path, false);
            if (update == IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
            bool written = UpdateResourceW(update, (IntPtr)16, (IntPtr)1, 0x0409, bytes, (uint)bytes.Length);
            int error = Marshal.GetLastWin32Error();
            bool ended = EndUpdateResourceW(update, !written);
            if (!written) throw new System.ComponentModel.Win32Exception(error);
            if (!ended) throw new System.ComponentModel.Win32Exception();
        }
    }
}
'@
}

$run = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('launcher-deployment-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($run)
$source = [IO.Path]::GetFullPath($PackageDirectory)
$htmlGuides = @('docs\Launcher.html', 'docs\LauncherDrop.html', 'docs\Installer.html', 'docs\SvgExtractor.html',
    'docs\advanced-icon-catalog.html', 'docs\FileLocations.html',
    'docs\Configuration.html', 'docs\LaunchTargets.html', 'docs\Appearance.html')
function NewPackage([string]$PackageName) {
    $directory = Join-Path $run $PackageName
    [void][IO.Directory]::CreateDirectory($directory)
    [IO.File]::Copy((Get-LauncherPackageExecutable $source), (Join-Path $directory 'Launcher.exe'), $false)
    [IO.File]::Copy((Join-Path $source 'LauncherDrop.exe'), (Join-Path $directory 'LauncherDrop.exe'), $false)
    [IO.File]::Copy((Join-Path $source 'LauncherSvgExtractor.exe'), (Join-Path $directory 'LauncherSvgExtractor.exe'), $false)
    foreach ($name in (@('Launcher.default.toml', 'Launcher.installed.default.toml', 'LICENSE.txt', 'THIRD-PARTY-NOTICES.txt', 'README.txt',
        'docs\Launcher.md', 'docs\LauncherDrop.md', 'docs\Installer.md', 'docs\SvgExtractor.md', 'docs\advanced-icon-catalog.md',
        'examples\basic_colors.toml', 'examples\extra_colors.toml', 'examples\colors-light.toml',
        'examples\colors-dark.toml') + $htmlGuides)) {
        WriteText (Join-Path $directory $name) "$PackageName bundled $name"
    }
    foreach ($name in @('Maintenance.ps1', 'Uninstall.cmd', 'Discovery.ps1', 'Processes.ps1', 'Lifecycle.ps1', 'Deployment.ps1', 'Registration.ps1', 'WindowTextQuery.cs')) {
        [IO.File]::Copy((Join-Path (Split-Path $DeploymentScript -Parent) $name), (Join-Path $directory $name), $false)
    }
    return $directory
}
function NewInstall([string]$Name) { return (Join-Path $run ("install-$Name\Vendor\Launcher")) }

$packageA = NewPackage 'package-A'
$packageB = NewPackage 'package-B'
[IO.File]::AppendAllText((Join-Path $packageB 'Lifecycle.ps1'), "`n# Updated graceful-close dependency fixture.`n")
[LauncherSetup.DeploymentVersionFixture]::Configure((Join-Path $packageA 'Launcher.exe'), '1.1.0.8', 'Prerelease')
[LauncherSetup.DeploymentVersionFixture]::Configure((Join-Path $packageB 'Launcher.exe'), '1.1.0.9', 'Prerelease')
$markedSource = Join-Path $packageA 'Launcher.exe'
Set-Content -LiteralPath $markedSource -Stream Zone.Identifier -Encoding Ascii -Value '[ZoneTransfer]', 'ZoneId=3'
# Appending an overlay changes an owned PE fixture without running it or altering
# the real build. Version resources remain available to the normal identity check.
foreach ($name in @('Launcher.exe', 'LauncherDrop.exe', 'LauncherSvgExtractor.exe')) {
    [IO.File]::AppendAllText((Join-Path $packageB $name), 'owned update fixture')
}
WriteText (Join-Path $packageA 'Launcher.toml') 'never install personal configuration'
WriteText (Join-Path $packageA 'Launcher.runtime.toml') 'never install saved adjustments'
WriteText (Join-Path $packageA 'unlisted.exe') 'never install unlisted content'
$manifestA = Get-LauncherPayload $packageA
Check ($manifestA.Files.Count -eq 34) 'The explicit payload manifest has unexpected entries.'
Check (@($manifestA.Files | Where-Object PreserveExisting).Count -eq 4) 'Only editable palettes should be seed-only.'
Check (@($manifestA.Files | Where-Object RelativePath -Like 'examples\demo_many_colors.*').Count -eq 0) 'Installed payload still includes runnable demo files.'
Check (@($manifestA.Files | Where-Object RelativePath -in @('Launcher.toml', 'Launcher.runtime.toml', 'unlisted.exe')).Count -eq 0) 'Personal/unlisted files entered the payload manifest.'
Check (!$manifestA.IsRelease) 'The deployment check expects the normal development build as its source.'

$fresh = NewInstall 'fresh'
$script:beforeCommitCount = 0
$script:progress = @()
$result = Install-LauncherPayload $packageA $fresh -BeforeCommit {
    $script:beforeCommitCount++
    Check (!(Test-Path -LiteralPath (Join-Path $fresh 'Launcher.exe'))) 'A file was published before BeforeCommit.'
    Check (@(Get-ChildItem -LiteralPath $fresh -Directory -Force -Filter '.launcher-setup-*').Count -eq 1) 'Payload was not staged before BeforeCommit.'
} -Progress { param($Percent, $Message) $script:progress += $Percent }
Check ($script:beforeCommitCount -eq 1) 'The publication callback must run exactly once.'
Check ($script:progress[-1] -eq 100) 'Installation did not report completion.'
Check ($result.FilesInstalled -eq 36 -and !$result.IsRelease -and !$result.PreviousExecutable) 'Fresh installation result is incorrect.'
Check ((Hash (Join-Path $fresh 'Lifecycle.ps1')) -ceq (Hash (Join-Path $packageA 'Lifecycle.ps1'))) 'Fresh installation omitted the graceful-close dependency.'
Check ((Hash (Join-Path $fresh 'Launcher.installed.default.toml')) -ceq (Hash (Join-Path $packageA 'Launcher.installed.default.toml'))) 'Fresh installation omitted the installed starter.'
Check ((Hash $result.ActiveExecutable) -eq $manifestA.Files[0].Sha256) 'Fresh active executable differs from the validated package.'
Check (!((Get-Item -LiteralPath $result.ActiveExecutable).Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Fresh install requires a link privilege.'
Check (!(Test-Path -LiteralPath (Join-Path $fresh 'Launcher.toml'))) 'The package configuration was copied over personal configuration.'
Check (!(Test-Path -LiteralPath (Join-Path $fresh 'Launcher.runtime.toml'))) 'Package runtime settings were installed.'
Check ([IO.Path]::GetFileName($result.VersionedExecutable) -eq 'Launcher-R1.1.0.8.exe') 'The numeric fourth component was omitted from the installed filename.'
Check ($result.AliasKind -in @('HardLink', 'Copy')) 'Installation did not report its alias type.'
if ($result.AliasKind -eq 'HardLink') {
    Check ((Get-Item -LiteralPath $result.ActiveExecutable).LinkType -eq 'HardLink') 'The reported hard-link alias is not linked.'
} else { Write-Output 'Hard-link alias check skipped: this test token/filesystem selected the verified-copy fallback.' }
Check ((Hash $result.VersionedExecutable) -eq (Hash $result.ActiveExecutable)) 'The hard-link alias differs from its immutable target.'
Check (!(Get-Item -LiteralPath $result.VersionedExecutable -Stream Zone.Identifier -ErrorAction SilentlyContinue)) 'The installed versioned executable kept its download marker.'
Check (!(Get-Item -LiteralPath $result.ActiveExecutable -Stream Zone.Identifier -ErrorAction SilentlyContinue)) 'The active alias kept its download marker.'
Check ([bool](Get-Item -LiteralPath $markedSource -Stream Zone.Identifier)) 'Setup removed the marker from the extracted package.'
Set-Content -LiteralPath $result.VersionedExecutable -Stream Zone.Identifier -Encoding Ascii -Value '[ZoneTransfer]', 'ZoneId=3'
$reinstalled = Install-LauncherPayload $packageA $fresh
Check (!(Get-Item -LiteralPath $reinstalled.VersionedExecutable -Stream Zone.Identifier -ErrorAction SilentlyContinue)) 'Setup retained the marker on an identical existing version.'
Check (!(Get-Item -LiteralPath $reinstalled.ActiveExecutable -Stream Zone.Identifier -ErrorAction SilentlyContinue)) 'Reactivation inherited the download marker from the existing version.'
NoStaging $fresh

WriteText (Join-Path $fresh 'Launcher.toml') 'personal portable configuration'
WriteText (Join-Path $fresh 'Launcher.runtime.toml') 'personal runtime adjustments'
WriteText (Join-Path $fresh 'examples\colors-light.toml') 'hand edited palette'
$personal = @{}
foreach ($name in @('Launcher.toml', 'Launcher.runtime.toml', 'examples\colors-light.toml')) {
    $personal[$name] = Hash (Join-Path $fresh $name)
}
$priorHash = Hash (Join-Path $fresh 'Launcher.exe')
$updated = Install-LauncherPayload $packageB $fresh
Check ((Hash $updated.ActiveExecutable) -eq (Hash (Join-Path $packageB 'Launcher.exe'))) 'Upgrade did not activate the new executable.'
Check ((Hash $updated.PreviousExecutable) -eq $priorHash) 'Upgrade did not preserve the prior active executable.'
foreach ($name in $personal.Keys) {
    Check ((Hash (Join-Path $fresh $name)) -eq $personal[$name]) "Upgrade changed $name."
}
$ownership = [IO.File]::ReadAllText((Join-Path $fresh 'Launcher.install-manifest.json')) | ConvertFrom-Json
Check ($ownership.ActiveVersionedExecutable -eq 'Launcher-R1.1.0.9.exe') 'The ownership manifest did not identify the current active version.'
Check (@($ownership.Files | Where-Object RelativePath -in @('Launcher.toml', 'Launcher.runtime.toml', 'examples\colors-light.toml')).Count -eq 0) 'The ownership manifest claimed personal settings or an edited palette.'
Check (@($ownership.Files | Where-Object RelativePath -eq 'Maintenance.ps1').Count -eq 1) 'The installed maintenance entry was omitted from the ownership manifest.'
Check ((Hash (Join-Path $fresh 'Lifecycle.ps1')) -ceq (Hash (Join-Path $packageB 'Lifecycle.ps1'))) 'Upgrade omitted or changed the graceful-close dependency.'
$ownedLifecycle = @($ownership.Files | Where-Object RelativePath -EQ 'Lifecycle.ps1')
Check ($ownedLifecycle.Count -eq 1 -and $ownedLifecycle[0].Sha256 -ceq (Hash (Join-Path $fresh 'Lifecycle.ps1'))) 'Ownership omitted the installed graceful-close dependency.'
Check ([IO.File]::ReadAllText((Join-Path $fresh 'docs\Launcher.md')) -eq 'package-B bundled docs\Launcher.md') 'Upgrade did not refresh owned documentation.'
foreach ($name in $htmlGuides) {
    Check ((Hash (Join-Path $fresh $name)) -eq (Hash (Join-Path $packageB $name))) "Upgrade did not refresh the HTML guide: $name"
    $ownedGuide = @($ownership.Files | Where-Object RelativePath -EQ $name)
    Check ($ownedGuide.Count -eq 1 -and $ownedGuide[0].Sha256 -eq (Hash (Join-Path $fresh $name))) "Ownership omitted the current HTML guide: $name"
}
Check ((Hash (Join-Path $fresh 'LauncherSvgExtractor.exe')) -eq (Hash (Join-Path $packageB 'LauncherSvgExtractor.exe'))) 'Upgrade did not refresh the SVG Extractor.'
foreach ($name in @('LauncherSvgExtractor.exe', 'docs\SvgExtractor.md', 'docs\advanced-icon-catalog.md')) {
    Check (@($ownership.Files | Where-Object RelativePath -EQ $name).Count -eq 1) "Ownership omitted the bundled SVG Extractor file: $name"
}
NoStaging $fresh

# Existing executables are independently identified by the payload backend.
# A valid PE with the other component's metadata is still a foreign occupant of
# this filename, even when no instance of it is running.
foreach ($foreign in @(
    @{ Destination = 'LauncherDrop.exe'; Source = 'Launcher.exe' },
    @{ Destination = 'LauncherSvgExtractor.exe'; Source = 'Launcher.exe' },
    @{ Destination = 'Launcher.exe'; Source = 'LauncherDrop.exe' }
)) {
    $foreignInstall = NewInstall ('foreign-' + $foreign.Destination)
    [void][IO.Directory]::CreateDirectory($foreignInstall)
    $occupied = Join-Path $foreignInstall $foreign.Destination
    [IO.File]::Copy((Join-Path $packageA $foreign.Source), $occupied, $false)
    $occupiedHash = Hash $occupied
    $script:foreignCallback = $false
    ExpectFailure {
        Install-LauncherPayload $packageB $foreignInstall -BeforeCommit { $script:foreignCallback = $true }
    } "A foreign existing $($foreign.Destination) was replaced."
    Check (!$script:foreignCallback) "A foreign $($foreign.Destination) was detected after application shutdown."
    Check ((Hash $occupied) -eq $occupiedHash) "Setup modified the foreign $($foreign.Destination)."
    Check (@(Get-ChildItem -LiteralPath $foreignInstall -Force).Count -eq 1) 'A foreign executable rejection left additional installed files.'
    NoStaging $foreignInstall
}

$missing = NewPackage 'missing-file'
[IO.File]::Delete((Join-Path $missing 'LauncherDrop.exe'))
$missingInstall = NewInstall 'missing-file'
ExpectFailure { Install-LauncherPayload $missing $missingInstall } 'An incomplete package was installed.'
Check (!(Test-Path -LiteralPath $missingInstall)) 'An invalid payload changed the destination.'
$missingExtractor = NewPackage 'missing-extractor'
[IO.File]::Delete((Join-Path $missingExtractor 'LauncherSvgExtractor.exe'))
ExpectFailure { Get-LauncherPayload $missingExtractor } 'A package without the bundled SVG Extractor was accepted.'
$missingGuide = NewPackage 'missing-html-guide'
[IO.File]::Delete((Join-Path $missingGuide 'docs\LaunchTargets.html'))
$missingGuideInstall = NewInstall 'missing-html-guide'
ExpectFailure { Install-LauncherPayload $missingGuide $missingGuideInstall } 'A package without a required HTML guide was installed.'
Check (!(Test-Path -LiteralPath $missingGuideInstall)) 'A missing HTML guide changed the installation before validation completed.'
$legacyGuidePackage = NewPackage 'older-combined-html-guide'
foreach ($name in @('docs\Configuration.html', 'docs\LaunchTargets.html', 'docs\Appearance.html')) {
    [IO.File]::Delete((Join-Path $legacyGuidePackage $name))
}
Check ((Get-LauncherPayload $legacyGuidePackage).Files.Count -eq 31) 'An older package with the combined HTML guide was rejected.'
$missingLifecycle = NewPackage 'missing-lifecycle'
[IO.File]::Delete((Join-Path $missingLifecycle 'Lifecycle.ps1'))
$missingLifecycleInstall = NewInstall 'missing-lifecycle'
ExpectFailure { Install-LauncherPayload $missingLifecycle $missingLifecycleInstall } 'A package without the graceful-close dependency was installed.'
Check (!(Test-Path -LiteralPath $missingLifecycleInstall)) 'A missing graceful-close dependency changed the destination before validation completed.'
$missingInstalledStarter = NewPackage 'missing-installed-starter'
[IO.File]::Delete((Join-Path $missingInstalledStarter 'Launcher.installed.default.toml'))
$missingStarterInstall = NewInstall 'missing-installed-starter'
ExpectFailure { Install-LauncherPayload $missingInstalledStarter $missingStarterInstall } 'A package without the installed starter was installed.'
Check (!(Test-Path -LiteralPath $missingStarterInstall)) 'A missing installed starter changed the destination before validation completed.'
foreach ($destination in @($packageA, (Join-Path $packageA 'nested'), $run)) {
    ExpectFailure { Install-LauncherPayload $packageA $destination } 'Overlapping source/destination trees were accepted.'
}

$cancelled = NewInstall 'callback-failure'
ExpectFailure { Install-LauncherPayload $packageA $cancelled -BeforeCommit { throw 'owned callback failure' } } 'A failed BeforeCommit callback was ignored.'
Check (!(Test-Path -LiteralPath $cancelled)) 'A pre-publication failure left a fresh application directory.'

# A locked helper is detected during preflight, before the application-shutdown
# callback is allowed to run. No process is terminated to unblock it.
$script:lockCallback = $false
$helperLock = [IO.File]::Open((Join-Path $fresh 'LauncherDrop.exe'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    ExpectFailure { Install-LauncherPayload $packageA $fresh -BeforeCommit { $script:lockCallback = $true } } 'A locked helper was replaced.'
} finally { $helperLock.Dispose() }
Check (!$script:lockCallback) 'Launcher was asked to stop before detecting the locked helper.'
Check ((Hash (Join-Path $fresh 'Launcher.exe')) -eq (Hash (Join-Path $packageB 'Launcher.exe'))) 'A locked helper changed the active executable.'
NoStaging $fresh

$extractorLock = [IO.File]::Open((Join-Path $fresh 'LauncherSvgExtractor.exe'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
$script:lockCallback = $false
try {
    ExpectFailure { Install-LauncherPayload $packageA $fresh -BeforeCommit { $script:lockCallback = $true } } 'A locked SVG Extractor was replaced.'
} finally { $extractorLock.Dispose() }
Check (!$script:lockCallback) 'Setup stopped Launcher before detecting a locked SVG Extractor.'
Check ((Hash (Join-Path $fresh 'Launcher.exe')) -eq (Hash (Join-Path $packageB 'Launcher.exe'))) 'A locked SVG Extractor changed the active main executable.'
NoStaging $fresh

# Locking active after preflight forces a failure at the last publication step.
# Earlier program/support changes, including every HTML guide, must all roll
# back to their original bytes and restore the previous ownership receipt.
$before = @{}
foreach ($file in (Get-LauncherPayload $packageB).Files) { $before[$file.RelativePath] = Hash (Join-Path $fresh $file.RelativePath) }
$beforeManifest = Hash (Join-Path $fresh 'Launcher.install-manifest.json')
$script:activeLock = $null
try {
    ExpectFailure {
        Install-LauncherPayload $packageA $fresh -BeforeCommit {
            $script:activeLock = [IO.File]::Open((Join-Path $fresh 'Launcher.exe'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        }
    } 'A final active-executable lock did not fail installation.'
} finally { if ($script:activeLock) { $script:activeLock.Dispose() } }
foreach ($name in $before.Keys) {
    Check ((Hash (Join-Path $fresh $name)) -eq $before[$name]) "Rollback failed to restore $name."
}
Check ((Hash (Join-Path $fresh 'Launcher.install-manifest.json')) -eq $beforeManifest) 'Rollback failed to restore HTML ownership metadata.'
NoStaging $fresh

# Exercise rollback after the new active entry has actually been published.
ExpectFailure {
    Install-LauncherPayload $packageA $fresh -Progress {
        param($Percent, $Message)
        if ($Percent -eq 100) { throw 'owned completion reporting failure' }
    }
} 'A completion reporting failure did not reach transaction rollback.'
foreach ($name in $before.Keys) {
    Check ((Hash (Join-Path $fresh $name)) -eq $before[$name]) "Rollback after activation failed to restore $name."
}
Check ((Hash (Join-Path $fresh 'Launcher.install-manifest.json')) -eq $beforeManifest) 'Rollback after activation failed to restore HTML ownership metadata.'
NoStaging $fresh

# A directory junction in either source or destination ancestry cannot redirect
# package reads or elevated writes into a different tree.
$outside = Join-Path $run 'outside'
[void][IO.Directory]::CreateDirectory($outside)
$junction = Join-Path $run 'junction'
[void](New-Item -ItemType Junction -Path $junction -Target $outside)
ExpectFailure { Install-LauncherPayload $packageA (Join-Path $junction 'Launcher') } 'A junction destination ancestor was accepted.'
Check (@(Get-ChildItem -LiteralPath $outside -Force).Count -eq 0) 'Unsafe destination validation wrote into the junction target.'
$linkedPackage = NewPackage 'linked-package'
$linkedDocs = Join-Path $linkedPackage 'docs'
foreach ($file in @(Get-ChildItem -LiteralPath $linkedDocs -File)) { [IO.File]::Delete($file.FullName) }
[IO.Directory]::Delete($linkedDocs)
[void](New-Item -ItemType Junction -Path $linkedDocs -Target $outside)
ExpectFailure { Get-LauncherPayload $linkedPackage } 'A source payload junction was accepted.'

# Preserve the old relative-release link's target while replacing the link entry.
# This does not depend on Developer Mode: a test link may be unavailable, while
# ordinary installs above always use regular active entries.
if (!('LauncherSetup.DeploymentFixture' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
namespace LauncherSetup {
    public static class DeploymentFixture {
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        [return: MarshalAs(UnmanagedType.I1)] public static extern bool CreateSymbolicLinkW(string path, string target, uint flags);
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
        [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);
        [DllImport("kernel32.dll", SetLastError=true)] static extern uint SizeofResource(IntPtr module, IntPtr resource);
        [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
        [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr LockResource(IntPtr resource);
        [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr module);
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr BeginUpdateResourceW(string path, bool deleteExisting);
        [DllImport("kernel32.dll", SetLastError=true)] static extern bool UpdateResourceW(IntPtr update, IntPtr type, IntPtr name, ushort language, byte[] bytes, uint size);
        [DllImport("kernel32.dll", SetLastError=true)] static extern bool EndUpdateResourceW(IntPtr update, bool discard);
        public static void MarkRelease(string path) {
            byte[] bytes;
            var module = LoadLibraryExW(path, IntPtr.Zero, 0x22);
            if (module == IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
            try {
                var resource = FindResourceW(module, (IntPtr)1, (IntPtr)16);
                uint size = SizeofResource(module, resource);
                var value = LockResource(LoadResource(module, resource));
                if (size == 0 || value == IntPtr.Zero) throw new InvalidOperationException("Missing version resource.");
                bytes = new byte[checked((int)size)];
                Marshal.Copy(value, bytes, 0, bytes.Length);
            } finally { FreeLibrary(module); }
            int offset = 6;
            while (offset + 1 < bytes.Length && (bytes[offset] != 0 || bytes[offset + 1] != 0)) offset += 2;
            offset = (offset + 5) & ~3;
            if (offset + 52 > bytes.Length || BitConverter.ToUInt32(bytes, offset) != 0xfeef04bd) throw new InvalidOperationException("Invalid version resource.");
            Array.Copy(BitConverter.GetBytes((uint)0x00010002), 0, bytes, offset + 16, 4);
            Array.Copy(BitConverter.GetBytes((uint)0), 0, bytes, offset + 20, 4);
            Array.Copy(BitConverter.GetBytes((uint)0x3f), 0, bytes, offset + 24, 4);
            Array.Copy(BitConverter.GetBytes((uint)0), 0, bytes, offset + 28, 4);
            var update = BeginUpdateResourceW(path, false);
            if (update == IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
            bool written = UpdateResourceW(update, (IntPtr)16, (IntPtr)1, 0x0409, bytes, (uint)bytes.Length);
            int error = Marshal.GetLastWin32Error();
            bool ended = EndUpdateResourceW(update, !written);
            if (!written) throw new System.ComponentModel.Win32Exception(error);
            if (!ended) throw new System.ComponentModel.Win32Exception();
        }
    }
}
'@
}
$linkInstall = NewInstall 'relative-link'
[void][IO.Directory]::CreateDirectory($linkInstall)
$oldRelease = Join-Path $linkInstall 'Launcher-R1.0.exe'
[IO.File]::Copy((Join-Path $packageA 'Launcher.exe'), $oldRelease, $false)
$oldReleaseHash = Hash $oldRelease
$linkActive = Join-Path $linkInstall 'Launcher.exe'
if ([LauncherSetup.DeploymentFixture]::CreateSymbolicLinkW($linkActive, 'Launcher-R1.0.exe', 2) -or
    [LauncherSetup.DeploymentFixture]::CreateSymbolicLinkW($linkActive, 'Launcher-R1.0.exe', 0)) {
    $linkResult = Install-LauncherPayload $packageB $linkInstall
    Check ((Hash $oldRelease) -eq $oldReleaseHash) 'Replacing the active link overwrote the preserved release target.'
    Check ((Hash $linkResult.PreviousExecutable) -eq $oldReleaseHash) 'The prior relative-link executable was not preserved.'
    Check (!((Get-Item -LiteralPath $linkActive).Attributes -band [IO.FileAttributes]::ReparsePoint)) 'The active link was not replaced by a regular executable.'
} else { Write-Output 'Relative-link migration check skipped: this test token cannot create a symbolic link.' }

$releasePackage = NewPackage 'release-package'
[LauncherSetup.DeploymentVersionFixture]::Configure((Join-Path $releasePackage 'Launcher.exe'), '1.2.0.0', 'Release')
$releaseManifest = Get-LauncherPayload $releasePackage
Check ($releaseManifest.IsRelease -and $releaseManifest.ReleaseLabel -eq '1.2') 'The owned release resource fixture was not recognized.'
$releaseInstall = NewInstall 'release'
$releaseResult = Install-LauncherPayload $releasePackage $releaseInstall
$releaseHash = Hash $releaseResult.ReleaseExecutable
Check ((Hash $releaseResult.ActiveExecutable) -eq $releaseHash) 'A release install did not preserve identical immutable executable bytes.'
$null = Install-LauncherPayload $releasePackage $releaseInstall
Check ((Hash $releaseResult.ReleaseExecutable) -eq $releaseHash) 'A repeated release install modified its preserved executable.'
[IO.File]::AppendAllText((Join-Path $releasePackage 'Launcher.exe'), 'conflicting release fixture')
$script:conflictCallback = $false
ExpectFailure { Install-LauncherPayload $releasePackage $releaseInstall -BeforeCommit { $script:conflictCallback = $true } } 'A conflicting immutable release was replaced.'
Check (!$script:conflictCallback) 'A release conflict was detected after application shutdown.'
Check ((Hash $releaseResult.ReleaseExecutable) -eq $releaseHash) 'A release conflict modified preserved bytes.'
NoStaging $releaseInstall

# The versioned ZIP source is resolved by metadata rather than a hard-coded name.
$versionedPackage = NewPackage 'versioned-source'
[LauncherSetup.DeploymentVersionFixture]::Configure((Join-Path $versionedPackage 'Launcher.exe'), '1.1.0.10', 'Prerelease')
$versionedSource = Join-Path $versionedPackage 'Launcher-R1.1.0.10.exe'
[IO.File]::Move((Join-Path $versionedPackage 'Launcher.exe'), $versionedSource)
Check ((Get-LauncherPayload $versionedPackage).ExecutablePath -eq $versionedSource) 'A four-part versioned ZIP executable was not resolved.'
$copyInstall = NewInstall 'copy-alias'
$copyResult = Install-LauncherPayload $versionedPackage $copyInstall -AliasHardLinkCreator { param($Path, $Target) return $false }
Check ($copyResult.AliasKind -eq 'Copy') 'A failed hard-link attempt did not select the ordinary-copy fallback.'
Check ((Hash $copyResult.ActiveExecutable) -eq (Hash $copyResult.VersionedExecutable)) 'The ordinary-copy alias failed byte verification.'
Check ((Get-Item -LiteralPath $copyResult.ActiveExecutable).LinkType -ne 'HardLink') 'The forced copy fallback unexpectedly produced a hard link.'
$savedVersionHash = Hash $copyResult.VersionedExecutable
[IO.File]::AppendAllText($copyResult.ActiveExecutable, 'disposable alias-only write')
Check ((Hash $copyResult.VersionedExecutable) -eq $savedVersionHash) 'The copy fallback shares writable bytes with its target.'
[IO.File]::Copy($versionedSource, (Join-Path $versionedPackage 'Launcher.exe'), $false)
ExpectFailure { Get-LauncherPayload $versionedPackage } 'An ambiguous package containing both executable names was accepted.'
[IO.File]::Delete((Join-Path $versionedPackage 'Launcher.exe'))
[IO.File]::Move($versionedSource, (Join-Path $versionedPackage 'Launcher-R1.1.0.11.exe'))
ExpectFailure { Get-LauncherPayload $versionedPackage } 'A versioned filename differing from the numeric PE revision was accepted.'

# Explicit Special distributions retain the prior active executable. An absent
# distribution comment also remains conservative even with an even minor.
$specialPackage = NewPackage 'special-package'
[LauncherSetup.DeploymentVersionFixture]::Configure((Join-Path $specialPackage 'Launcher.exe'), '1.2.3.0', 'Special')
$specialInstall = NewInstall 'special'
$specialOld = Install-LauncherPayload $packageA $specialInstall
$specialResult = Install-LauncherPayload $specialPackage $specialInstall
Check (!$specialResult.IsRelease -and $specialResult.DistributionKind -eq 'Special') 'A distributable special build was mistaken for a final release.'
Check ((Hash $specialResult.PreviousExecutable) -eq (Hash $specialOld.VersionedExecutable)) 'A special build removed the prior active version.'
Check ((Get-LauncherDistributionKind -Metadata ([pscustomobject]@{ Comments = ''; IsPreRelease = $false })) -eq 'Prerelease') 'Legacy metadata authorized destructive release cleanup.'

# A final release removes every verified older version. Its source was
# changed above for the conflict check; restore
# its original bytes from the already installed owned release.
[IO.File]::Copy($releaseResult.VersionedExecutable, (Join-Path $releasePackage 'Launcher.exe'), $true)
$olderArchive = $result.VersionedExecutable
$superseded = $updated.VersionedExecutable
$finalResult = Install-LauncherPayload $releasePackage $fresh
Check (!(Test-Path -LiteralPath $superseded)) 'A final release retained the superseded active version.'
Check (!(Test-Path -LiteralPath $olderArchive)) 'A final release retained an older archived version.'
Check (!$finalResult.PreviousExecutable -and !$finalResult.CleanupWarnings.Count) 'A clean final activation reported leftover prior files.'

$ambiguousInstall = NewInstall 'ambiguous-legacy-archives'
[void][IO.Directory]::CreateDirectory($ambiguousInstall)
foreach ($name in @('Launcher.exe', 'Launcher-R1.1.exe', 'Launcher-R1.1.0.8.exe')) {
    [IO.File]::Copy((Join-Path $packageA 'Launcher.exe'), (Join-Path $ambiguousInstall $name), $false)
}
$ambiguousResult = Install-LauncherPayload $releasePackage $ambiguousInstall
Check (!$ambiguousResult.PreviousExecutable) 'A final legacy update retained its disposable prior-active backup.'
Check (@(Get-ChildItem -LiteralPath $ambiguousInstall -Filter 'Launcher-R1.1*.exe').Count -eq 1 -and
    (Test-Path -LiteralPath (Join-Path $ambiguousInstall 'Launcher-R1.1.exe'))) 'Final cleanup did not distinguish a filename/PE mismatch from the verified older version.'

# Locked cleanup is reported after successful activation without rolling back
# the new version or touching another retained executable.
$cleanupInstall = NewInstall 'locked-cleanup'
$cleanupOld = Install-LauncherPayload $packageA $cleanupInstall
$script:cleanupLock = $null
try {
    $cleanupResult = Install-LauncherPayload $releasePackage $cleanupInstall -Progress {
        param($Percent, $Message)
        if ($Percent -eq 100) {
            $script:cleanupLock = [IO.File]::Open($cleanupOld.VersionedExecutable, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        }
    }
    Check ($cleanupResult.IsRelease -and $cleanupResult.CleanupWarnings.Count -eq 1) 'Locked cleanup did not return a successful activation with a warning.'
    Check ((Hash $cleanupResult.ActiveExecutable) -eq (Hash $releaseResult.ActiveExecutable)) 'A cleanup warning rolled back the activated release.'
    Check ((Hash $cleanupResult.PreviousExecutable) -eq (Hash $cleanupOld.VersionedExecutable)) 'Locked cleanup lost the previous executable.'
} finally { if ($script:cleanupLock) { $script:cleanupLock.Dispose() } }

# Legacy unversioned active files are retained as fresh backup copies, with no
# write through their aliases and no collision with an immutable archive.
$legacyInstall = NewInstall 'legacy-active'
[void][IO.Directory]::CreateDirectory($legacyInstall)
[IO.File]::Copy((Join-Path $packageA 'Launcher.exe'), (Join-Path $legacyInstall 'Launcher.exe'), $false)
$legacyResult = Install-LauncherPayload $packageB $legacyInstall
Check ($legacyResult.PreviousExecutable -like '*.backup.exe' -and (Hash $legacyResult.PreviousExecutable) -eq (Hash (Join-Path $packageA 'Launcher.exe'))) 'A legacy active executable was not preserved before installing a prerelease.'
NoStaging $legacyInstall

$scopedInstall = NewInstall 'preserve-registration-scope'
$scopeSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$scopedManifestPath = Join-Path $scopedInstall 'Launcher.install-manifest.json'
$markerPath = Join-Path $run '.launcher-updater-transaction'
$transactionId = [Guid]::NewGuid()
$markerText = $transactionId.ToString('N')
WriteText $markerPath $markerText
Check ((Get-LauncherUpdaterTransactionId $packageB) -eq [Guid]::Empty) 'A marker without the outer ZIP entry point was accepted.'
WriteText (Join-Path $run 'Install.cmd') '@echo off'
foreach ($invalid in @('', ('0' * 32), ('A' * 32), ($markerText + [char]10), "$markerText ", ('a' * 31))) {
    WriteText $markerPath $invalid
    Check ((Get-LauncherUpdaterTransactionId $packageB) -eq [Guid]::Empty) 'A malformed Updater transaction marker was accepted.'
}
[IO.File]::Delete($markerPath)
Check ((Get-LauncherUpdaterTransactionId $packageB) -eq [Guid]::Empty) 'An absent marker supplied a transaction ID.'
[void][IO.Directory]::CreateDirectory($markerPath)
Check ((Get-LauncherUpdaterTransactionId $packageB) -eq [Guid]::Empty) 'A marker directory supplied a transaction ID.'
[IO.Directory]::Delete($markerPath)
$markerTarget = Join-Path $run 'marker-link-target'
WriteText $markerTarget $markerText
if ([LauncherSetup.DeploymentFixture]::CreateSymbolicLinkW($markerPath, $markerTarget, 2) -or
    [LauncherSetup.DeploymentFixture]::CreateSymbolicLinkW($markerPath, $markerTarget, 0)) {
    Check ((Get-LauncherUpdaterTransactionId $packageB) -eq [Guid]::Empty) 'A marker reparse point supplied a transaction ID.'
    [IO.File]::Delete($markerPath)
}
WriteText $markerPath $markerText
$markerId = Get-LauncherUpdaterTransactionId $packageB
Check ($markerId -eq $transactionId) 'The exact Updater transaction marker was not read.'
$scopedFresh = Install-LauncherPayload $packageA $scopedInstall -InstallScope CurrentUser -InstallUserSid $scopeSid -AfterActivation { } -UpdaterTransactionId $markerId
$scopedManifest = [IO.File]::ReadAllText($scopedManifestPath) | ConvertFrom-Json
Check (!$scopedFresh.CommittedUpdate -and $null -eq $scopedManifest.PSObject.Properties['LastUpdate']) 'Fresh Setup recorded a previous installation.'
$scopedPreviousHash = Hash (Join-Path $scopedInstall 'Launcher.exe')
$scopedUpdate = Install-LauncherPayload $packageB $scopedInstall -InstallScope CurrentUser -InstallUserSid $scopeSid -AfterActivation { } -UpdaterTransactionId $markerId
$scopedManifest = [IO.File]::ReadAllText($scopedManifestPath) | ConvertFrom-Json
$scopedActiveHash = Hash (Join-Path $scopedInstall 'Launcher.exe')
Check ($scopedManifest.InstallScope -ceq 'CurrentUser' -and $scopedManifest.InstallUserSid -ceq $scopeSid) 'The update receipt has the wrong scope or account context.'
$lastUpdate = $scopedManifest.LastUpdate
Check ($scopedUpdate.CommittedUpdate -and $lastUpdate.SchemaVersion -eq 1 -and $lastUpdate.TransactionId -ceq $markerText) 'The update receipt did not adopt the Updater transaction ID.'
Check (([DateTimeOffset]$lastUpdate.CommittedUtc).Offset -eq [TimeSpan]::Zero) 'The update receipt time is not UTC.'
Check ($lastUpdate.Previous.Version -ceq '1.1.0.8' -and $lastUpdate.Previous.Sha256 -ceq $scopedPreviousHash -and
    $lastUpdate.Current.Version -ceq '1.1.0.9' -and $lastUpdate.Current.Sha256 -ceq $scopedActiveHash) 'The update receipt did not identify the actual prior and new bytes.'
$startupRequest = [pscustomobject]@{
    PackageDirectory = $packageB; DestinationDirectory = $scopedInstall
    Scope = 'CurrentUser'; UpdaterTransactionId = $markerText
}
$startupMarkerPath = Join-Path $run '.launcher-updater-watch-startup'
$startupPlanPath = Join-Path $run '.launcher-updater-startup-plan.json'
# Exercise the same JSON round trip the wizard receives from the worker.
$startupDeployment = $scopedUpdate | ConvertTo-Json -Depth 10 | ConvertFrom-Json
$restartArguments = @('--config "C:\Test profile\drawer.toml"', ('--config "C:\' + [char]0x0141 + '\second.toml"'))
Check (!(Write-LauncherUpdaterStartupPlan $startupRequest $startupDeployment $restartArguments)) 'An older Updater transaction unexpectedly took over launch.'
WriteText $startupMarkerPath ([Guid]::NewGuid().ToString('N'))
Check (!(Write-LauncherUpdaterStartupPlan $startupRequest $startupDeployment $restartArguments)) 'Another transaction took over launch.'
WriteText $startupMarkerPath ($markerText + [char]10)
Check (!(Write-LauncherUpdaterStartupPlan $startupRequest $startupDeployment $restartArguments)) 'A malformed opt-in marker took over launch.'
WriteText $startupMarkerPath $markerText
$startupDeployment.LastUpdate.TransactionId = [Guid]::NewGuid().ToString('N')
Check (!(Write-LauncherUpdaterStartupPlan $startupRequest $startupDeployment $restartArguments)) 'An unrelated committed receipt took over launch.'
$startupDeployment.LastUpdate.TransactionId = $markerText
Check (!(Write-LauncherUpdaterStartupPlan $startupRequest $startupDeployment @('x' * 30001))) 'An argument above the protocol limit took over launch.'
Check (!(Write-LauncherUpdaterStartupPlan $startupRequest $startupDeployment (@('') * 129))) 'An array above the protocol limit took over launch.'
Check (!(Test-Path -LiteralPath $startupPlanPath)) 'An unsupported handoff created a startup plan.'
Check (Write-LauncherUpdaterStartupPlan $startupRequest $startupDeployment $restartArguments) 'The committed opt-in update did not hand launch to Updater.'
$startupPlan = [IO.File]::ReadAllText($startupPlanPath) | ConvertFrom-Json
Check ($startupPlan.SchemaVersion -eq 1 -and $startupPlan.TransactionId -ceq $markerText -and
    $startupPlan.Scope -ceq 'CurrentUser' -and $startupPlan.RestartArguments -is [array] -and
    $startupPlan.RestartArguments.Count -eq 2 -and $startupPlan.RestartArguments[0] -ceq $restartArguments[0] -and
    $startupPlan.RestartArguments[1] -ceq $restartArguments[1] -and
    ($startupPlan.PSObject.Properties.Name -join ',') -ceq 'SchemaVersion,TransactionId,Scope,RestartArguments' -and
    [IO.File]::ReadAllBytes($startupPlanPath)[0] -eq [byte][char]'{') 'The startup plan changed the restart arguments, included an executable path or had an unexpected encoding.'
$startupPlanHash = Hash $startupPlanPath
ExpectFailure { Write-LauncherUpdaterStartupPlan $startupRequest $startupDeployment @() } 'Setup replaced an existing startup plan.'
Check ((Hash $startupPlanPath) -ceq $startupPlanHash -and
    @(Get-ChildItem -LiteralPath $run -Force -Filter '.launcher-updater-startup-plan-*.tmp').Count -eq 0) 'A failed handoff changed the plan or left a temporary file.'
[IO.File]::Delete($startupPlanPath)
Check (Write-LauncherUpdaterStartupPlan $startupRequest $startupDeployment @()) 'Setup did not hand off the decision to leave Launcher closed.'
$startupPlan = [IO.File]::ReadAllText($startupPlanPath) | ConvertFrom-Json
Check ($startupPlan.RestartArguments -is [array] -and $startupPlan.RestartArguments.Count -eq 0) 'Declining startup must produce an empty argument array.'
[IO.File]::Delete($startupPlanPath)
Check (Write-LauncherUpdaterStartupPlan $startupRequest $startupDeployment @('')) 'Setup did not hand off the default Launcher invocation.'
$startupPlan = [IO.File]::ReadAllText($startupPlanPath) | ConvertFrom-Json
Check ($startupPlan.RestartArguments -is [array] -and $startupPlan.RestartArguments.Count -eq 1 -and
    $startupPlan.RestartArguments[0] -ceq '') 'A default startup must produce one empty argument string.'
[IO.File]::Delete($startupPlanPath)
$scopedRepair = Install-LauncherPayload $packageB $scopedInstall -InstallScope CurrentUser -InstallUserSid $scopeSid -AfterActivation { } -UpdaterTransactionId $markerId
$scopedManifest = [IO.File]::ReadAllText($scopedManifestPath) | ConvertFrom-Json
Check (!$scopedRepair.CommittedUpdate -and $scopedManifest.LastUpdate.TransactionId -ceq $lastUpdate.TransactionId) 'A same-byte repair replaced the last update receipt.'
Check (!(Write-LauncherUpdaterStartupPlan $startupRequest $scopedRepair $restartArguments)) 'A same-byte repair with a reused transaction marker took over launch.'

# Model already authenticated history. This repair source has been extracted
# elsewhere and no longer has the ZIP or detached signature next to it.
$archiveRepairPackage = Join-Path $run 'archive-repair-payload'
Copy-Item -LiteralPath $packageB -Destination $archiveRepairPackage -Recurse
$installerSource = Split-Path $DeploymentScript -Parent
$archiveRecoverySource = Join-Path $installerSource 'ArchiveRecovery.ps1'
$archiveJournalSource = Join-Path $installerSource 'ArchiveRestoreJournal.ps1'
if (!(Test-Path -LiteralPath $archiveRecoverySource -PathType Leaf)) {
    $archiveRecoverySource = Join-Path $PSScriptRoot '..\tools\installer\ArchiveRecovery.ps1.in'
    $archiveJournalSource = Join-Path $PSScriptRoot '..\tools\installer\ArchiveRestoreJournal.ps1'
}
Copy-Item -LiteralPath $archiveRecoverySource -Destination (Join-Path $archiveRepairPackage 'ArchiveRecovery.ps1')
Copy-Item -LiteralPath $archiveJournalSource -Destination $archiveRepairPackage
WriteText (Join-Path $archiveRepairPackage 'Launcher.archive.json') '{}'
WriteText (Join-Path $archiveRepairPackage 'ArchiveSigner.cer') 'Certificate placeholder, unused without ZIP evidence.'
$archiveRepairInstall = NewInstall 'archive-repair'
$registered = @{ InstallDirectory = $archiveRepairInstall; InstallScope = 'CurrentUser'; InstallUserSid = $scopeSid; AfterActivation = {} }
$null = Install-LauncherPayload $packageA @registered
$null = Install-LauncherPayload $archiveRepairPackage @registered -UpdaterTransactionId $markerId
$archiveManifestPath = Join-Path $archiveRepairInstall 'Launcher.install-manifest.json'
$archiveManifest = [IO.File]::ReadAllText($archiveManifestPath) | ConvertFrom-Json
$history = [ordered]@{
    SchemaVersion = 1
    Current = [ordered]@{ Build = 'launcher-fixture-9'; Platform = 'windows-x64'; Size = 12345; Sha256 = ('B' * 64) }
    TransactionId = $markerText
    Previous = [ordered]@{ Build = 'launcher-fixture-8'; Platform = 'windows-x64'; Size = 12346; Sha256 = ('A' * 64) }
}
$archiveManifest | Add-Member -NotePropertyName ArchiveHistory -NotePropertyValue $history
WriteText $archiveManifestPath ($archiveManifest | ConvertTo-Json -Depth 8)
$null = Install-LauncherPayload $archiveRepairPackage @registered
$repaired = [IO.File]::ReadAllText($archiveManifestPath) | ConvertFrom-Json
Check ($repaired.LastUpdate.TransactionId -ceq $markerText -and
    ($repaired.ArchiveHistory | ConvertTo-Json -Depth 4 -Compress) -ceq ($history | ConvertTo-Json -Depth 4 -Compress)) 'Extracted same-byte repair discarded the authenticated archive history.'
[IO.File]::AppendAllText((Join-Path $archiveRepairPackage 'Lifecycle.ps1'), "`n# Different managed file.")
$null = Install-LauncherPayload $archiveRepairPackage @registered
$repaired = [IO.File]::ReadAllText($archiveManifestPath) | ConvertFrom-Json
Check ($repaired.LastUpdate.TransactionId -ceq $markerText -and !$repaired.PSObject.Properties['ArchiveHistory']) 'A repair with different managed bytes retained unrelated archive evidence.'

# Make two genuine archives with separate in-memory signing keys, but use the
# production verification, cleanup, repair and archive restore entry points.
function New-SignedReviewPackage([string]$Version, [string]$Kind, [byte[]]$Certificate,
    [Security.Cryptography.RSA]$Key, [byte[]]$AdditionalCertificate) {
    $build = 'launcher-fixture-' + $Version.Replace('.', '-')
    $root = Join-Path $run $build
    $payloadRoot = Join-Path $root 'payload'
    $package = Join-Path $payloadRoot "Launcher-R$Version-windows-x64"
    [void][IO.Directory]::CreateDirectory($payloadRoot)
    $sourcePackage = NewPackage "source-$build"
    Move-Item -LiteralPath $sourcePackage -Destination $package
    $main = Join-Path $package 'Launcher.exe'
    [LauncherSetup.DeploymentVersionFixture]::Configure($main, $Version, $Kind)
    [IO.File]::Move($main, (Join-Path $package "Launcher-R$Version.exe"))
    [IO.File]::AppendAllText((Join-Path $package 'LauncherSvgExtractor.exe'), $build)
    $certPath = Join-Path $package 'ArchiveSigner.cer'
    [IO.File]::WriteAllBytes($certPath, $Certificate)
    $recovery = [IO.File]::ReadAllText($archiveRecoverySource).Replace($archiveSourceSignerHash, (Hash $certPath))
    $additionalHex = if ($AdditionalCertificate) { [BitConverter]::ToString($AdditionalCertificate).Replace('-', '') } else { '' }
    $recovery = $recovery -creplace '(?m)^(    \$additionalCertificateHex = )''[^'']*''', ('${1}''' + $additionalHex + '''')
    WriteText (Join-Path $package 'ArchiveRecovery.ps1') $recovery
    Copy-Item -LiteralPath $archiveJournalSource -Destination $package
    WriteText (Join-Path $package 'Launcher.archive.json') (@{
        SchemaVersion = 1; Package = 'launcher'; Build = $build; Platform = 'windows-x64'; Version = $Version
    } | ConvertTo-Json -Compress)
    $zip = Join-Path $root 'package.zip'
    [IO.Compression.ZipFile]::CreateFromDirectory($payloadRoot, $zip)
    $digest = [Security.Cryptography.SHA256]::Create()
    try { $archiveHash = $digest.ComputeHash([IO.File]::ReadAllBytes($zip)) } finally { $digest.Dispose() }
    $sig = Join-Path $root 'package.sig'
    [IO.File]::WriteAllBytes($sig, $Key.SignHash($archiveHash, [Security.Cryptography.HashAlgorithmName]::SHA256,
        [Security.Cryptography.RSASignaturePadding]::Pkcs1))
    $id = [Guid]::NewGuid()
    WriteText (Join-Path $payloadRoot '.launcher-updater-archive.json') (@{
        SchemaVersion = 1; TransactionId = $id.ToString('N'); Build = $build; Platform = 'windows-x64'
        Size = (Get-Item -LiteralPath $zip).Length; Sha256 = Hash $zip
    } | ConvertTo-Json -Compress)
    return [pscustomobject]@{ Directory = $package; Version = $Version; TransactionId = $id; Zip = $zip; Signature = $sig }
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archiveSourceSignerHash = '@LAUNCHER_ARCHIVE_SIGNER_SHA256@'
if ($archiveRecoverySource.EndsWith('ArchiveRecovery.ps1')) {
    $archiveSourceSignerHash = Hash (Join-Path $installerSource 'ArchiveSigner.cer')
}
function New-ReviewPublisher([string]$Name, [Security.Cryptography.RSA]$Key) {
    $request = [Security.Cryptography.X509Certificates.CertificateRequest]::new("CN=$Name", $Key,
        [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $now = [DateTimeOffset]::UtcNow
    $certificate = $request.CreateSelfSigned($now.AddMinutes(-1), $now.AddHours(1))
    try { return ,$certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert) }
    finally { $certificate.Dispose() }
}
$prereleaseKey = [Security.Cryptography.RSA]::Create(2048)
$releaseKey = [Security.Cryptography.RSA]::Create(3072)
try {
    $prereleaseCertificate = New-ReviewPublisher 'Launcher isolated prerelease' $prereleaseKey
    $releaseCertificate = New-ReviewPublisher 'Launcher isolated release' $releaseKey
    $signedPrevious = New-SignedReviewPackage '1.1.0.70' 'Prerelease' $prereleaseCertificate $prereleaseKey
    $signedRelease = New-SignedReviewPackage '1.2.1' 'Release' $releaseCertificate $releaseKey $prereleaseCertificate
} finally {
    $releaseKey.Dispose()
    $prereleaseKey.Dispose()
}

$releaseInstall = NewInstall 'signed-release-review'
$installSigned = @{ InstallDirectory = $releaseInstall; InstallScope = 'CurrentUser'; InstallUserSid = $scopeSid; AfterActivation = {} }
$older = Install-LauncherPayload $signedPrevious.Directory @installSigned -UpdaterTransactionId $signedPrevious.TransactionId
$accepted = Install-LauncherPayload $signedRelease.Directory @installSigned -UpdaterTransactionId $signedRelease.TransactionId
$releaseManifestPath = Join-Path $releaseInstall 'Launcher.install-manifest.json'
$releaseManifestHash = Hash $releaseManifestPath
$releaseProfile = Join-Path $releaseInstall 'Launcher.toml'
WriteText $releaseProfile 'personal configuration survives cleanup and revert'
$releaseProfileHash = Hash $releaseProfile
$releaseScope = [pscustomobject]@{ Scope = 'CurrentUser'; UserSid = $scopeSid }
$releaseContext = [pscustomobject]@{ UserSid = $scopeSid; IsElevated = $false }
$registration = Get-LauncherRegistrationPlan -Directory $releaseInstall -Scope CurrentUser -UserSid $scopeSid -Version $signedRelease.Version
$releaseRegistration = [pscustomobject]@{ Values = [ordered]@{}; Writes = 0 }
foreach ($name in $registration.Values.Keys) { $releaseRegistration.Values[$name] = [pscustomobject]@{ Value = $registration.Values[$name] } }
$releaseStore = {
    param($Operation, $Plan, $Snapshot)
    switch ($Operation) {
        'AssertWritable' { }
        'Read' { [pscustomobject]@{ Exists = $true; Values = $releaseRegistration.Values } }
        'Write' {
            $values = [ordered]@{}
            foreach ($name in $Plan.Values.Keys) { $values[$name] = [pscustomobject]@{ Value = $Plan.Values[$name] } }
            $releaseRegistration.Values = $values
            $releaseRegistration.Writes++
        }
        'Restore' { $releaseRegistration.Values = $Snapshot.Values }
        default { throw "Unexpected registration fixture operation: $Operation" }
    }
}.GetNewClosure()
. (Join-Path $releaseInstall 'Maintenance.ps1')
. (Join-Path $releaseInstall 'ArchiveRecovery.ps1')
. (Join-Path $releaseInstall 'ArchiveRestoreJournal.ps1')
$releaseArchives = @{ SourceArchive = $signedPrevious.Zip; SourceSignature = $signedPrevious.Signature
    CurrentArchive = $signedRelease.Zip; CurrentSignature = $signedRelease.Signature }
$releaseArguments = @{ Directory = $releaseInstall; RegistrationScope = $releaseScope; Context = $releaseContext
    TransactionId = $signedRelease.TransactionId.ToString('N'); Store = $releaseStore }
Check ($accepted.IsRelease -and (Hash $older.VersionedExecutable) -ceq $older.Sha256) 'Signed Release Setup did not preserve its previous executable before review.'
Check ((Get-LauncherRecoveryIconLocation $accepted) -like ($accepted.VersionedExecutable + ',*')) 'Release Setup used the retiring executable for the recovery shortcut icon.'
$badArchives = $releaseArchives.Clone()
$badArchives.SourceSignature = $signedRelease.Signature
ExpectFailure { Remove-LauncherAcceptedReleaseFallback @releaseArguments -Archives $badArchives } 'Cleanup accepted a mismatched archive signature.'
$duplicate = Join-Path $releaseInstall 'Launcher.20261110-120000.0123456789abcdef0123456789abcdef.backup.exe'
[IO.File]::Copy($older.VersionedExecutable, $duplicate)
ExpectFailure { Remove-LauncherAcceptedReleaseFallback @releaseArguments -Archives $releaseArchives } 'Cleanup guessed which of two identical fallbacks to remove.'
Check ((Hash $duplicate) -ceq $older.Sha256 -and (Hash $older.VersionedExecutable) -ceq $older.Sha256) 'Declined cleanup removed a copy of the fallback.'
[IO.File]::Delete($duplicate)
$modifiedProgram = Join-Path $releaseInstall 'Uninstall.cmd'
[IO.File]::AppendAllText($modifiedProgram, 'changed file prevents archived revert')
ExpectFailure { Remove-LauncherAcceptedReleaseFallback @releaseArguments -Archives $releaseArchives } 'Cleanup removed the fallback with a modified managed program file.'
[IO.File]::Copy((Join-Path $signedRelease.Directory 'Uninstall.cmd'), $modifiedProgram, $true)
$editedReleaseDoc = Join-Path $releaseInstall 'docs\Launcher.md'
[IO.File]::AppendAllText($editedReleaseDoc, 'personal annotations survive both operations')
$editedReleaseDocHash = Hash $editedReleaseDoc
$held = [IO.File]::Open($older.VersionedExecutable, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try { ExpectFailure { Remove-LauncherAcceptedReleaseFallback @releaseArguments -Archives $releaseArchives } 'Cleanup removed a locked fallback.' }
finally { $held.Dispose() }
Remove-LauncherAcceptedReleaseFallback @releaseArguments -Archives $releaseArchives
Remove-LauncherAcceptedReleaseFallback @releaseArguments -Archives $releaseArchives
Check (![IO.File]::Exists($older.VersionedExecutable) -and (Hash $accepted.ActiveExecutable) -ceq $accepted.Sha256 -and
    (Hash $releaseManifestPath) -ceq $releaseManifestHash -and (Hash $releaseProfile) -ceq $releaseProfileHash -and
    (Hash $editedReleaseDoc) -ceq $editedReleaseDocHash -and
    $releaseRegistration.Writes -eq 0) 'Keep cleanup or retry affected the current build, receipt, profile, or registration.'
$null = Install-LauncherPayload $signedRelease.Directory @installSigned -UpdaterTransactionId $signedRelease.TransactionId
Check (![IO.File]::Exists($older.VersionedExecutable)) 'Same-byte Release repair recreated a retired executable.'
Invoke-LauncherArchiveRollback @releaseArguments -Archives $releaseArchives -ProcessReader { @() }
$restoredRelease = [IO.File]::ReadAllText($releaseManifestPath) | ConvertFrom-Json
Check ((Hash $accepted.ActiveExecutable) -ceq $older.Sha256 -and
    (Hash (Join-Path $releaseInstall 'LauncherSvgExtractor.exe')) -ceq (Hash (Join-Path $signedPrevious.Directory 'LauncherSvgExtractor.exe')) -and
    $restoredRelease.LastRollback.TransactionId -ceq $releaseArguments.TransactionId -and
    $releaseRegistration.Values.DisplayVersion.Value -ceq $signedPrevious.Version -and $releaseRegistration.Writes -eq 1 -and
    (Hash $releaseProfile) -ceq $releaseProfileHash -and [IO.File]::Exists($accepted.VersionedExecutable)) 'Archived Revert after Keep cleanup failed to restore the exact prior package and scope.'
NoStaging $releaseInstall

[IO.File]::Delete($startupMarkerPath)
$manifestHash = Hash $scopedManifestPath
ExpectFailure {
    Install-LauncherPayload $packageA $scopedInstall -InstallScope CurrentUser -InstallUserSid $scopeSid -AfterActivation { throw 'Owned registry publication failure.' } -UpdaterTransactionId ([Guid]::NewGuid())
} 'A failed registration publication did not fail activation.'
Check ((Hash (Join-Path $scopedInstall 'Launcher.exe')) -eq $scopedActiveHash) 'Failed registration publication did not restore the original active file.'
Check ((Hash $scopedManifestPath) -ceq $manifestHash) 'Failed registration publication did not restore the original update receipt.'
$null = Install-LauncherPayload $packageA $scopedInstall
$scopedManifest = [IO.File]::ReadAllText($scopedManifestPath) | ConvertFrom-Json
Check ($scopedManifest.InstallScope -ceq 'CurrentUser' -and $scopedManifest.InstallUserSid -ceq $scopeSid) 'A file-only development update dropped the original registration hive/account context.'
Check ($null -eq $scopedManifest.PSObject.Properties['LastUpdate']) 'A file-only replacement left a stale update receipt.'
$null = Install-LauncherPayload $packageB $scopedInstall -InstallScope CurrentUser -InstallUserSid $scopeSid -AfterActivation { }
$scopedManifest = [IO.File]::ReadAllText($scopedManifestPath) | ConvertFrom-Json
Check ($scopedManifest.LastUpdate.TransactionId -cmatch '\A[0-9a-f]{32}\z' -and
    $scopedManifest.LastUpdate.TransactionId -cnotin @($markerText, ('0' * 32))) 'Standalone Setup did not generate an independent update receipt ID.'

# Registered Release Setup retains one verified copy of the previous build.
$registeredSetup = @{ InstallScope = 'CurrentUser'; InstallUserSid = $scopeSid; AfterActivation = {} }
$reviewInstall = NewInstall 'release-review'
$reviewOlder = Install-LauncherPayload $packageA $reviewInstall @registeredSetup
$reviewOld = Install-LauncherPayload $packageB $reviewInstall @registeredSetup
$reviewOldHash = Hash $reviewOld.VersionedExecutable
$duplicate = Join-Path $reviewInstall 'Launcher.20261110-120000.0123456789abcdef0123456789abcdef.backup.exe'
[IO.File]::Copy($reviewOld.VersionedExecutable, $duplicate)
$reviewFinal = Install-LauncherPayload $releasePackage $reviewInstall @registeredSetup
$reviewManifest = Join-Path $reviewInstall 'Launcher.install-manifest.json'
$reviewReceipt = ([IO.File]::ReadAllText($reviewManifest) | ConvertFrom-Json).LastUpdate
Check ($reviewFinal.PreviousExecutable -eq $reviewOld.VersionedExecutable -and
    (Hash $reviewOld.VersionedExecutable) -eq $reviewOldHash -and $reviewReceipt.Previous.Sha256 -eq $reviewOldHash) 'Release cleanup removed the exact previous build from its update receipt.'
Check (!(Test-Path -LiteralPath $reviewOlder.VersionedExecutable) -and !(Test-Path -LiteralPath $duplicate) -and
    !$reviewFinal.CleanupWarnings.Count) 'Protecting the review fallback changed cleanup of other older executables.'
$reviewRepair = Install-LauncherPayload $releasePackage $reviewInstall @registeredSetup
Check ((Hash $reviewOld.VersionedExecutable) -eq $reviewOldHash -and !$reviewRepair.CleanupWarnings.Count -and
    ([IO.File]::ReadAllText($reviewManifest) | ConvertFrom-Json).LastUpdate.TransactionId -eq $reviewReceipt.TransactionId) 'A matching Release repair discarded the verified review fallback or receipt.'

# A same-version decoy cannot replace a missing fallback, even when its PE
# metadata is otherwise valid for cleanup. The mutable WIP name is no archive.
$mutable = Join-Path $reviewInstall 'Launcher-WIP.exe'
[IO.File]::Copy($reviewOld.VersionedExecutable, $mutable)
[IO.File]::AppendAllText($reviewOld.VersionedExecutable, 'owned changed fallback fixture')
$reviewRepair = Install-LauncherPayload $releasePackage $reviewInstall @registeredSetup
Check (!(Test-Path -LiteralPath $reviewOld.VersionedExecutable) -and !(Test-Path -LiteralPath $mutable) -and
    !$reviewRepair.CleanupWarnings.Count) 'Release repair protected a file that could not serve as the receipt fallback.'

$reviewLegacy = NewInstall 'release-review-legacy'
[void][IO.Directory]::CreateDirectory($reviewLegacy)
[IO.File]::Copy((Join-Path $packageA 'Launcher.exe'), (Join-Path $reviewLegacy 'Launcher.exe'))
$machineSetup = @{ InstallScope = 'AllUsers'; AfterActivation = {} }
$legacyReviewResult = Install-LauncherPayload $releasePackage $reviewLegacy @machineSetup
$legacyReviewBackup = $legacyReviewResult.PreviousExecutable
$legacyReviewManifest = Join-Path $reviewLegacy 'Launcher.install-manifest.json'
$legacyReviewReceipt = ([IO.File]::ReadAllText($legacyReviewManifest) | ConvertFrom-Json).LastUpdate
Check ($legacyReviewBackup -like '*.backup.exe' -and (Hash $legacyReviewBackup) -eq $legacyReviewReceipt.Previous.Sha256) 'Release did not retain the verified backup of the legacy active executable.'
$null = Install-LauncherPayload $releasePackage $reviewLegacy @machineSetup
Check ((Hash $legacyReviewBackup) -eq $legacyReviewReceipt.Previous.Sha256) 'Release repair deleted the exact legacy backup.'

# A malformed receipt or a file-only repair provides no cleanup exemption.
$malformedReview = [IO.File]::ReadAllText($legacyReviewManifest) | ConvertFrom-Json
$malformedReview.LastUpdate.TransactionId = 'invalid-transaction'
WriteText $legacyReviewManifest ($malformedReview | ConvertTo-Json -Depth 8)
$null = Install-LauncherPayload $releasePackage $reviewLegacy @machineSetup
Check (!(Test-Path -LiteralPath $legacyReviewBackup)) 'A malformed receipt protected a superseded executable.'
$fileOnlyReview = NewInstall 'release-review-file-only'
$fileOnlyOld = Install-LauncherPayload $packageA $fileOnlyReview @registeredSetup
$null = Install-LauncherPayload $releasePackage $fileOnlyReview @registeredSetup
$null = Install-LauncherPayload $releasePackage $fileOnlyReview
Check (!(Test-Path -LiteralPath $fileOnlyOld.VersionedExecutable)) 'The file-only Release workflow gained a registered review exemption.'

# Exercise a release followed by an evaluation build and a public Beta. Both
# numbered builds and the previous final remain until the next final activates.
$beta4 = NewPackage 'sequence-beta4'
$beta5 = NewPackage 'sequence-beta5'
$final14 = NewPackage 'sequence-final14'
[LauncherSetup.DeploymentVersionFixture]::Configure((Join-Path $beta4 'Launcher.exe'), '1.3.0.4', 'Prerelease')
[LauncherSetup.DeploymentVersionFixture]::Configure((Join-Path $beta5 'Launcher.exe'), '1.3.0.5', 'Beta')
[LauncherSetup.DeploymentVersionFixture]::Configure((Join-Path $final14 'Launcher.exe'), '1.4.0.0', 'Release')
$sequenceInstall = NewInstall 'release-sequence'
$sequence12 = Install-LauncherPayload $releasePackage $sequenceInstall
$sequence4 = Install-LauncherPayload $beta4 $sequenceInstall
$sequence5 = Install-LauncherPayload $beta5 $sequenceInstall
Check (!$sequence5.IsRelease -and $sequence5.DistributionKind -ceq 'Beta') 'The public Beta was not recognized as a non-final installation.'
foreach ($kept in @($sequence12.VersionedExecutable, $sequence4.VersionedExecutable, $sequence5.VersionedExecutable)) {
    Check (Test-Path -LiteralPath $kept) 'A beta package prematurely removed a retained executable.'
}
$sequenceHashes = @{}
foreach ($kept in @($sequence12.VersionedExecutable, $sequence4.VersionedExecutable, $sequence5.VersionedExecutable)) { $sequenceHashes[$kept] = Hash $kept }
ExpectFailure {
    Install-LauncherPayload $final14 $sequenceInstall -Progress {
        param($Percent, $Message)
        if ($Percent -eq 100) { throw 'owned failed final activation' }
    }
} 'A failed final activation ignored its reporting failure.'
foreach ($kept in $sequenceHashes.Keys) {
    Check ((Hash $kept) -eq $sequenceHashes[$kept]) 'Failed final activation removed or changed an older executable.'
}
Check ((Hash (Join-Path $sequenceInstall 'Launcher.exe')) -eq $sequenceHashes[$sequence5.VersionedExecutable]) 'Failed final activation did not restore the prior active beta.'
$sequenceFinal = Install-LauncherPayload $final14 $sequenceInstall
foreach ($removed in $sequenceHashes.Keys) { Check (!(Test-Path -LiteralPath $removed)) 'The next final retained an executable from the previous release/beta sequence.' }
Check (@(Get-ChildItem -LiteralPath $sequenceInstall -Filter 'Launcher-R*.exe').Count -eq 1 -and !$sequenceFinal.CleanupWarnings.Count) 'The release sequence did not finish with only the new final version.'

# A later equal/newer executable is preserved when installing an older final,
# including a legacy unversioned active file that needs a fresh backup.
$newerPackage = NewPackage 'newer-for-downgrade'
[LauncherSetup.DeploymentVersionFixture]::Configure((Join-Path $newerPackage 'Launcher.exe'), '2.0.0.0', 'Release')
$downgradeInstall = NewInstall 'downgrade'
$newerResult = Install-LauncherPayload $newerPackage $downgradeInstall
$downgraded = Install-LauncherPayload $final14 $downgradeInstall
Check ((Hash $newerResult.VersionedExecutable) -eq (Hash (Join-Path $newerPackage 'Launcher.exe'))) 'Installing an older final removed a newer retained executable.'
Check ($downgraded.PreviousExecutable -eq $newerResult.VersionedExecutable) 'A retained newer previous version disappeared from the deployment receipt.'
$legacyDowngrade = NewInstall 'legacy-downgrade'
[void][IO.Directory]::CreateDirectory($legacyDowngrade)
[IO.File]::Copy((Join-Path $newerPackage 'Launcher.exe'), (Join-Path $legacyDowngrade 'Launcher.exe'), $false)
$legacyDowngraded = Install-LauncherPayload $final14 $legacyDowngrade
Check ($legacyDowngraded.PreviousExecutable -like '*.backup.exe' -and
    (Hash $legacyDowngraded.PreviousExecutable) -eq (Hash (Join-Path $newerPackage 'Launcher.exe'))) 'An older final deleted the newly preserved legacy backup of a newer executable.'

# Cleanup does not trust a candidate that changes after the pre-activation
# snapshot, and does not sweep a new matching filename that appears later.
$changedInstall = NewInstall 'changed-cleanup'
$changedOld = Install-LauncherPayload $packageA $changedInstall
$changedOldPath = $changedOld.VersionedExecutable
$appearedPath = Join-Path $changedInstall 'Launcher.20261023-120000.0123456789abcdef0123456789abcdef.backup.exe'
$changedResult = Install-LauncherPayload $final14 $changedInstall -Progress {
    param($Percent, $Message)
    if ($Percent -eq 100) {
        [IO.File]::AppendAllText($changedOldPath, 'owned concurrent archived-file change')
        [IO.File]::Copy((Join-Path $packageA 'Launcher.exe'), $appearedPath, $false)
    }
}
Check ($changedResult.CleanupWarnings.Count -eq 1 -and (Test-Path -LiteralPath $changedOldPath)) 'Changed cleanup bytes were deleted or were not reported.'
Check (Test-Path -LiteralPath $appearedPath) 'Cleanup deleted an executable that appeared after its reviewed snapshot.'
Check ((Hash $changedResult.ActiveExecutable) -eq (Hash (Join-Path $final14 'Launcher.exe'))) 'A changed cleanup candidate invalidated the successfully activated final.'

# Isolated discovery fixtures cover the exact deletion boundary without
# executing synthetic files. Metadata is injected only for this read-only scan.
$candidateDirectory = NewInstall 'candidate-discovery'
[void][IO.Directory]::CreateDirectory($candidateDirectory)
$script:cleanupMetadata = @{}
function Add-CleanupCandidate([string]$Name, [string]$Version, [string]$State = 'Verified') {
    $path = Join-Path $candidateDirectory $Name
    [IO.File]::WriteAllText($path, $Name + ' owned metadata fixture')
    $numeric = [version]$Version
    $owner = 'D' + [char]0x0142 + 'ugosz'
    $value = [pscustomobject]@{
        ProductName = $owner + ' Launcher'; LegalCopyright = 'Copyright (c) 2026 John ' + $owner
        ProductVersion = $Version; InternalName = 'Launcher'; OriginalFilename = 'Launcher.exe'
        ProductMajorPart = $numeric.Major; ProductMinorPart = $numeric.Minor
        ProductBuildPart = [Math]::Max(0, $numeric.Build); ProductPrivatePart = [Math]::Max(0, $numeric.Revision)
        FileVersion = $Version; FileMajorPart = $numeric.Major; FileMinorPart = $numeric.Minor
        FileBuildPart = [Math]::Max(0, $numeric.Build); FilePrivatePart = [Math]::Max(0, $numeric.Revision)
        IsDebug = $false; IsSpecialBuild = $false; IsPrivateBuild = $false; SpecialBuild = ''; PrivateBuild = ''
    }
    switch ($State) {
        'Debug' { $value.IsDebug = $true }
        'Instrumented' { $value.IsSpecialBuild = $true; $value.SpecialBuild = 'owned instrumentation' }
        'Foreign' { $value.ProductName = 'Some other product' }
        'Unparseable' { $value.FileVersion = 'not a numeric version' }
        'FileMismatch' { $value.FilePrivatePart++ }
        'SplitVersion' { $value.ProductVersion = '{0}.{1}' -f $numeric.Major, $numeric.Minor; $value.ProductBuildPart = 0; $value.ProductPrivatePart = 0 }
    }
    $script:cleanupMetadata[$path] = $value
    return $path
}
$expectedCandidates = @(
    Add-CleanupCandidate 'Launcher-R1.2.exe' '1.2'
    Add-CleanupCandidate 'Launcher-R1.3.0.4.exe' '1.3.0.4' 'SplitVersion'
    Add-CleanupCandidate 'Launcher-R1.3.0.5.20261023-120000.0123456789abcdef0123456789abcdef.backup.exe' '1.3.0.5'
    Add-CleanupCandidate 'Launcher.20261023-120001.1123456789abcdef0123456789abcdef.backup.exe' '1.3.0.6'
    Add-CleanupCandidate 'Launcher-WIP.legacy.2123456789abcdef0123456789abcdef.backup.exe' '1.3'
    Add-CleanupCandidate 'Launcher-WIP.exe' '1.3.0.2'
)
foreach ($excluded in @(
    @('Launcher-R1.4.exe', '1.4', 'Verified'), @('Launcher-R1.4.0.0.exe', '1.4.0.0', 'Verified'),
    @('Launcher-R1.4.0.1.exe', '1.4.0.1', 'Verified'), @('Launcher-R2.0.exe', '2.0', 'Verified'),
    @('Launcher-R0.1.exe', '1.3', 'Verified'), @('Launcher-R1.0.exe', '1.0', 'Foreign'),
    @('Launcher-R1.1.exe', '1.1', 'Debug'), @('Launcher-R1.1.0.2.exe', '1.1.0.2', 'Instrumented'),
    @('Launcher-R1.1.0.3.exe', '1.1.0.3', 'Unparseable'), @('Launcher-R1.1.0.4.exe', '1.1.0.4', 'FileMismatch'),
    @('Launcher.exe', '1.2', 'Verified'), @('Launcher.notes.backup.exe', '1.2', 'Verified'),
    @('Launcher-R1.2.20261023-120002.3123456789abcdef0123456789abcdef.backup.exe', '1.3', 'Verified')
)) { [void](Add-CleanupCandidate $excluded[0] $excluded[1] $excluded[2]) }
$reader = { param($Path) $script:cleanupMetadata[$Path] }
$foundCandidates = @(Get-LauncherSupersededExecutables -Directory $candidateDirectory -IncomingVersion '1.4' -MetadataReader $reader)
Check ($foundCandidates.Count -eq $expectedCandidates.Count) 'Superseded discovery admitted a same/newer, foreign, debug, instrumented, mismatched or unrecognized executable.'
foreach ($expected in $expectedCandidates) {
    Check (@($foundCandidates | Where-Object { $_.Path -eq $expected -and $_.Sha256 -eq (Hash $expected) -and $_.Version -lt [version]'1.4.0.0' }).Count -eq 1) 'Superseded discovery omitted a verified older executable or normalized its version incorrectly.'
}
$linkCandidate = Join-Path $candidateDirectory 'Launcher-R1.3.0.7.exe'
if ([LauncherSetup.DeploymentFixture]::CreateSymbolicLinkW($linkCandidate, 'Launcher-R1.3.0.4.exe', 2) -or
    [LauncherSetup.DeploymentFixture]::CreateSymbolicLinkW($linkCandidate, 'Launcher-R1.3.0.4.exe', 0)) {
    $script:cleanupMetadata[$linkCandidate] = $script:cleanupMetadata[$expectedCandidates[1]]
    $linkedCandidates = @(Get-LauncherSupersededExecutables -Directory $candidateDirectory -IncomingVersion '1.4' -MetadataReader $reader)
    Check ($linkedCandidates.Count -eq $expectedCandidates.Count) 'A reparse-point executable became eligible for cleanup.'
}

$retired = Join-Path $run 'obsolete stock demos'
$retiredWrapper = Join-Path $retired 'examples\demo_many_colors.cmd'
WriteText $retiredWrapper ([IO.File]::ReadAllText((Join-Path $PackageDirectory 'examples\demo_many_colors.cmd')))
$retiredEdited = Join-Path $retired 'examples\demo_many_colors.toml'
WriteText $retiredEdited '# Keep my edited demo.'
$retiredHash = Hash $retiredEdited
$retiredWarnings = @(Remove-LauncherObsoleteStockDemos -InstallDirectory $retired)
Check (![IO.File]::Exists($retiredWrapper)) 'A known stock CMD remained after obsolete-demo cleanup.'
Check ((Hash $retiredEdited) -ceq $retiredHash -and $retiredWarnings.Count -eq 1) 'Obsolete-demo cleanup deleted an edited demo or failed to report its preservation.'

# Use identical supporting files and distinct genuine PE identities. The
# registration store stays in memory and the process reader never closes apps.
. (Join-Path (Split-Path $DeploymentScript -Parent) 'Maintenance.ps1')
$quickOld = NewPackage 'quick-before'
$quickNew = Join-Path $run 'quick-after'
Copy-Item -LiteralPath $quickOld -Destination $quickNew -Recurse
[LauncherSetup.DeploymentVersionFixture]::Configure((Join-Path $quickOld 'Launcher.exe'), '1.1.0.101', 'Prerelease')
[LauncherSetup.DeploymentVersionFixture]::Configure((Join-Path $quickNew 'Launcher.exe'), '1.1.0.102', 'Prerelease')
$quickInstall = NewInstall 'quick-rollback'
$quickSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$quickScope = [pscustomobject]@{ Scope = 'CurrentUser'; UserSid = $quickSid }
$quickContext = [pscustomobject]@{ UserSid = $quickSid; IsElevated = $false }
$quickParams = @{ InstallDirectory = $quickInstall; InstallScope = 'CurrentUser'; InstallUserSid = $quickSid; AfterActivation = {} }
$quickPrevious = Install-LauncherPayload -PackageDirectory $quickOld @quickParams
$quickId = [Guid]::NewGuid()
$quickCurrent = Install-LauncherPayload -PackageDirectory $quickNew @quickParams -UpdaterTransactionId $quickId -QuickRollbackEligible
$quickManifestPath = Join-Path $quickInstall 'Launcher.install-manifest.json'
$quickManifest = [IO.File]::ReadAllText($quickManifestPath) | ConvertFrom-Json
Check ($quickManifest.QuickRollback.TransactionId -ceq $quickId.ToString('N')) 'Setup did not authorize a verified executable-only upgrade.'
$quickCurrent = Install-LauncherPayload -PackageDirectory $quickNew @quickParams -QuickRollbackEligible
$quickManifest = [IO.File]::ReadAllText($quickManifestPath) | ConvertFrom-Json
Check ($quickManifest.QuickRollback.TransactionId -ceq $quickId.ToString('N')) 'An unchanged repair lost an existing quick rollback.'
$quickManifestHash = Hash $quickManifestPath
$quickProfile = Join-Path $quickInstall 'Launcher.toml'
WriteText $quickProfile '# Preserve my user-owned settings.'
$quickProfileHash = Hash $quickProfile
$quickForeign = Join-Path $quickInstall 'docs\user-notes.txt'
WriteText $quickForeign 'Keep this unrelated file.'
$quickForeignHash = Hash $quickForeign
$quickRegistryPlan = Get-LauncherRegistrationPlan -Directory $quickInstall -Scope CurrentUser -UserSid $quickSid -Version '1.1.0.102'
$quickValues = [ordered]@{}
foreach ($name in $quickRegistryPlan.Values.Keys) { $quickValues[$name] = [pscustomobject]@{ Value = $quickRegistryPlan.Values[$name] } }
$quickState = [pscustomobject]@{ Values = $quickValues; FailWrite = $false }
$quickStore = {
    param($Operation, $Plan, $Snapshot)
    switch ($Operation) {
        'AssertWritable' { }
        'Read' { [pscustomobject]@{ Exists = $true; Values = $quickState.Values } }
        'Write' {
            $new = [ordered]@{}
            foreach ($name in $Plan.Values.Keys) { $new[$name] = [pscustomobject]@{ Value = $Plan.Values[$name] } }
            $quickState.Values = $new
            if ($quickState.FailWrite) { throw 'Fixture registration publication failed.' }
        }
        'Restore' { $quickState.Values = $Snapshot.Values }
    }
}.GetNewClosure()
$rollbackParams = @{ Directory = $quickInstall; RegistrationScope = $quickScope; TransactionId = $quickId.ToString('N')
    Context = $quickContext; Store = $quickStore; ProcessReader = { @() } }
$quickReadme = Join-Path $quickInstall 'README.txt'
[IO.File]::AppendAllText($quickReadme, 'Changed supporting file after update.')
ExpectFailure { Invoke-LauncherQuickRollback @rollbackParams } 'Quick rollback accepted a changed companion.'
Check ((Hash $quickCurrent.ActiveExecutable) -ceq $quickCurrent.Sha256) 'Rejected rollback changed the active executable.'
[IO.File]::Copy((Join-Path $quickNew 'README.txt'), $quickReadme, $true)
$quickState.FailWrite = $true
ExpectFailure { Invoke-LauncherQuickRollback @rollbackParams } 'Quick rollback ignored a partial registration failure.'
Check ((Hash $quickCurrent.ActiveExecutable) -ceq $quickCurrent.Sha256 -and (Hash $quickManifestPath) -ceq $quickManifestHash -and
    $quickState.Values.DisplayVersion.Value -ceq '1.1.0.102') 'Quick rollback did not recover the alias, receipt and registration after failure.'
NoStaging $quickInstall
$quickState.FailWrite = $false
Invoke-LauncherQuickRollback @rollbackParams
$quickManifest = [IO.File]::ReadAllText($quickManifestPath) | ConvertFrom-Json
Check ((Hash $quickCurrent.ActiveExecutable) -ceq $quickPrevious.Sha256 -and
    (Hash $quickCurrent.VersionedExecutable) -ceq $quickCurrent.Sha256 -and
    $quickState.Values.DisplayVersion.Value -ceq '1.1.0.101') 'Quick rollback did not restore the verified previous build and registration.'
Check ($quickManifest.ActiveVersionedExecutable -ceq 'Launcher-R1.1.0.101.exe' -and
    $quickManifest.LastRollback.TransactionId -ceq $quickId.ToString('N') -and
    $quickManifest.LastRollback.Restored.Sha256 -ceq $quickPrevious.Sha256 -and
    $null -eq $quickManifest.PSObject.Properties['LastUpdate'] -and
    $null -eq $quickManifest.PSObject.Properties['QuickRollback']) 'Quick rollback left an unresolved receipt or invalid proof.'
Check ((Hash $quickProfile) -ceq $quickProfileHash -and (Hash $quickForeign) -ceq $quickForeignHash) 'Quick rollback changed user-owned files.'
ExpectFailure { Invoke-LauncherQuickRollback @rollbackParams } 'A completed transaction could be rolled back a second time.'
NoStaging $quickInstall
$null = Install-LauncherPayload -PackageDirectory $quickOld @quickParams -QuickRollbackEligible
$quickRepaired = [IO.File]::ReadAllText($quickManifestPath) | ConvertFrom-Json
Check ($quickRepaired.LastRollback.TransactionId -ceq $quickId.ToString('N') -and
    $quickRepaired.LastRollback.Restored.Sha256 -ceq $quickPrevious.Sha256 -and
    $null -eq $quickRepaired.PSObject.Properties['LastUpdate']) 'An unchanged repair replaced the successful rollback proof.'

$quickChangedInstall = NewInstall 'quick-changed-companion'
$quickParams.InstallDirectory = $quickChangedInstall
$null = Install-LauncherPayload -PackageDirectory $quickOld @quickParams
[IO.File]::AppendAllText((Join-Path $quickNew 'README.txt'), 'A legitimate package companion change.')
$null = Install-LauncherPayload -PackageDirectory $quickNew @quickParams -QuickRollbackEligible
$quickChangedManifest = [IO.File]::ReadAllText((Join-Path $quickChangedInstall 'Launcher.install-manifest.json')) | ConvertFrom-Json
Check ($quickChangedManifest.LastUpdate.Current.Version -ceq '1.1.0.102' -and
    $null -eq $quickChangedManifest.PSObject.Properties['QuickRollback']) 'Setup authorized quick rollback across a companion update.'
$quickChangedManifest.LastUpdate.PSObject.Properties.Remove('Previous')
WriteText (Join-Path $quickChangedInstall 'Launcher.install-manifest.json') ($quickChangedManifest | ConvertTo-Json -Depth 8)
$null = Install-LauncherPayload -PackageDirectory $quickNew @quickParams -QuickRollbackEligible
$quickMalformedManifest = [IO.File]::ReadAllText((Join-Path $quickChangedInstall 'Launcher.install-manifest.json')) | ConvertFrom-Json
Check ($null -eq $quickMalformedManifest.PSObject.Properties['QuickRollback']) 'An invalid legacy receipt acquired quick rollback authorization during repair.'
Write-Output "Installer deployment checks passed: $script:checks; fixtures: $run"
