[CmdletBinding()]
param([string] $ConfigurationPath)

$ErrorActionPreference = 'Stop'
$launcherInstall = Join-Path $env:ProgramFiles ('D' + [char]0x0142 + 'ugosz\Launcher')
$launcherExecutable = Join-Path $launcherInstall 'Launcher.exe'
if (!(Test-Path -LiteralPath $launcherExecutable -PathType Leaf)) {
    throw 'Installed Launcher is missing. Install a release or run tools\accept-build.ps1 after selected Release checks pass.'
}
if ($ConfigurationPath -and $ConfigurationPath.Contains('"')) { throw 'ConfigurationPath cannot contain a double quote.' }
$launcherArguments = ''
if ($ConfigurationPath) { $launcherArguments = '"' + $ConfigurationPath + '"' }
& (Join-Path $PSScriptRoot 'start-desktop-launcher.ps1') -ExecutablePath $launcherExecutable `
    -Arguments $launcherArguments -WorkingDirectory $launcherInstall
