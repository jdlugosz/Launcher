[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $Launcher,
    [Parameter(Mandatory)][string] $Fixture,
    [Parameter(Mandatory)][string] $ExpectedVersion,
    [Parameter(Mandatory)][string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
$displayScreens = @([System.Windows.Forms.Screen]::AllScreens |
    Sort-Object @{ Expression = 'Primary'; Descending = $true }, DeviceName)
if (!$displayScreens.Count) { throw 'No connected displays were discovered.' }

$displayLauncher = [IO.Path]::GetFullPath($Launcher)
$displayFixture = [IO.File]::ReadAllText([IO.Path]::GetFullPath($Fixture))
if (!$displayFixture.Contains('display = "primary"') -or
    !$displayFixture.Contains('edge = "top"')) {
    throw 'The display test fixture must specify primary/top as its base.'
}
$displayRun = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ([Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($displayRun)
$displayResults = @()
$displayIndex = 0
foreach ($displayScreen in $displayScreens) {
    # Two orientations per monitor. Avoid the primary Windows taskbar edge;
    # the secondary display also exercises the bottom-facing horizontal tabs.
    $displayHorizontal = if ($displayScreen.Primary) { 'top' } else { 'bottom' }
    foreach ($displayEdge in @($displayHorizontal, 'right')) {
        $displayDevice = $displayScreen.DeviceName
        if ($displayDevice.Contains("'")) { throw 'Unexpected display device name.' }
        $displayConfig = Join-Path $displayRun ("display-$displayIndex-$displayEdge.toml")
        $displayCaseLog = Join-Path $displayRun ("display-$displayIndex-$displayEdge.log")
        $displayText = $displayFixture.Replace('display = "primary"',
            ("display = '" + $displayDevice + "'")).Replace('edge = "top"',
            ('edge = "' + $displayEdge + '"'))
        # Resolve the filename beside this temporary configuration. No shared
        # AppData log is opened, even while the normal Launcher is running.
        $displayText += "`n[logging]`nfile = 'display-$displayIndex-$displayEdge.log'`n"
        [IO.File]::WriteAllText($displayConfig, $displayText, [Text.UTF8Encoding]::new($false))
        $displayProcess = Start-Process -FilePath $displayLauncher -ArgumentList @(
            '--smoke-test', '--config', ('"' + $displayConfig + '"')) -WindowStyle Hidden -PassThru
        try {
            if (!$displayProcess.WaitForExit(10000)) {
                $displayProcess.Kill()
                $displayProcess.WaitForExit()
                throw "Timed out testing $displayDevice/$displayEdge."
            }
            $displayProcess.Refresh()
            $displayPid = $displayProcess.Id
            $displayExit = $displayProcess.ExitCode
        } finally {
            $displayProcess.Dispose()
        }
        # Include rotation of this case's own log, retaining the PID/path checks.
        $displayLines = @(@("$displayCaseLog.1", $displayCaseLog) | ForEach-Object {
            if (Test-Path -LiteralPath $_) {
                [IO.File]::ReadAllLines($_) |
                    Where-Object { $_ -match (' pid=' + $displayPid + ' ') }
            }
        })
        $displayStart = @($displayLines | Where-Object {
            $_.Contains('Starting Launcher ' + $ExpectedVersion + '; config=' + $displayConfig + '.')
        })
        if ($displayStart.Count -ne 1) {
            throw "Missing or ambiguous config-start evidence for $displayDevice/$displayEdge."
        }
        $displayLines = @($displayLines | Select-Object -Skip (
            [Array]::IndexOf($displayLines, $displayStart[0])))
        if ($displayExit -ne 0) {
            $displayLines | Select-Object -Last 8 | Write-Output
            throw "Native smoke failed for $displayDevice/$displayEdge (exit $displayExit)."
        }
        $displayPattern = 'Dock placement: display=' + [regex]::Escape($displayDevice) +
            '; edge=' + $displayEdge + '; dpi=(\d+);'
        $displayPlacement = @($displayLines | Where-Object { $_ -match $displayPattern })
        $displayAllPlacements = @($displayLines | Where-Object { $_ -match 'Dock placement: display=' })
        if (!$displayPlacement.Count -or $displayPlacement.Count -ne $displayAllPlacements.Count -or
            !($displayLines | Where-Object { $_ -match 'Smoke test passed:' }) -or
            !($displayLines | Where-Object { $_ -match 'Smoke tab selection: pages=4;' })) {
            throw "Missing native monitor/DPI/page-selection evidence for $displayDevice/$displayEdge."
        }
        $displayDpis = @($displayPlacement | ForEach-Object {
            [int][regex]::Match($_, $displayPattern).Groups[1].Value
        } | Select-Object -Unique)
        if ($displayDpis.Count -ne 1 -or $displayDpis[0] -lt 96) {
            throw "Unexpected DPI changes or invalid DPI for $displayDevice/$displayEdge."
        }
        $displayResults += [pscustomobject]@{
            Device = $displayDevice; Primary = $displayScreen.Primary; Edge = $displayEdge
            Dpi = $displayDpis[0]; Pid = $displayPid; Exit = $displayExit; Config = $displayConfig; Log = $displayCaseLog
        }
        "$displayDevice/${displayEdge}: PASS; DPI=$($displayDpis[0])"
    }
    ++$displayIndex
}
$displayResults | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (
    Join-Path $displayRun 'results.json') -Encoding UTF8
if ($displayScreens.Count -lt 2) {
    'Second-display coverage was not exercised: only one connected display.'
}
if (@($displayResults.Dpi | Select-Object -Unique).Count -lt 2) {
    'Different-DPI coverage was not exercised: only one DPI value was observed.'
}
"Passed $($displayResults.Count) native edge cases on $($displayScreens.Count) displays. Evidence: $displayRun"
