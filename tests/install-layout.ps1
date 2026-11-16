[CmdletBinding()]
param([Parameter(Mandatory)][string] $SourceDirectory, [Parameter(Mandatory)][string] $OutputDirectory)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$repository = Split-Path -Parent $PSScriptRoot
. (Join-Path $repository 'tools\installed-launcher.ps1')
$run = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ([Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($run)
$script:checks = 0
$paletteNames = @('examples\basic_colors.toml', 'examples\extra_colors.toml', 'examples\colors-light.toml', 'examples\colors-dark.toml')
$exampleNames = $paletteNames
function Check([bool] $Condition, [string] $Message) {
    if (!$Condition) { throw $Message }
    $script:checks++
}
function Hash([string] $Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function WriteText([string] $Path, [string] $Text) { [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false)) }
# These PE fixtures are never started. Keep process discovery deterministic and
# avoid touching a user's actual Launcher or requiring access to system CIM.
$global:LauncherFixtureBeforeProcessDiscovery = $null
$global:LauncherFixtureProcesses = @()
function Get-CimInstance {
    [CmdletBinding()] param([string]$ClassName, [string]$Filter)
    if ($global:LauncherFixtureBeforeProcessDiscovery) {
        $hook = $global:LauncherFixtureBeforeProcessDiscovery
        $global:LauncherFixtureBeforeProcessDiscovery = $null
        & $hook | Out-Null
    }
    @($global:LauncherFixtureProcesses)
}
function NewCase([string] $Name) {
    $root = Join-Path $run $Name
    $install = Join-Path $root 'Program Files'
    $data = Join-Path $root ('Local AppData\D' + [char]0x0142 + 'ugosz\Launcher')
    [void][IO.Directory]::CreateDirectory($install)
    [void][IO.Directory]::CreateDirectory($data)
    return [pscustomobject]@{ Install = $install; Data = $data }
}
function NewRelease([string] $Version, [string] $Name, [string] $Distribution = '') {
    $directory = Join-Path $run $Name
    [void][IO.Directory]::CreateDirectory($directory)
    $numeric = [version]$Version
    $numericVersion = '{0}.{1}.{2}.{3}' -f $numeric.Major, $numeric.Minor, [Math]::Max(0, $numeric.Build), [Math]::Max(0, $numeric.Revision)
    $code = @"
using System.Reflection;
[assembly: AssemblyVersion("$numericVersion")]
[assembly: AssemblyFileVersion("$numericVersion")]
[assembly: AssemblyInformationalVersion("$Version")]
[assembly: AssemblyDescription("$Distribution")]
[assembly: AssemblyProduct("D\u0142ugosz Launcher")]
[assembly: AssemblyCopyright("Copyright (c) 2026 John D\u0142ugosz")]
public static class $Name { public static int Main() { return 0; } }
"@
    Add-Type -TypeDefinition $code -Language CSharp -OutputAssembly (Join-Path $directory 'Launcher.exe') -OutputType ConsoleApplication
    Initialize-LauncherInstallFixture
    [LauncherInstallVersionFixture]::Mark((Join-Path $directory 'Launcher.exe'), 0)
    WriteText (Join-Path $directory 'Launcher.default.toml') "release $Version default"
    WriteText (Join-Path $directory 'Launcher.installed.default.toml') "release $Version installed default"
    WriteText (Join-Path $directory 'LICENSE.txt') "release $Version license"
    WriteText (Join-Path $directory 'THIRD-PARTY-NOTICES.txt') "release $Version notices"
    WriteText (Join-Path $directory 'README.txt') "release $Version readme"
    [void][IO.Directory]::CreateDirectory((Join-Path $directory 'examples'))
    foreach ($exampleName in $exampleNames) {
        WriteText (Join-Path $directory $exampleName) "release $Version $exampleName"
    }
    return $directory
}
function InstallRelease($Case, [string] $Source) {
    & (Join-Path $repository 'tools\install-release.ps1') -InstallDirectory $Case.Install -SourceDirectory $Source -NoStart
}
function AcceptBuild($Case, [string] $Source = $SourceDirectory) {
    & (Join-Path $repository 'tools\accept-build.ps1') -InstallDirectory $Case.Install -SourceDirectory $Source -UserDataDirectory $Case.Data -NoStart
}

function Initialize-LauncherInstallFixture {
    if (!('LauncherInstallVersionFixture' -as [type])) {
        Add-Type @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class LauncherInstallVersionFixture {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32.dll", SetLastError=true)] static extern uint SizeofResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr LockResource(IntPtr resource);
    [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr module);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr BeginUpdateResourceW(string path, bool deleteExisting);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool UpdateResourceW(IntPtr update, IntPtr type, IntPtr name, ushort language, byte[] bytes, uint size);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool EndUpdateResourceW(IntPtr update, bool discard);
    public static void Mark(string path, uint flags) {
        byte[] bytes;
        var module = LoadLibraryExW(path, IntPtr.Zero, 0x22);
        if (module == IntPtr.Zero) throw new Win32Exception();
        try {
            var resource = FindResourceW(module, (IntPtr)1, (IntPtr)16);
            if (resource == IntPtr.Zero) throw new Win32Exception();
            var size = SizeofResource(module, resource);
            var value = LockResource(LoadResource(module, resource));
            if (size == 0 || value == IntPtr.Zero) throw new Win32Exception();
            bytes = new byte[checked((int)size)];
            Marshal.Copy(value, bytes, 0, bytes.Length);
        } finally { FreeLibrary(module); }
        // Match the native application's InternalName rather than C#'s default
        // output basename, while retaining its otherwise real PE metadata.
        var key = System.Text.Encoding.Unicode.GetBytes("InternalName\0");
        for (int index = 0; index + key.Length < bytes.Length; ++index) {
            bool match = true;
            for (int j = 0; j < key.Length; ++j) if (bytes[index + j] != key[j]) { match = false; break; }
            if (!match) continue;
            int value = (index + key.Length + 3) & ~3;
            var replacement = System.Text.Encoding.Unicode.GetBytes("Launcher\0");
            Array.Copy(replacement, 0, bytes, value, replacement.Length);
            Array.Copy(BitConverter.GetBytes((ushort)9), 0, bytes, index - 4, 2);
            break;
        }
        int offset = 6;
        while (offset + 1 < bytes.Length && (bytes[offset] != 0 || bytes[offset + 1] != 0)) offset += 2;
        offset = (offset + 5) & ~3;
        if (offset + 52 > bytes.Length || BitConverter.ToUInt32(bytes, offset) != 0xfeef04bd) {
            throw new InvalidOperationException("Owned version fixture has no fixed version information.");
        }
        Array.Copy(BitConverter.GetBytes((uint)0x3f), 0, bytes, offset + 24, 4);
        Array.Copy(BitConverter.GetBytes(flags), 0, bytes, offset + 28, 4);
        var update = BeginUpdateResourceW(path, true);
        if (update == IntPtr.Zero) throw new Win32Exception();
        bool written = UpdateResourceW(update, (IntPtr)16, (IntPtr)1, 0x0409, bytes, (uint)bytes.Length);
        int error = Marshal.GetLastWin32Error();
        bool ended = EndUpdateResourceW(update, !written);
        if (!written) throw new Win32Exception(error);
        if (!ended) throw new Win32Exception();
    }
}
'@
    }
}

function NewSpecialRelease([string] $Name, [uint32] $Flags) {
    $directory = NewRelease '1.4' $Name
    $executable = Join-Path $directory 'Launcher.exe'
    [LauncherInstallVersionFixture]::Mark($executable, $Flags)
    # Simulate renaming a separate scenario executable to the normal basename.
    $scenario = Join-Path $directory 'LauncherVersionTest.exe'
    Move-Item -LiteralPath $executable -Destination $scenario -ErrorAction Stop
    [IO.File]::Copy($scenario, $executable)
    return $directory
}

$release10 = NewRelease '1.0' 'Release10'
$release12 = NewRelease '1.2' 'Release12'
$releaseHash = Hash (Join-Path $release10 'Launcher.exe')
$buildHash = Hash (Join-Path $SourceDirectory 'Launcher.exe')
Assert-LauncherNormalBuild (Join-Path $SourceDirectory 'Launcher.exe')
foreach ($marker in @(@{ Name = 'SpecialBuild'; Flag = 0x20 }, @{ Name = 'PrivateBuild'; Flag = 0x08 })) {
    $specialSource = NewSpecialRelease $marker.Name $marker.Flag
    $specialVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $specialSource 'Launcher.exe'))
    Check ([bool]$specialVersion.('Is' + $marker.Name)) "Owned $($marker.Name) fixture lacks its expected native flag."
    $specialCase = NewCase ("reject-" + $marker.Name)
    $specialActive = Join-Path $specialCase.Install 'Launcher.exe'
    [IO.File]::Copy((Join-Path $release10 'Launcher.exe'), $specialActive)
    $specialConfig = Join-Path $specialCase.Data 'Launcher.toml'
    WriteText $specialConfig 'personal settings remain untouched'
    $specialConfigHash = Hash $specialConfig
    foreach ($operation in @('release', 'acceptance')) {
        $rejected = $false
        try {
            if ($operation -eq 'release') { $null = InstallRelease $specialCase $specialSource }
            else { $null = AcceptBuild $specialCase $specialSource }
        } catch { $rejected = $_.Exception.Message -like 'Instrumented or special Launcher builds*' }
        Check $rejected "$operation did not reject the renamed $($marker.Name) executable by its version resource."
        Check ((Hash $specialActive) -eq $releaseHash) "$operation changed the active executable before rejecting $($marker.Name)."
        Check (@(Get-ChildItem -LiteralPath $specialCase.Install -Force).Count -eq 1) "$operation created release, staging or backup entries before rejecting $($marker.Name)."
        Check ((Hash $specialConfig) -eq $specialConfigHash) "$operation changed personal settings before rejecting $($marker.Name)."
        Check (@(Get-ChildItem -LiteralPath $specialCase.Data -Force).Count -eq 1) "$operation created a configuration snapshot before rejecting $($marker.Name)."
    }
}
$case = NewCase 'normal'
$null = InstallRelease $case $release10
$active = Join-Path $case.Install 'Launcher.exe'
$preserved = Join-Path $case.Install 'Launcher-R1.0.exe'
Check (!((Get-Item -LiteralPath $active).Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Release alias unexpectedly needs a symbolic-link privilege.'
Check ((Hash $preserved) -eq $releaseHash) 'Versioned release was not preserved.'
Check ((Hash (Join-Path $case.Install 'LICENSE.txt')) -eq (Hash (Join-Path $release10 'LICENSE.txt'))) 'Initial release did not install its license.'
foreach ($name in $exampleNames) {
    Check ((Hash (Join-Path $case.Install $name)) -eq (Hash (Join-Path $release10 $name))) "Initial release did not install $name."
    Check (!(Test-Path -LiteralPath (Join-Path $case.Install ([IO.Path]::GetFileName($name))))) "Initial release also installed a root copy of $name."
}
$startup = Join-Path $run 'Startup'
[void][IO.Directory]::CreateDirectory($startup)
$null = & (Join-Path $repository 'tools\install-startup-shortcut.ps1') -InstallDirectory $case.Install -StartupDirectory $startup
$shell = New-Object -ComObject WScript.Shell
Check ($shell.CreateShortcut((Join-Path $startup 'Launcher.lnk')).TargetPath -eq $active) 'Startup shortcut does not target the permanent name.'
$savedShortcut = $shell.CreateShortcut((Join-Path $startup 'Launcher.lnk'))
$savedShortcut.Arguments = '--config "custom.toml"'
$savedShortcut.Save()
$null = & (Join-Path $repository 'tools\install-startup-shortcut.ps1') -InstallDirectory $case.Install -StartupDirectory $startup
Check ($shell.CreateShortcut((Join-Path $startup 'Launcher.lnk')).Arguments -eq '--config "custom.toml"') 'Refreshing the Startup shortcut changed its configured arguments.'
$config = Join-Path $case.Data 'Launcher.toml'
$snapshot = Join-Path $case.Data 'Launcher-R1.0.toml'
WriteText $config "release configuration`r`n"
WriteText (Join-Path $case.Data 'Launcher.runtime.toml') 'personal runtime'
$personalPaletteHashes = @{}
foreach ($name in $paletteNames) {
    $palette = Join-Path $case.Install $name
    WriteText $palette "personal $name"
    $personalPaletteHashes[$name] = Hash $palette
}
$configHash = Hash $config
$null = AcceptBuild $case
Check (!((Get-Item -LiteralPath $active).Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Development entry still links to the release.'
Check ((Hash $active) -eq $buildHash) 'Development executable was not accepted.'
Check ((Hash (Join-Path $case.Install 'LICENSE.txt')) -eq (Hash (Join-Path $SourceDirectory 'LICENSE.txt'))) 'Development acceptance retained an older license.'
Check ((Hash $snapshot) -eq $configHash) 'First configuration snapshot differs from the original bytes.'
Check ([IO.File]::ReadAllText((Join-Path $case.Data 'Launcher-R1.0.runtime.toml')) -eq 'personal runtime') 'First release snapshot did not preserve its sibling runtime.'
Check ((Hash $config) -eq $configHash) 'Live configuration changed.'
Check ((Hash $preserved) -eq $releaseHash) 'Writing the development build changed the release target.'
foreach ($name in $paletteNames) {
    Check ((Hash (Join-Path $case.Install $name)) -eq $personalPaletteHashes[$name]) "Development acceptance changed the existing $name."
}
Check ([IO.File]::ReadAllText((Join-Path $case.Data 'Launcher.runtime.toml')) -eq 'personal runtime') 'Runtime settings changed.'
WriteText $config 'new development-only configuration'
$null = AcceptBuild $case
Check ((Hash $snapshot) -eq $configHash) 'Later development update replaced the first snapshot.'
Check (@(Get-ChildItem -LiteralPath $case.Data -Filter 'Launcher-R*.toml' | Where-Object Name -NotLike '*.runtime.toml').Count -eq 1) 'Later update created extra release snapshots.'
Check ($shell.CreateShortcut((Join-Path $startup 'Launcher.lnk')).TargetPath -eq $active) 'Changing the executable changed the Startup target.'

$null = InstallRelease $case $release12
Check ((Hash (Join-Path $case.Install 'LICENSE.txt')) -eq (Hash (Join-Path $release12 'LICENSE.txt'))) 'Release upgrade retained an older license.'
foreach ($name in $paletteNames) {
    Check ((Hash (Join-Path $case.Install $name)) -eq $personalPaletteHashes[$name]) "Release upgrade changed the existing $name."
}
$newConfigHash = Hash $config
$null = AcceptBuild $case
Check ((Hash (Join-Path $case.Data 'Launcher-R1.2.toml')) -eq $newConfigHash) 'Next release transition did not create its own snapshot.'
Check ((Hash $snapshot) -eq $configHash) 'Next release transition changed the older snapshot.'

$legacyCase = NewCase 'legacy'
[IO.File]::Copy((Join-Path $release10 'Launcher.exe'), (Join-Path $legacyCase.Install 'Launcher.exe'))
[IO.File]::Copy((Join-Path $SourceDirectory 'Launcher.exe'), (Join-Path $legacyCase.Install 'Launcher-WIP.exe'))
WriteText (Join-Path $legacyCase.Data 'Launcher.toml') 'migration settings'
$legacyResult = AcceptBuild $legacyCase
Check ((Hash $legacyResult.ReleaseExecutable) -eq $releaseHash) 'Legacy release migration changed its bytes.'
Check ((Hash $legacyResult.LegacyExecutableBackup) -eq $buildHash) 'Legacy development executable was not preserved.'
Check (!(Test-Path -LiteralPath (Join-Path $legacyCase.Install 'Launcher-WIP.exe'))) 'Old active WIP name was not retired.'
foreach ($name in $exampleNames) {
    Check ((Hash (Join-Path $legacyCase.Install $name)) -eq (Hash (Join-Path $SourceDirectory $name))) "Development acceptance did not seed $name."
}

$missingCase = NewCase 'no-configuration'
$null = InstallRelease $missingCase $release10
$null = AcceptBuild $missingCase
Check (!(Test-Path -LiteralPath (Join-Path $missingCase.Data 'Launcher-R1.0.toml'))) 'Missing configuration produced an invented snapshot.'
WriteText (Join-Path $missingCase.Data 'Launcher.toml') 'first created by development'
$null = AcceptBuild $missingCase
Check (!(Test-Path -LiteralPath (Join-Path $missingCase.Data 'Launcher-R1.0.toml'))) 'Later development settings were mislabeled as release settings.'

$failureCase = NewCase 'snapshot-failure'
$null = InstallRelease $failureCase $release10
WriteText (Join-Path $failureCase.Data 'Launcher.toml') 'must be preserved'
[void][IO.Directory]::CreateDirectory((Join-Path $failureCase.Data 'Launcher-R1.0.toml'))
$failed = $false
try { $null = AcceptBuild $failureCase } catch { $failed = $true }
Check $failed 'A failed snapshot did not stop acceptance.'
Check ((Hash (Join-Path $failureCase.Install 'Launcher.exe')) -eq $releaseHash) 'Snapshot failure replaced the active release.'
Check (!((Get-Item -LiteralPath (Join-Path $failureCase.Install 'Launcher.exe')).Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Snapshot failure changed the release alias to a reparse point.'

foreach ($lockedName in @('Launcher.default.toml', 'Launcher.installed.default.toml', 'LICENSE.txt', 'README.txt', 'Launcher.exe')) {
    $upgradeCase = NewCase ("upgrade-failure-$lockedName")
    $null = InstallRelease $upgradeCase $release10
    $upgradeActive = Join-Path $upgradeCase.Install 'Launcher.exe'
    if ($lockedName -eq 'Launcher.exe') {
        # Lock a regular entry: opening a symlink locks its target instead of
        # reliably preventing the active link itself from being renamed.
        Remove-Item -LiteralPath $upgradeActive -ErrorAction Stop
        [IO.File]::Copy((Join-Path $release10 'Launcher.exe'), $upgradeActive)
    }
    $wasLink = [bool]((Get-Item -LiteralPath $upgradeActive).Attributes -band [IO.FileAttributes]::ReparsePoint)
    $upgradeConfig = Join-Path $upgradeCase.Data 'Launcher.toml'
    $upgradeRuntime = Join-Path $upgradeCase.Data 'Launcher.runtime.toml'
    $preservedPalettes = @('examples\colors-light.toml', 'examples\basic_colors.toml')
    $missingPalettes = @('examples\colors-dark.toml', 'examples\extra_colors.toml')
    WriteText $upgradeConfig 'personal configuration'
    WriteText $upgradeRuntime 'personal runtime'
    $paletteBefore = @{}
    foreach ($name in $preservedPalettes) {
        $palette = Join-Path $upgradeCase.Install $name
        WriteText $palette "personal $name"
        $paletteBefore[$name] = Hash $palette
    }
    foreach ($name in $missingPalettes) {
        Remove-Item -LiteralPath (Join-Path $upgradeCase.Install $name) -ErrorAction Stop
    }
    $configBefore = Hash $upgradeConfig
    $runtimeBefore = Hash $upgradeRuntime
    $lockedFile = [IO.File]::Open((Join-Path $upgradeCase.Install $lockedName),
        [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $failed = $false
    try { $null = InstallRelease $upgradeCase $release12 }
    catch { $failed = $true }
    finally { $lockedFile.Dispose() }
    Check $failed "Upgrade did not report the locked $lockedName."
    Check ((Hash $upgradeActive) -eq $releaseHash) "Failed upgrade changed the active executable with $lockedName locked."
    Check ([bool]((Get-Item -LiteralPath $upgradeActive).Attributes -band [IO.FileAttributes]::ReparsePoint) -eq $wasLink) "Failed upgrade changed the active entry type with $lockedName locked."
    foreach ($name in @('Launcher.default.toml', 'Launcher.installed.default.toml', 'LICENSE.txt', 'THIRD-PARTY-NOTICES.txt', 'README.txt')) {
        Check ((Hash (Join-Path $upgradeCase.Install $name)) -eq (Hash (Join-Path $release10 $name))) "Failed upgrade changed $name with $lockedName locked."
    }
    foreach ($name in $missingPalettes) {
        Check (!(Test-Path -LiteralPath (Join-Path $upgradeCase.Install $name))) "Failed upgrade left newly installed $name with $lockedName locked."
    }
    foreach ($name in $preservedPalettes) {
        Check ((Hash (Join-Path $upgradeCase.Install $name)) -eq $paletteBefore[$name]) "Failed upgrade changed existing $name with $lockedName locked."
    }
    Check (@(Get-ChildItem -LiteralPath $upgradeCase.Install -Filter '.launcher-install-*' -Force).Count -eq 0) "Failed upgrade left staging files with $lockedName locked."

    $null = InstallRelease $upgradeCase $release12
    Check ((Hash $upgradeActive) -eq (Hash (Join-Path $release12 'Launcher.exe'))) "Upgrade retry did not activate the new release after unlocking $lockedName."
    Check (!((Get-Item -LiteralPath $upgradeActive).Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Upgrade retry created a symbolic link.'
    foreach ($name in (@('Launcher.default.toml', 'Launcher.installed.default.toml', 'LICENSE.txt', 'THIRD-PARTY-NOTICES.txt', 'README.txt') + $missingPalettes)) {
        Check ((Hash (Join-Path $upgradeCase.Install $name)) -eq (Hash (Join-Path $release12 $name))) "Upgrade retry did not install $name."
    }
    Check ((Hash (Join-Path $upgradeCase.Install 'Launcher-R1.0.exe')) -eq $releaseHash) 'Upgrade changed the preserved release.'
    Check ((Hash $upgradeConfig) -eq $configBefore) 'Upgrade changed the personal configuration.'
    Check ((Hash $upgradeRuntime) -eq $runtimeBefore) 'Upgrade changed personal runtime settings.'
    foreach ($name in $preservedPalettes) {
        Check ((Hash (Join-Path $upgradeCase.Install $name)) -eq $paletteBefore[$name]) "Upgrade changed existing $name."
    }
    Check (@(Get-ChildItem -LiteralPath $upgradeCase.Install -Filter '.launcher-install-*' -Force).Count -eq 0) 'Upgrade retry left staging files.'
}

# A failed final activation must also remove an examples directory first
# created by that attempt, after rolling back all its newly published files.
$directoryCase = NewCase 'examples-directory-rollback'
$null = InstallRelease $directoryCase $release10
$directoryActive = Join-Path $directoryCase.Install 'Launcher.exe'
$examplesDirectory = Join-Path $directoryCase.Install 'examples'
foreach ($name in $exampleNames) {
    Remove-Item -LiteralPath (Join-Path $directoryCase.Install $name) -ErrorAction Stop
}
[IO.Directory]::Delete($examplesDirectory)
Remove-Item -LiteralPath $directoryActive -ErrorAction Stop
[IO.File]::Copy((Join-Path $release10 'Launcher.exe'), $directoryActive)
$lockedActive = [IO.File]::Open($directoryActive, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
$failed = $false
try { $null = InstallRelease $directoryCase $release12 }
catch { $failed = $true }
finally { $lockedActive.Dispose() }
Check $failed 'An active-executable lock did not stop the fresh examples installation.'
Check ((Hash $directoryActive) -eq $releaseHash) 'Fresh examples rollback changed the active executable.'
Check (!(Test-Path -LiteralPath $examplesDirectory)) 'Failed installation left its newly created examples directory.'
foreach ($name in @('Launcher.default.toml', 'Launcher.installed.default.toml', 'LICENSE.txt', 'THIRD-PARTY-NOTICES.txt', 'README.txt')) {
    Check ((Hash (Join-Path $directoryCase.Install $name)) -eq (Hash (Join-Path $release10 $name))) "Fresh examples rollback changed $name."
}
Check (@(Get-ChildItem -LiteralPath $directoryCase.Install -Filter '.launcher-install-*' -Force).Count -eq 0) 'Fresh examples rollback left staging files.'

$keepCase = NewCase 'upgrade-keep-active'
$null = InstallRelease $keepCase $release10
$keepActive = Join-Path $keepCase.Install 'Launcher.exe'
$keepPalettes = @('examples\colors-light.toml', 'examples\extra_colors.toml')
$seedPalettes = @('examples\colors-dark.toml', 'examples\basic_colors.toml')
$keepPaletteHashes = @{}
foreach ($name in $keepPalettes) {
    $palette = Join-Path $keepCase.Install $name
    WriteText $palette "personal $name"
    $keepPaletteHashes[$name] = Hash $palette
}
foreach ($name in $seedPalettes) {
    Remove-Item -LiteralPath (Join-Path $keepCase.Install $name) -ErrorAction Stop
}
$null = & (Join-Path $repository 'tools\install-release.ps1') -InstallDirectory $keepCase.Install -SourceDirectory $release12 -KeepActive -NoStart
Check ((Hash $keepActive) -eq $releaseHash) 'KeepActive changed the active executable.'
Check (!((Get-Item -LiteralPath $keepActive).Attributes -band [IO.FileAttributes]::ReparsePoint)) 'KeepActive changed the alias type.'
foreach ($name in @('Launcher.default.toml', 'Launcher.installed.default.toml', 'LICENSE.txt', 'THIRD-PARTY-NOTICES.txt', 'README.txt')) {
    Check ((Hash (Join-Path $keepCase.Install $name)) -eq (Hash (Join-Path $release10 $name))) "KeepActive replaced the active release's $name."
}
foreach ($name in $keepPalettes) {
    Check ((Hash (Join-Path $keepCase.Install $name)) -eq $keepPaletteHashes[$name]) "KeepActive changed existing $name."
}
foreach ($name in $seedPalettes) {
    Check ((Hash (Join-Path $keepCase.Install $name)) -eq (Hash (Join-Path $release12 $name))) "KeepActive did not seed missing $name."
}
Check ((Hash (Join-Path $keepCase.Install 'Launcher-R1.0.exe')) -eq $releaseHash) 'KeepActive changed the preserved release.'
Check (@(Get-ChildItem -LiteralPath $keepCase.Install -Filter '.launcher-install-*' -Force).Count -eq 0) 'KeepActive left staging files.'

$originalReleaseHash = Hash $preserved
[IO.File]::AppendAllText((Join-Path $release10 'Launcher.exe'), 'different bytes')
$failed = $false
try { $null = InstallRelease $case $release10 } catch { $failed = $true }
Check $failed 'A conflicting versioned release was overwritten.'
Check ((Hash $preserved) -eq $originalReleaseHash) 'The conflicting install modified the preserved release.'

# Repeated ordinary builds share the revision-zero resource version. Preserve
# the old canonical entry under a unique name before publishing different bytes.
$devCollision = NewCase 'same-version-development'
$sourceExe = Join-Path $SourceDirectory 'Launcher.exe'
if (!(Test-LauncherFinalRelease $sourceExe)) {
    $first = Publish-LauncherDevelopmentVersion $sourceExe $devCollision.Install
    $alias = Join-Path $devCollision.Install 'Launcher.exe'
    New-LauncherReleaseLink $alias $first.Path
    $different = Join-Path $run 'different-build.exe'
    [IO.File]::Copy($sourceExe, $different, $false)
    [IO.File]::AppendAllText($different, 'owned development PE overlay fixture')
    $second = Publish-LauncherDevelopmentVersion $different $devCollision.Install
    Check ([bool]$second.RetainedExecutable) 'Same-version development update has no unique retained file.'
    Check ((Hash $second.RetainedExecutable) -eq $first.Hash) 'Same-version development update changed the retained bytes.'
    Check ((Hash $alias) -eq $first.Hash) 'Publishing a new version wrote through the old active alias.'
    Check ((Hash $second.Path) -eq (Hash $different)) 'Same-version development update did not publish the new bytes.'
    $stageAlias = Join-Path $devCollision.Install 'Launcher.staging.exe'
    New-LauncherReleaseLink $stageAlias $second.Path
    $null = Set-LauncherActiveEntry $devCollision.Install $stageAlias
    Check ((Hash $alias) -eq $second.Hash) 'Replacing the active alias did not activate the new build.'
    Check ((Hash $second.RetainedExecutable) -eq $first.Hash) 'Replacing the active alias altered the retained build.'
}

$finalCase = NewCase 'explicit-final-cleanup'
$finalSource = NewRelease '1.6' 'FinalRelease16' 'Launcher distribution: Release'
Check ([Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $finalSource 'Launcher.exe')).Comments -ceq 'Launcher distribution: Release') 'Final fixture lacks its explicit distribution marker.'
$null = InstallRelease $finalCase $release10
$null = & (Join-Path $repository 'tools\install-release.ps1') -InstallDirectory $finalCase.Install -SourceDirectory $release12 -KeepActive -NoStart
$null = InstallRelease $finalCase $finalSource
Check (!(Test-Path -LiteralPath (Join-Path $finalCase.Install 'Launcher-R1.0.exe'))) 'Explicit final release kept its superseded active executable.'
Check (!(Test-Path -LiteralPath (Join-Path $finalCase.Install 'Launcher-R1.2.exe'))) 'Final cleanup kept an older inactive version.'
Check ((Hash (Join-Path $finalCase.Install 'Launcher.exe')) -eq (Hash (Join-Path $finalSource 'Launcher.exe'))) 'Final cleanup did not leave the new version active.'

# Match the real development cycle: a stable release, two separately packaged
# betas accepted for development use, then the next final release.
$cycle = NewCase 'release-beta-final-cleanup'
$cycle12 = NewRelease '1.2' 'CycleStable12' 'Launcher distribution: Release'
$cycle1304 = NewRelease '1.3.0.4' 'CycleBeta1304' 'Launcher distribution: Prerelease'
$cycle1305 = NewRelease '1.3.0.5' 'CycleBeta1305' 'Launcher distribution: Prerelease'
$cycle14 = NewRelease '1.4' 'CycleFinal14' 'Launcher distribution: Release'
$null = InstallRelease $cycle $cycle12
WriteText (Join-Path $cycle.Data 'Launcher.toml') 'cycle personal config'
WriteText (Join-Path $cycle.Data 'Launcher.runtime.toml') 'cycle personal controls'
$null = AcceptBuild $cycle $cycle1304
$null = AcceptBuild $cycle $cycle1305
$cycleMain = Join-Path $cycle.Install 'Launcher.exe'
$cycleOldHash = Hash $cycleMain
$cycleOldPaths = @(Get-ChildItem -LiteralPath $cycle.Install -Filter 'Launcher*.exe' -File | Where-Object Name -NE 'Launcher.exe' | ForEach-Object { $_.FullName })
Check ($cycleOldPaths.Count -ge 4) 'Beta cycle did not retain both betas and prior executable backups.'

# An equal version, newer version, foreign binary, and mismatched basename all
# survive lower-version cleanup even when their names look like Launcher files.
$equalBackup = Join-Path $cycle.Install ('Launcher-R1.4.20261023-120000.' + [Guid]::NewGuid().ToString('N') + '.backup.exe')
[IO.File]::Copy((Join-Path $cycle14 'Launcher.exe'), $equalBackup)
$newer = Join-Path $cycle.Install 'Launcher-R1.6.exe'
[IO.File]::Copy((Join-Path $finalSource 'Launcher.exe'), $newer)
$foreign = Join-Path $cycle.Install 'Launcher-R0.2.exe'
WriteText $foreign 'foreign bytes'
$mismatched = Join-Path $cycle.Install 'Launcher-R0.4.exe'
[IO.File]::Copy((Join-Path $cycle1304 'Launcher.exe'), $mismatched)
$preservedPaths = @($equalBackup, $newer, $foreign, $mismatched)
$preservedHashes = @{}
foreach ($path in $preservedPaths) { $preservedHashes[$path] = Hash $path }
$lockedNotice = [IO.File]::Open((Join-Path $cycle.Install 'LICENSE.txt'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
$failed = $false
try { $null = InstallRelease $cycle $cycle14 } catch { $failed = $true } finally { $lockedNotice.Dispose() }
Check $failed 'Final-cycle failure fixture did not reject a locked supporting file.'
Check ((Hash $cycleMain) -eq $cycleOldHash) 'Failed final installation changed the active beta.'
foreach ($path in $cycleOldPaths) { Check (Test-Path -LiteralPath $path) 'Failed final installation removed an older executable.' }

$null = & (Join-Path $repository 'tools\install-release.ps1') -InstallDirectory $cycle.Install -SourceDirectory $cycle14 -KeepActive -NoStart
Check ((Hash $cycleMain) -eq $cycleOldHash) 'KeepActive replaced the active beta.'
foreach ($path in $cycleOldPaths) { Check (Test-Path -LiteralPath $path) 'KeepActive removed an older executable.' }
$null = InstallRelease $cycle $cycle14
Check ((Hash $cycleMain) -eq (Hash (Join-Path $cycle14 'Launcher.exe'))) 'Final cycle did not activate R1.4.'
foreach ($path in $cycleOldPaths) { Check (!(Test-Path -LiteralPath $path)) 'Final cycle retained an older versioned executable or recognized backup.' }
Check (@(Get-ChildItem -LiteralPath $cycle.Install -Filter '*.backup.exe' -File | Where-Object FullName -NE $equalBackup).Count -eq 0) 'Final cycle retained a newly created older active-alias backup.'
foreach ($path in $preservedPaths) { Check ((Hash $path) -eq $preservedHashes[$path]) 'Final cleanup changed an equal/newer, foreign or mismatched executable.' }
Check ([IO.File]::ReadAllText((Join-Path $cycle.Data 'Launcher.toml')) -eq 'cycle personal config') 'Final cleanup changed the current personal configuration.'
Check ([IO.File]::ReadAllText((Join-Path $cycle.Data 'Launcher.runtime.toml')) -eq 'cycle personal controls') 'Final cleanup changed the current personal controls.'

# Installing a lower final release must also retain the higher active version's
# newly created alias backup; cleanup is ordered numerically, never by age/name.
$downgrade = NewCase 'final-older-than-active'
$null = InstallRelease $downgrade $finalSource
$downgradeResult = InstallRelease $downgrade $cycle14
Check ([bool]$downgradeResult.PreviousExecutable) 'Lower final release did not retain the higher active version backup.'
Check ((Hash $downgradeResult.PreviousExecutable) -eq (Hash (Join-Path $finalSource 'Launcher.exe'))) 'Lower final release damaged the newer active backup.'
Check (Test-Path -LiteralPath (Join-Path $downgrade.Install 'Launcher-R1.6.exe')) 'Lower final release deleted a newer versioned executable.'

$lockedCleanup = NewCase 'final-locked-archive'
$null = InstallRelease $lockedCleanup $cycle12
$null = AcceptBuild $lockedCleanup $cycle1304
$lockedArchive = Join-Path $lockedCleanup.Install 'Launcher-R1.2.exe'
$archiveLock = [IO.File]::Open($lockedArchive, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
$cleanupWarnings = @()
try {
    $null = & (Join-Path $repository 'tools\install-release.ps1') -InstallDirectory $lockedCleanup.Install -SourceDirectory $cycle14 -NoStart -WarningVariable cleanupWarnings -WarningAction SilentlyContinue
} finally { $archiveLock.Dispose() }
Check (Test-Path -LiteralPath $lockedArchive) 'Final cleanup removed a locked older archive.'
Check ($cleanupWarnings.Count -ge 1) 'Final cleanup did not warn that locked old executables were retained.'
Check ((Hash (Join-Path $lockedCleanup.Install 'Launcher.exe')) -eq (Hash (Join-Path $cycle14 'Launcher.exe'))) 'A locked archive prevented successful final activation.'
$null = InstallRelease $lockedCleanup $cycle14
Check (!(Test-Path -LiteralPath $lockedArchive)) 'Retry did not clean the unlocked older archive.'

$changedCleanup = NewCase 'final-changed-archive'
$null = InstallRelease $changedCleanup $cycle12
$null = AcceptBuild $changedCleanup $cycle1304
$changedArchive = Join-Path $changedCleanup.Install 'Launcher-R1.2.exe'
$beforeChangeHash = Hash $changedArchive
$global:LauncherFixtureBeforeProcessDiscovery = { [IO.File]::AppendAllText($changedArchive, 'changed after cleanup candidate snapshot') }.GetNewClosure()
$cleanupWarnings = @()
$null = & (Join-Path $repository 'tools\install-release.ps1') -InstallDirectory $changedCleanup.Install -SourceDirectory $cycle14 -NoStart -WarningVariable cleanupWarnings -WarningAction SilentlyContinue
Check (Test-Path -LiteralPath $changedArchive) 'Final cleanup removed an archive that changed after review.'
Check ((Hash $changedArchive) -ne $beforeChangeHash) 'Changed-archive fixture did not mutate at the publication boundary.'
Check ($cleanupWarnings.Count -ge 1) 'Final cleanup did not warn about a changed older archive.'

# Exercise the same acceptance transaction used by accept-build after the
# alias has been published. Failure must restore the old runnable installation.
foreach ($hadActive in @($true, $false)) {
    $acceptFailure = NewCase ('accept-verification-failure-' + $hadActive)
    $priorHash = $null
    if ($hadActive) {
        $null = InstallRelease $acceptFailure $release10
        $priorHash = Hash (Join-Path $acceptFailure.Install 'Launcher.exe')
        foreach ($name in $exampleNames) { [IO.File]::Delete((Join-Path $acceptFailure.Install $name)) }
        [IO.Directory]::Delete((Join-Path $acceptFailure.Install 'examples'))
    }
    $candidate = Publish-LauncherDevelopmentVersion $sourceExe $acceptFailure.Install
    $failed = $false
    try {
        $null = Install-LauncherAcceptedBuild -Directory $acceptFailure.Install -SourceDirectory $SourceDirectory `
            -VersionedExecutable $candidate.Path -Sha256 $candidate.Hash -VerifyActive { throw 'forced verification failure after activation' }
    } catch { $failed = $_.Exception.Message -like 'forced verification failure*' }
    Check $failed 'Post-activation failure did not propagate from acceptance.'
    $failedActive = Join-Path $acceptFailure.Install 'Launcher.exe'
    if ($hadActive) { Check ((Hash $failedActive) -eq $priorHash) 'Failed acceptance left the new alias active.' }
    else { Check (!(Test-Path -LiteralPath $failedActive)) 'Failed fresh acceptance left a new active alias.' }
    foreach ($name in @('Launcher.default.toml', 'Launcher.installed.default.toml', 'LICENSE.txt', 'THIRD-PARTY-NOTICES.txt')) {
        $destination = Join-Path $acceptFailure.Install $name
        if ($hadActive) { Check ((Hash $destination) -eq (Hash (Join-Path $release10 $name))) "Failed acceptance did not restore $name."
        } else { Check (!(Test-Path -LiteralPath $destination)) "Failed fresh acceptance left $name." }
    }
    Check (!(Test-Path -LiteralPath (Join-Path $acceptFailure.Install 'examples'))) 'Failed acceptance left newly installed examples.'
    Check (@(Get-ChildItem -LiteralPath $acceptFailure.Install -Force -Filter '.launcher-accept-*').Count -eq 0) 'Failed acceptance left staging entries.'
    Check ((Hash $candidate.Path) -eq $candidate.Hash) 'Failed acceptance changed its retained versioned executable.'
}

# Bundled companions refresh with ordinary acceptance; historical source-only
# fixtures above deliberately omit the optional extractor and remain supported.
$extractorSource = Join-Path $SourceDirectory 'LauncherSvgExtractor.exe'
if (Test-Path -LiteralPath $extractorSource -PathType Leaf) {
    $bundled = NewRelease '1.8' 'BundledFinal18' 'Launcher distribution: Release'
    $bundledBeta = NewRelease '1.9' 'BundledBeta19' 'Launcher distribution: Prerelease'
    $htmlGuideNames = @('Launcher.html', 'LauncherDrop.html', 'Installer.html', 'SvgExtractor.html', 'advanced-icon-catalog.html',
        'FileLocations.html', 'Configuration.html', 'LaunchTargets.html', 'Appearance.html')
    $guideNames = @('Launcher.md', 'LauncherDrop.md', 'Installer.md', 'SvgExtractor.md', 'advanced-icon-catalog.md') + $htmlGuideNames
    $maintenanceNames = @('Maintenance.ps1', 'Uninstall.cmd', 'Discovery.ps1', 'Processes.ps1', 'Lifecycle.ps1', 'Deployment.ps1', 'Registration.ps1', 'WindowTextQuery.cs')
    $bundledSupportNames = @('LauncherSvgExtractor.exe') + @($guideNames | ForEach-Object { 'docs\' + $_ }) + $maintenanceNames
    foreach ($bundle in @($bundled, $bundledBeta)) {
        [IO.File]::Copy($extractorSource, (Join-Path $bundle 'LauncherSvgExtractor.exe'))
        [void][IO.Directory]::CreateDirectory((Join-Path $bundle 'docs'))
        foreach ($guide in $guideNames) { WriteText (Join-Path $bundle ('docs\' + $guide)) "$bundle $guide" }
        foreach ($name in $maintenanceNames) { WriteText (Join-Path $bundle $name) "$bundle unexecuted maintenance fixture $name" }
    }
    [IO.File]::AppendAllText((Join-Path $bundledBeta 'LauncherSvgExtractor.exe'), 'updated companion fixture')
    $companionCase = NewCase 'bundled-companion'
    $null = InstallRelease $companionCase $bundled
    foreach ($name in $bundledSupportNames) {
        Check ((Hash (Join-Path $companionCase.Install $name)) -eq (Hash (Join-Path $bundled $name))) "Release installation omitted $name."
    }
    $null = AcceptBuild $companionCase $bundledBeta
    foreach ($name in $bundledSupportNames) {
        Check ((Hash (Join-Path $companionCase.Install $name)) -eq (Hash (Join-Path $bundledBeta $name))) "Development acceptance did not refresh $name."
    }
    $companionHash = Hash (Join-Path $companionCase.Install 'LauncherSvgExtractor.exe')
    $failed = $false
    $candidate = Publish-LauncherDevelopmentVersion (Join-Path $bundledBeta 'Launcher.exe') $companionCase.Install
    try {
        $null = Install-LauncherAcceptedBuild -Directory $companionCase.Install -SourceDirectory $bundled `
            -VersionedExecutable $candidate.Path -Sha256 $candidate.Hash -VerifyActive { throw 'forced companion rollback' }
    } catch { $failed = $_.Exception.Message -like 'forced companion rollback*' }
    Check $failed 'Companion acceptance ignored post-activation failure.'
    Check ((Hash (Join-Path $companionCase.Install 'LauncherSvgExtractor.exe')) -eq $companionHash) 'Failed acceptance did not restore the previously installed companion.'
    foreach ($relative in $maintenanceNames) {
        Check ((Hash (Join-Path $companionCase.Install $relative)) -eq (Hash (Join-Path $bundledBeta $relative))) "Failed acceptance did not restore $relative."
    }
    foreach ($guide in $guideNames) {
        $relative = 'docs\' + $guide
        Check ((Hash (Join-Path $companionCase.Install $relative)) -eq (Hash (Join-Path $bundledBeta $relative))) "Failed acceptance did not restore $relative."
    }
    # Historical source-only installations have no docs directory. Failed
    # acceptance must remove the newly added HTML rather than leaving a mixed
    # installation or an empty docs directory behind.
    $freshHtmlCase = NewCase 'html-first-acceptance-rollback'
    $null = InstallRelease $freshHtmlCase $release12
    $priorHash = Hash (Join-Path $freshHtmlCase.Install 'Launcher.exe')
    $candidate = Publish-LauncherDevelopmentVersion (Join-Path $bundledBeta 'Launcher.exe') $freshHtmlCase.Install
    $failed = $false
    try {
        $null = Install-LauncherAcceptedBuild -Directory $freshHtmlCase.Install -SourceDirectory $bundledBeta `
            -VersionedExecutable $candidate.Path -Sha256 $candidate.Hash -VerifyActive { throw 'forced HTML rollback' }
    } catch { $failed = $_.Exception.Message -like 'forced HTML rollback*' }
    Check $failed 'First HTML acceptance ignored post-activation failure.'
    Check ((Hash (Join-Path $freshHtmlCase.Install 'Launcher.exe')) -eq $priorHash) 'Failed HTML acceptance did not restore the prior active executable.'
    foreach ($guide in $htmlGuideNames) { Check (!(Test-Path -LiteralPath (Join-Path $freshHtmlCase.Install ('docs\' + $guide)))) "Failed acceptance left newly installed $guide." }
    foreach ($relative in $maintenanceNames) { Check (!(Test-Path -LiteralPath (Join-Path $freshHtmlCase.Install $relative))) "Failed acceptance left newly installed $relative." }
    Check (!(Test-Path -LiteralPath (Join-Path $freshHtmlCase.Install 'docs'))) 'Failed HTML acceptance left its newly created docs directory.'
    $fakeMain = Join-Path $companionCase.Install 'must-not-start.exe'
    $global:LauncherFixtureProcesses = @(
        [pscustomobject]@{ ExecutablePath = $fakeMain },
        [pscustomobject]@{ ExecutablePath = Join-Path $companionCase.Install 'LauncherSvgExtractor.exe' }
    )
    $failed = $false
    try { Stop-LauncherExecutables @($fakeMain) }
    catch { $failed = $_.Exception.Message -like 'Close SVG Extractor*' }
    finally { $global:LauncherFixtureProcesses = @() }
    Check $failed 'SVG Extractor preflight did not run before the first main-executable stop attempt.'
} else { Write-Output 'Optional developer companion checks skipped: this source-only build predates the bundled extractor.' }
Write-Output "Installation checks passed: $script:checks; fixtures: $run"
