[CmdletBinding()]
param(
    [string] $InstallDirectory = (Join-Path $env:ProgramFiles ('D' + [char]0x0142 + 'ugosz\Launcher')),
    [string] $StartupDirectory = [Environment]::GetFolderPath('Startup')
)

$ErrorActionPreference = 'Stop'
$install = [IO.Path]::GetFullPath($InstallDirectory)
$executable = Join-Path $install 'Launcher.exe'
if (!(Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Installed Launcher is missing: $executable" }
if (!(Test-Path -LiteralPath $StartupDirectory -PathType Container)) { throw 'The user Startup directory is missing.' }
$shortcutPath = Join-Path $StartupDirectory 'Launcher.lnk'
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $executable
$shortcut.WorkingDirectory = $install
# A newly created shortcut has empty arguments. Retain any explicit config
# argument on an existing shortcut when refreshing its permanent target.
$shortcut.Description = 'Launcher'
$shortcut.Save()
Write-Output "Log-in shortcut: $shortcutPath -> $executable"
