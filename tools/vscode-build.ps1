[CmdletBinding()]
param([ValidateSet('Build', 'Configure')] [string] $Action = 'Build')

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'enter-x64-dev.ps1') -Quiet
$launcherProject = Split-Path -Parent $PSScriptRoot
Push-Location -LiteralPath $launcherProject
try {
    $launcherCache = Join-Path $launcherProject 'build\windows-x64\CMakeCache.txt'
    if ($Action -eq 'Configure' -or !(Test-Path -LiteralPath $launcherCache)) {
        & cmake.exe --preset windows-x64
        if ($LASTEXITCODE -ne 0) { throw 'Launcher CMake configuration failed.' }
    }
    if ($Action -eq 'Build') {
        $launcherOutput = Join-Path $launcherProject 'build\windows-x64\Release\Launcher.exe'
        if (Test-Path -LiteralPath $launcherOutput) {
            $launcherStop = Start-Process -FilePath $launcherOutput -ArgumentList '--stop' -WindowStyle Hidden -Wait -PassThru
            if ($launcherStop.ExitCode -ne 0) { throw 'The build-directory Launcher could not close cleanly.' }
        }
        & cmake.exe --build --preset release --target Launcher
        if ($LASTEXITCODE -ne 0) { throw 'Launcher Release build failed.' }
        Write-Host 'Release application built. Tests and WIP installation are separate actions.'
    }
} finally {
    Pop-Location
}
