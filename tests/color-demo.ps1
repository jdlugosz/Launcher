#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Wrapper,
    [string]$OutputDirectory = [IO.Path]::GetTempPath()
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$Wrapper) { $Wrapper = Join-Path $PSScriptRoot '..\examples\demo_many_colors.cmd' }
$script:checks = 0
function Check([bool]$Condition, [string]$Message) { $script:checks++; if (!$Condition) { throw $Message } }
$run = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('launcher-color-demo-' + [Guid]::NewGuid().ToString('N'))
$unrelated = Join-Path $run 'Unrelated working folder'
[void][IO.Directory]::CreateDirectory($unrelated)
# This fixture is deliberately a windowless stand-in, never the real Launcher.
# Atomic capture publication lets the wrapper keep its normal asynchronous START.
$source = @'
using System;
using System.IO;
using System.Reflection;
using System.Text;
static class CaptureDemo {
    static int Main(string[] args) {
        string capture = Environment.GetEnvironmentVariable("LauncherDemoCapture");
        string[] lines = new string[args.Length + 3];
        lines[0] = Path.GetFullPath(Assembly.GetExecutingAssembly().Location);
        lines[1] = args.Length.ToString();
        Array.Copy(args, 0, lines, 2, args.Length);
        lines[lines.Length - 1] = Environment.CurrentDirectory;
        File.WriteAllLines(capture + ".new", lines, new UTF8Encoding(false));
        File.Move(capture + ".new", capture);
        return 0;
    }
}
'@
$sourcePath = Join-Path $run 'capture.cs'
$fixtureExe = Join-Path $run 'CaptureDemo.exe'
[IO.File]::WriteAllText($sourcePath, $source, [Text.UTF8Encoding]::new($false))
$compiler = Join-Path $env:SystemRoot 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
& $compiler /nologo /target:winexe ("/out:" + $fixtureExe) $sourcePath
if ($LASTEXITCODE) { throw 'Cannot compile the isolated demo capture fixture.' }
function New-Case([string]$Name, [string[]]$Executables) {
    $directory = Join-Path $run ($Name + ' spaces ' + [char]0x5DE5 + [char]0x5177)
    [void][IO.Directory]::CreateDirectory((Join-Path $directory 'examples'))
    [IO.File]::Copy([IO.Path]::GetFullPath($Wrapper), (Join-Path $directory 'examples\demo_many_colors.cmd'))
    foreach ($name in $Executables) { [IO.File]::Copy($fixtureExe, (Join-Path $directory $name)) }
    # A decoy in the working directory must never be selected.
    return $directory
}
function Invoke-Demo([string]$Directory, [string]$Case, [switch]$ExpectCapture) {
    $capture = Join-Path $run ($Case + '.txt')
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $env:ComSpec
    $start.WorkingDirectory = $unrelated
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.Arguments = '/d /v:off /s /c ""%LauncherDemoWrapper%""'
    $start.EnvironmentVariables['LauncherDemoWrapper'] = Join-Path $Directory 'examples\demo_many_colors.cmd'
    $start.EnvironmentVariables['LauncherDemoCapture'] = $capture
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        Check ($process.Start()) 'Could not start the isolated demo wrapper.'
        $process.StandardInput.Close() # PAUSE on expected failures must see EOF.
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(10000)) {
            $process.Kill() # Only this test-owned CMD process.
            throw 'The isolated demo wrapper did not return.'
        }
        $result = [pscustomobject]@{ ExitCode = $process.ExitCode; Output = $stdout.Result + $stderr.Result; Capture = $capture }
    } finally { $process.Dispose() }
    if ($ExpectCapture) {
        $timer = [Diagnostics.Stopwatch]::StartNew()
        while (![IO.File]::Exists($capture) -and $timer.ElapsedMilliseconds -lt 10000) { Start-Sleep -Milliseconds 20 }
        Check ([IO.File]::Exists($capture)) ("The isolated launcher did not capture its arguments: " + $result.Output)
    }
    return $result
}
function Check-Capture($Result, [string]$Executable) {
    Check ($Result.ExitCode -eq 0) ('Demo launch failed: ' + $Result.Output)
    $lines = [IO.File]::ReadAllLines($Result.Capture)
    Check ($lines.Count -eq 4) 'The demo supplied unexpected additional arguments.'
    Check ($lines[0] -ceq $Executable) 'The demo selected the wrong executable.'
    Check ($lines[1] -ceq '1' -and $lines[2] -ceq '--color-demo') 'The demo bypassed native writable-example selection.'
    Check ($lines[3] -ceq $unrelated) 'The demo unexpectedly changed the caller working directory.'
}
[IO.File]::Copy($fixtureExe, (Join-Path $unrelated 'Launcher.exe'))
$alias = New-Case 'Alias preferred' @('Launcher.exe', 'Launcher-R1.1.0.4.exe', 'Launcher-R1.1.0.5.exe')
Check-Capture (Invoke-Demo $alias 'alias' -ExpectCapture) (Join-Path $alias 'Launcher.exe')
$portable = New-Case 'Portable single version' @('Launcher-R1.1.0.9.exe')
Check-Capture (Invoke-Demo $portable 'portable' -ExpectCapture) (Join-Path $portable 'Launcher-R1.1.0.9.exe')
$ambiguous = New-Case 'Ambiguous versions' @('Launcher-R1.1.0.4.exe', 'Launcher-R1.1.0.5.exe')
$ambiguousResult = Invoke-Demo $ambiguous 'ambiguous'
Check ($ambiguousResult.ExitCode -eq 1 -and $ambiguousResult.Output.Contains('More than one versioned Launcher')) 'Ambiguous versions did not abort with a useful message.'
Check (![IO.File]::Exists($ambiguousResult.Capture)) 'An ambiguous version was launched.'
$missing = New-Case 'Missing executable' @()
$missingResult = Invoke-Demo $missing 'missing'
Check ($missingResult.ExitCode -eq 1 -and $missingResult.Output.Contains('not found')) 'A missing executable did not abort with a useful message.'
Check (![IO.File]::Exists($missingResult.Capture)) 'The demo selected an executable outside its payload.'
Write-Output "Color demo wrapper passed: $script:checks checks; only isolated capture executables ran. Fixtures: $run"
