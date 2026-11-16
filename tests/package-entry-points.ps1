#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$EntryPointScript,
    [string]$SetupExecutable,
    [string]$PackageName = 'Launcher-R1.1.0.8-windows-x64',
    [string]$OutputDirectory = [IO.Path]::GetTempPath()
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$EntryPointScript) { $EntryPointScript = Join-Path $PSScriptRoot '..\tools\zip-entry-points.ps1' }
. $EntryPointScript
$script:checks = 0
function Check([bool]$Condition, [string]$Message) { $script:checks++; if (!$Condition) { throw $Message } }
function WriteText([string]$Path, [string]$Text) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

$run = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('launcher-entry-points-' + [Guid]::NewGuid().ToString('N'))
$original = Join-Path $run 'Original extraction'
$unrelated = Join-Path $run ('Unrelated working folder ' + [char]0x5DE5 + [char]0x4F5C)
[void][IO.Directory]::CreateDirectory($unrelated)
Write-LauncherZipEntryPoints -OutputDirectory $original -PackageName $PackageName
$payload = Join-Path $original $PackageName
[void][IO.Directory]::CreateDirectory($payload)
# Never copy or execute the real installer. This stand-in accepts only harmless
# diagnostic switches, records its invocation, and exits with a fixture value.
$fakeInstaller = @'
[CmdletBinding()]
param([switch]$DiscoveryOnly, [switch]$RenderDetails, [int]$RenderDpi, [string]$RenderDirectory)
$ErrorActionPreference = 'Stop'
if (!$DiscoveryOnly -or !$RenderDetails -or $RenderDpi -ne 144 -or !$RenderDirectory) {
    [Console]::Error.WriteLine('Fixture diagnostic arguments were not forwarded correctly.')
    exit 93
}
$capture = [ordered]@{
    Script = $PSCommandPath; Payload = $PSScriptRoot
    WorkingDirectory = (Get-Location).ProviderPath
    DiscoveryOnly = [bool]$DiscoveryOnly; RenderDetails = [bool]$RenderDetails
    RenderDpi = $RenderDpi; RenderDirectory = $RenderDirectory
    PowerShellMajor = $PSVersionTable.PSVersion.Major
    Apartment = [Threading.Thread]::CurrentThread.GetApartmentState().ToString()
}
$json = $capture | ConvertTo-Json -Depth 4
[IO.File]::WriteAllText((Join-Path $RenderDirectory 'capture.json'), $json, [Text.UTF8Encoding]::new($false))
[Console]::Out.WriteLine('Reached the isolated nested installer fixture.')
exit [int]([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'fixture-exit-code.txt')))
'@
WriteText (Join-Path $payload 'Install.ps1') $fakeInstaller
WriteText (Join-Path $payload 'fixture-exit-code.txt') '37'
WriteText (Join-Path $unrelated 'Install.ps1') "throw 'The wrapper incorrectly used the current working directory.'"

function Invoke-EntryPoint([string]$Directory, [string]$Case, [switch]$DelayedExpansion, [switch]$NoArguments) {
    $captureDirectory = Join-Path $run ('Captures ' + $Case + ' %EntryPair% !EntryBang!')
    [void][IO.Directory]::CreateDirectory($captureDirectory)
    $wrapper = Join-Path $Directory 'Install.cmd'
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $env:ComSpec
    $start.WorkingDirectory = $unrelated
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $mode = if ($DelayedExpansion) { 'on' } else { 'off' }
    $start.Arguments = '/d /v:' + $mode + ' /s /c ""%LauncherEntryFixtureWrapper%" -DiscoveryOnly -RenderDetails -RenderDpi 144 -RenderDirectory "%LauncherEntryFixtureCapture%""'
    if ($NoArguments) { $start.Arguments = '/d /v:' + $mode + ' /s /c ""%LauncherEntryFixtureWrapper%""' }
    # Expanding one variable avoids CMD interpreting percent pairs in a literal
    # invocation path. Escape ! only at the caller boundary when /V is enabled;
    # the wrapper still receives literal ! characters and must preserve them.
    $start.EnvironmentVariables['LauncherEntryFixtureWrapper'] = $(if ($DelayedExpansion) { $wrapper.Replace('!', '^!') } else { $wrapper })
    $start.EnvironmentVariables['LauncherEntryFixtureCapture'] = $(if ($DelayedExpansion) { $captureDirectory.Replace('!', '^!') } else { $captureDirectory })
    $start.EnvironmentVariables['EntryPair'] = 'UNEXPECTED PERCENT EXPANSION'
    $start.EnvironmentVariables['EntryBang'] = 'UNEXPECTED DELAYED EXPANSION'
    $start.EnvironmentVariables['PSModulePath'] = 'Deliberately invalid inherited module path'
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        Check ($process.Start()) 'CMD could not start the isolated entry-point fixture.'
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(30000)) {
            # This is the test's own CMD process, never an installed application.
            $process.Kill()
            throw 'The isolated wrapper fixture did not finish within 30 seconds.'
        }
        $result = [pscustomobject]@{ ExitCode = $process.ExitCode; Output = $stdout.Result + $stderr.Result;
            CaptureDirectory = $captureDirectory; CapturePath = Join-Path $captureDirectory 'capture.json' }
    } finally { $process.Dispose() }
    return $result
}

