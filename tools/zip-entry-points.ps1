#requires -Version 5.1
# Generated download entry points live beside the payload stage, never inside
# an installation. Keep their content deterministic for ZIP validation.

function Get-LauncherZipEntryPointContents {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$PackageName)

    if ($PackageName -cnotmatch '^Launcher-R([0-9]+\.[0-9]+(?:\.[0-9]+){0,2})-windows-x64$') { throw 'Invalid ZIP payload directory name.' }
    $version = $Matches[1]
    $install = @(
        '@echo off'
        'setlocal DisableDelayedExpansion'
        'rem Use native Windows PowerShell and its own modules, including from PowerShell 7.'
        'set "PSModulePath="'
        'set "LauncherSetupPowerShell=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"'
        'if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "LauncherSetupPowerShell=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"'
        ('if not exist "%~dp0' + $PackageName + '\Install.ps1" (')
        ('  echo Launcher installation files are missing: "%~dp0' + $PackageName + '\Install.ps1" 1>&2')
        '  echo Extract the complete ZIP and keep its versioned folder beside Install.cmd. 1>&2'
        '  exit /b 2'
        ')'
        ('if "%~1"=="" if exist "%~dp0' + $PackageName + '\Launcher Setup.exe" goto native_setup')
        ('"%LauncherSetupPowerShell%" -NoLogo -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0' + $PackageName + '\Install.ps1" %*')
        'exit /b %errorlevel%'
        ''
        ':native_setup'
        ('start "" /wait "%~dp0' + $PackageName + '\Launcher Setup.exe"')
        'exit /b %errorlevel%'
    )
    $profile = '%LOCALAPPDATA%\D' + [char]0x0142 + 'ugosz\Launcher'
    $portable = @(
        ("Launcher $version - portable use")
        ''
        'Extract the complete ZIP before starting. Keep the versioned folder and'
        'everything inside it together; do not run Launcher from inside the ZIP.'
        ''
        'To run without installing:'
        ("1. Open the $PackageName folder.")
        ("2. Run Launcher-R$version.exe there.")
        ''
        'For a separate portable profile, first copy Launcher.default.toml to'
        'Launcher.toml inside that versioned folder. Keep your relative Icon or'
        'Icons files beside that configuration. Runtime adjustments stay beside'
        'the selected TOML using its basename.'
        ''
        'Without an adjacent Launcher.toml, Launcher can use an existing profile'
        ("in $profile. Portable first use creates a missing adjacent")
        'configuration when neither location has one. Right-click Launcher to'
        'edit its configuration, then choose Restart after saving changes.'
        ''
        'To install or upgrade instead, run Launcher Setup.exe in the versioned'
        'folder, or run Install.cmd beside this Portable.txt. Portable use creates'
        'no Windows uninstall entry.'
        ''
        ("More help: $PackageName\README.txt and docs\Installer.md inside that folder.")
        ("License terms: $PackageName\LICENSE.txt.")
        ("Third-party notices: $PackageName\THIRD-PARTY-NOTICES.txt.")
    )
    return [ordered]@{
        'Install.cmd' = ($install -join "`r`n") + "`r`n"
        'Portable.txt' = ($portable -join "`r`n") + "`r`n"
    }
}

function Write-LauncherZipEntryPoints {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$OutputDirectory, [Parameter(Mandatory)][string]$PackageName)

    $contents = Get-LauncherZipEntryPointContents -PackageName $PackageName
    $directory = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\', '/')
    if ($directory -ieq [IO.Path]::GetPathRoot($directory).TrimEnd('\', '/')) { throw 'ZIP entry points require a dedicated output directory.' }
    for ($current = $directory; $current; $current = [IO.Path]::GetDirectoryName($current)) {
        $entry = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
        if ($entry -and (!$entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint))) { throw "ZIP entry points must use real directories: $current" }
    }
    foreach ($name in $contents.Keys) {
        $entry = Get-Item -LiteralPath (Join-Path $directory $name) -Force -ErrorAction SilentlyContinue
        if ($entry -and ($entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint))) { throw "ZIP entry point must be a regular file: $($entry.FullName)" }
    }
    [void][IO.Directory]::CreateDirectory($directory)
    $encoding = [Text.UTF8Encoding]::new($false)
    foreach ($name in $contents.Keys) {
        $destination = Join-Path $directory $name
        $temporary = Join-Path $directory ('.entry-point-' + [Guid]::NewGuid().ToString('N') + '.tmp')
        try {
            $bytes = $encoding.GetBytes($contents[$name])
            $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            try { $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
            if (Test-Path -LiteralPath $destination) { [IO.File]::Replace($temporary, $destination, [NullString]::Value) }
            else { [IO.File]::Move($temporary, $destination) }
        } finally { if (Test-Path -LiteralPath $temporary) { [IO.File]::Delete($temporary) } }
    }
}
