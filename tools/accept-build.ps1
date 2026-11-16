[CmdletBinding()]
param(
    [string] $InstallDirectory = (Join-Path $env:ProgramFiles ('D' + [char]0x0142 + 'ugosz\Launcher')),
    [string] $SourceDirectory,
    [string] $UserDataDirectory = (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) ('D' + [char]0x0142 + 'ugosz\Launcher')),
    [string] $ConfigurationPath,
    [switch] $NoStart
)

# Run after focused Release checks, using the ordinary desktop user's context.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'installed-launcher.ps1')
if (!$SourceDirectory) { $SourceDirectory = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\windows-x64\Release' }
$install = [IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\')
$source = [IO.Path]::GetFullPath($SourceDirectory)
$data = [IO.Path]::GetFullPath($UserDataDirectory)
Assert-LauncherDirectory $install
$examples = Join-Path $install 'examples'
if (Get-LauncherEntry $examples) { Assert-LauncherDirectory $examples }
$sourceExe = Find-LauncherSourceExecutable $source
if (!(Test-Path -LiteralPath $sourceExe -PathType Leaf)) { throw "Missing development executable: $sourceExe" }
Assert-LauncherNormalBuild $sourceExe
if ($ConfigurationPath -and $ConfigurationPath.Contains('"')) { throw 'ConfigurationPath cannot contain a double quote.' }
foreach ($name in @('Launcher.default.toml', 'LICENSE.txt', 'THIRD-PARTY-NOTICES.txt', 'examples\basic_colors.toml', 'examples\extra_colors.toml', 'examples\colors-light.toml', 'examples\colors-dark.toml')) {
    if (!(Test-Path -LiteralPath (Join-Path $source $name) -PathType Leaf)) { throw "Missing bundled file: $name" }
}
$release = Get-LauncherInstalledRelease $install
$active = Join-Path $install 'Launcher.exe'
$legacy = Join-Path $install 'Launcher-WIP.exe'
$transition = $release -and (Test-Path -LiteralPath $active -PathType Leaf) -and
    (Get-FileHash -LiteralPath $active -Algorithm SHA256).Hash -eq $release.Hash
Stop-LauncherExecutables (@($active, $legacy, $sourceExe) + @($release | ForEach-Object { $_.Path }))
$snapshot = if ($release) { Join-Path $data "Launcher-R$($release.Label).toml" } else { $null }
if ($transition) { $snapshot = Backup-LauncherConfiguration $data $release.Label }
elseif (!$snapshot -or !(Test-Path -LiteralPath $snapshot -PathType Leaf)) { $snapshot = $null }

$published = Publish-LauncherDevelopmentVersion $sourceExe $install
$hash = $published.Hash
$verifyRelease = {
    param($ActivePath)
    if ($release -and (Get-FileHash -LiteralPath $release.Path -Algorithm SHA256).Hash -ne $release.Hash) {
        throw 'Preserved release failed verification during development acceptance.'
    }
}
$prior = Install-LauncherAcceptedBuild -Directory $install -SourceDirectory $source -VersionedExecutable $published.Path -Sha256 $hash -VerifyActive $verifyRelease
$legacyBackup = $null
if (Get-LauncherEntry $legacy) {
    $legacyBackup = Join-Path $install ('Launcher-WIP.legacy.' + [Guid]::NewGuid().ToString('N') + '.backup.exe')
    Move-Item -LiteralPath $legacy -Destination $legacyBackup -ErrorAction Stop
}
[pscustomobject]@{ ActiveExecutable = $active; ReleaseExecutable = $(if ($release) { $release.Path } else { $null }); ConfigurationBackup = $snapshot;
    VersionedExecutable = $published.Path; RetainedExecutable = $published.RetainedExecutable;
    PreviousExecutable = $prior; LegacyExecutableBackup = $legacyBackup; Sha256 = $hash }
if (!$NoStart) {
    $arguments = ''
    if ($ConfigurationPath) { $arguments = '"' + $ConfigurationPath + '"' }
    & (Join-Path $PSScriptRoot 'start-desktop-launcher.ps1') -ExecutablePath $active -Arguments $arguments -WorkingDirectory $install
}
