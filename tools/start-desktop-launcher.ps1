[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $ExecutablePath,
    [string] $Arguments = '',
    [string] $WorkingDirectory
)

$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $ExecutablePath -PathType Leaf)) { throw "Launcher executable is missing: $ExecutablePath" }
if (!$WorkingDirectory) { $WorkingDirectory = Split-Path -Parent $ExecutablePath }

# Use the existing desktop Explorer so the child does not inherit Codex's
# package filesystem redirection. A new Shell.Application alone is insufficient.
$desktopShell = New-Object -ComObject Shell.Application
$desktopWindowHandle = 0
$desktopWindow = $desktopShell.Windows().FindWindowSW(0, 0, 8, [ref] $desktopWindowHandle, 1)
if (!$desktopWindow) { throw 'Desktop Explorer is unavailable. Launcher startup was not requested.' }
$desktopApplication = $desktopWindow.Document.Application
if (!$desktopApplication) { throw 'Desktop Explorer Shell access is unavailable. Launcher startup was not requested.' }
$desktopApplication.ShellExecute($ExecutablePath, $Arguments, $WorkingDirectory, 'open', 0)
Write-Output "Launcher startup requested through desktop Explorer: $ExecutablePath"
