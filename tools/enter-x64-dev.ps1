[CmdletBinding()]
param([switch] $Quiet)

# Dot-source this file from an interactive terminal. Keep script variables and
# error preferences local while initializing the current process environment.
& {
    param([bool] $QuietMode)
    $ErrorActionPreference = 'Stop'
    Set-StrictMode -Version 2.0
    $launcherVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $launcherVsWhere)) { throw 'Visual Studio Installer / vswhere.exe is missing.' }
    $launcherVsInstall = & $launcherVsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or !$launcherVsInstall) { throw 'Install the Visual Studio C++ x64 build tools.' }

    # VS initialization can replace VCPKG_ROOT with its bundled checkout. Keep
    # the user's configured checkout, which supplies this project's packages.
    $launcherVcpkg = [Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'User')
    if (!$launcherVcpkg) { $launcherVcpkg = $env:VCPKG_ROOT }
    if (!$launcherVcpkg -or !(Test-Path -LiteralPath (Join-Path $launcherVcpkg 'scripts\buildsystems\vcpkg.cmake'))) {
        throw 'Set VCPKG_ROOT to the installed vcpkg checkout.'
    }
    & (Join-Path $launcherVsInstall 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation -NoLogo | Out-Null
    $env:VCPKG_ROOT = $launcherVcpkg
    $launcherToolDirectories = @(
        (Join-Path $launcherVsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'),
        (Join-Path $launcherVsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'),
        $launcherVcpkg
    )
    foreach ($launcherToolDirectory in $launcherToolDirectories) {
        if (!(Test-Path -LiteralPath $launcherToolDirectory -PathType Container)) { throw "Missing build tool directory: $launcherToolDirectory" }
        if ($env:Path.Split(';') -notcontains $launcherToolDirectory) { $env:Path = "$launcherToolDirectory;$env:Path" }
    }
    foreach ($launcherTool in @('cl.exe', 'link.exe', 'rc.exe', 'cmake.exe', 'ninja.exe')) {
        $null = Get-Command $launcherTool -CommandType Application -ErrorAction Stop
    }
    if ($env:VSCMD_ARG_TGT_ARCH -notin @('x64', 'amd64') -or $env:VSCMD_ARG_HOST_ARCH -notin @('x64', 'amd64')) {
        throw 'Visual Studio did not initialize the x64 host and target.'
    }
    if (!$QuietMode) { Write-Host 'MSVC x64 ready: compiler, Windows SDK, CMake and Ninja.' }
} ([bool] $Quiet)