function Check-Capture($Result, [string]$Directory, [int]$ExpectedExit) {
    Check ($Result.ExitCode -eq $ExpectedExit) ("Wrapper lost the nested installer exit status. Exit=$($Result.ExitCode); output=$($Result.Output)")
    Check ([IO.File]::Exists($Result.CapturePath)) ("The wrapper did not reach its isolated nested script: $($Result.Output)")
    $capture = [IO.File]::ReadAllText($Result.CapturePath) | ConvertFrom-Json
    Check ($capture.Script -ceq (Join-Path (Join-Path $Directory $PackageName) 'Install.ps1')) 'The wrapper selected a script outside its versioned payload.'
    Check ($capture.Payload -ceq (Join-Path $Directory $PackageName)) 'The nested script did not see the correct moved payload directory.'
    Check ($capture.WorkingDirectory -ceq $unrelated) 'The wrapper changed the caller working directory.'
    Check ($capture.DiscoveryOnly -and $capture.RenderDetails -and $capture.RenderDpi -eq 144) 'Diagnostic switches were lost or changed.'
    Check ($capture.RenderDirectory -ceq $Result.CaptureDirectory) 'Quoted diagnostic arguments lost spaces, percent pairs, or exclamation marks.'
    Check ($capture.PowerShellMajor -eq 5 -and $capture.Apartment -ceq 'STA') 'The wrapper did not use Windows PowerShell in STA mode.'
}

$top = @(Get-ChildItem -LiteralPath $original -Force | ForEach-Object Name | Sort-Object)
Check (($top -join '|') -ceq ((@('Install.cmd', 'Portable.txt', $PackageName) | Sort-Object) -join '|')) 'Generated extraction root does not contain exactly its two entry points and payload folder.'
$portable = [IO.File]::ReadAllText((Join-Path $original 'Portable.txt'))
Check ($portable.Contains($PackageName) -and $portable.Contains('Launcher.default.toml') -and $portable.Contains('Launcher.toml')) 'Portable instructions lost the actual payload or profile guidance.'
$first = Invoke-EntryPoint $original 'original'
Check-Capture $first $original 37

$moved = Join-Path $run ('Moved download ' + [char]0x5DE5 + [char]0x5177 + ' %EntryPair% !EntryBang!')
foreach ($path in @($original, $moved)) {
    Check ([IO.Path]::GetFullPath($path).StartsWith($run + '\', [StringComparison]::OrdinalIgnoreCase)) 'Fixture move escaped its dedicated test directory.'
}
Move-Item -LiteralPath $original -Destination $moved -ErrorAction Stop
$second = Invoke-EntryPoint $moved 'moved'
Check-Capture $second $moved 37
WriteText (Join-Path (Join-Path $moved $PackageName) 'fixture-exit-code.txt') '19'
$delayed = Invoke-EntryPoint $moved 'delayed' -DelayedExpansion
Check-Capture $delayed $moved 19

$nested = Join-Path (Join-Path $moved $PackageName) 'Install.ps1'
if ($SetupExecutable) {
    Copy-Item -LiteralPath $SetupExecutable -Destination (Join-Path (Split-Path -Parent $nested) 'Launcher Setup.exe')
    [IO.File]::Move($nested, ($nested + '.diagnostic'))
    WriteText $nested 'exit 38'
    # Exit 38 shows both the manual EXE and the wrapper waited for the script.
    $native = Invoke-EntryPoint $moved 'native' -NoArguments -DelayedExpansion
    Check ($native.ExitCode -eq 38) "The ZIP wrapper did not wait for native Setup. Exit=$($native.ExitCode); $($native.Output)"
    [IO.File]::Delete($nested)
    [IO.File]::Move(($nested + '.diagnostic'), $nested)
}
Move-Item -LiteralPath $nested -Destination ($nested + '.missing') -ErrorAction Stop
$missing = Invoke-EntryPoint $moved 'missing'
Check ($missing.ExitCode -eq 2) 'A missing payload did not return the documented failure status.'
Check (![IO.File]::Exists($missing.CapturePath)) 'Missing-payload handling ran an unrelated script.'
Check ($missing.Output -match '(?i)missing' -and $missing.Output -match '(?i)extract.*complete.*ZIP' -and $missing.Output.Contains($PackageName)) 'Missing-payload output did not explain which folder to restore and how to extract it.'
$missingFolder = Join-Path $moved $PackageName
$hiddenFolder = $missingFolder + '.absent'
foreach ($path in @($missingFolder, $hiddenFolder)) {
    Check ([IO.Path]::GetFullPath($path).StartsWith($run + '\', [StringComparison]::OrdinalIgnoreCase)) 'Missing-payload fixture move escaped its dedicated test directory.'
}
Move-Item -LiteralPath $missingFolder -Destination $hiddenFolder -ErrorAction Stop
$absent = Invoke-EntryPoint $moved 'absent-folder'
Check ($absent.ExitCode -eq 2 -and ![IO.File]::Exists($absent.CapturePath)) 'An absent payload folder ran another script or lost its failure status.'
Check ($absent.Output -match '(?i)extract.*complete.*ZIP') 'An absent payload folder did not explain recovery.'
Write-Output "Package entry-point checks passed: $script:checks; isolated fake installer only. Fixtures: $run"
