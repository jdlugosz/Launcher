[CmdletBinding()]
param(
    [string] $InstallDirectory = (Join-Path $env:ProgramFiles ('D' + [char]0x0142 + 'ugosz\Launcher')),
    [string] $SourceDirectory,
    [string] $UserDataDirectory = (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) ('D' + [char]0x0142 + 'ugosz\Launcher')),
    [string] $ConfigurationPath,
    [switch] $NoStart
)

# Compatibility entry point. New commands and documentation use accept-build.
& (Join-Path $PSScriptRoot 'accept-build.ps1') @PSBoundParameters
