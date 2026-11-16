#requires -Version 5.1
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Probe, [string]$ClientSource)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (!$ClientSource) { $ClientSource = Join-Path $PSScriptRoot '..\tools\installer\WindowTextQuery.cs' }
Add-Type -Path $ClientSource -ReferencedAssemblies System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class WindowTextQueryTestWindows {
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr word, IntPtr data);
}
'@
$script:checks = 0
function Check([bool]$Condition, [string]$Message) { $script:checks++; if (!$Condition) { throw $Message } }
function Reject([scriptblock]$Action, [string]$Reason) {
    $message = ''
    try { & $Action | Out-Null } catch { $message = $_.Exception.Message }
    Check ($message -like ('*' + $Reason + '*')) ('Expected rejection containing "' + $Reason + '"; got "' + $message + '".')
}
function Quote-Argument([string]$Value) {
    $quoted = [Text.StringBuilder]::new()
    [void]$quoted.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') { $slashes++; continue }
        if ($character -eq '"') { [void]$quoted.Append(('\' * ($slashes * 2 + 1))) }
        else { [void]$quoted.Append(('\' * $slashes)) }
        $slashes = 0
        [void]$quoted.Append($character)
    }
    [void]$quoted.Append(('\' * ($slashes * 2)))
    [void]$quoted.Append('"')
    $quoted.ToString()
}
$request = 'Dlugosz.WindowTextQuery.Test.' + [Guid]::NewGuid().ToString('N')
$children = [Collections.Generic.List[object]]::new()
function Start-Probe([string]$Mode, [string]$Text = '') {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = [IO.Path]::GetFullPath($Probe)
    $start.Arguments = ((@($request, $Mode, $Text) | ForEach-Object { Quote-Argument $_ }) -join ' ')
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true; $start.RedirectStandardOutput = $true
    $process = [Diagnostics.Process]::Start($start)
    $child = [pscustomobject]@{ Process = $process; Window = [IntPtr]::Zero }
    $children.Add($child)
    $ready = $process.StandardOutput.ReadLineAsync()
    if (!$ready.Wait(5000)) { throw 'The isolated reply probe did not publish its window.' }
    $value = 0L
    if (![long]::TryParse($ready.Result, [ref]$value) -or $value -eq 0) { throw 'The isolated reply probe returned no valid window.' }
    $child.Window = [IntPtr]::new($value)
    $child
}
function Stop-Probe($Child) {
    if (!$Child.Process.HasExited) {
        [void][WindowTextQueryTestWindows]::PostMessage($Child.Window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
        if (!$Child.Process.WaitForExit(5000)) {
            # Only the exact child created by this fixture may be terminated.
            $Child.Process.Kill()
            [void]$Child.Process.WaitForExit(5000)
            throw 'The isolated reply probe did not close normally.'
        }
    }
}
try {
    $text = '--config "C:\profiles\' + [char]0x03b4 + [char]0x03bf + [char]0x03ba + [char]0x03b9 + [char]0x03bc + [char]0x03ae + '.toml" --log "D:\logs\first dock.log" tail\'
    $normal = Start-Probe 'reply' $text
    Check ([Dlugosz.Win32.WindowTextQuery]::Read($normal.Window, $request, 2000) -ceq $text) 'The nested cross-process COPYDATA reply must preserve UTF-16, quotes and backslashes exactly.'
    Check ([Dlugosz.Win32.WindowTextQuery]::Read($normal.Window, $request, 2000) -ceq $text) 'A second query must use a fresh temporary receiver and correlation cookie.'
    Check ([Dlugosz.Win32.WindowTextQuery]::Read($normal.Window, $request, 2000, $normal.Process.Id) -ceq $text) 'A caller may bind the target window to an independently verified process ID.'
    Reject { [Dlugosz.Win32.WindowTextQuery]::Read($normal.Window, $request, 2000, [Diagnostics.Process]::GetCurrentProcess().Id) } 'different process than expected'
    $empty = Start-Probe 'empty'
    Check ([Dlugosz.Win32.WindowTextQuery]::Read($empty.Window, $request, 2000) -ceq '') 'An empty but terminated response is supported text.'
    $maximum = Start-Probe 'max'
    Check ([Dlugosz.Win32.WindowTextQuery]::Read($maximum.Window, $request, 2000) -ceq ('x' * 32766)) 'The maximum 32767-character buffer includes its terminal NUL.'

    $unsupported = Start-Probe 'unsupported'
    Reject { [Dlugosz.Win32.WindowTextQuery]::Read($unsupported.Window, $request, 2000) } 'does not support'
    foreach ($mode in @('no-reply', 'wrong-cookie', 'wrong-source')) {
        $child = Start-Probe $mode
        Reject { [Dlugosz.Win32.WindowTextQuery]::Read($child.Window, $request, 2000) } 'without returning a matching response'
    }
    foreach ($mode in @('malformed', 'no-nul', 'embedded-nul', 'oversized')) {
        $child = Start-Probe $mode
        Reject { [Dlugosz.Win32.WindowTextQuery]::Read($child.Window, $request, 2000) } 'invalid text response'
    }
    $slow = Start-Probe 'delay' 'late response'
    $elapsed = [Diagnostics.Stopwatch]::StartNew()
    Reject { [Dlugosz.Win32.WindowTextQuery]::Read($slow.Window, $request, 100) } 'timeout'
    Check ($elapsed.ElapsedMilliseconds -lt 2000) 'The sender must not hang waiting for an unresponsive target.'
    Stop-Probe $slow
    $destroyed = Start-Probe 'destroy'
    Reject { [Dlugosz.Win32.WindowTextQuery]::Read($destroyed.Window, $request, 2000) } 'destroyed or replaced'
    Stop-Probe $normal
    Reject { [Dlugosz.Win32.WindowTextQuery]::Read($normal.Window, $request, 100) } 'no longer exists'
    Reject { [Dlugosz.Win32.WindowTextQuery]::Read([IntPtr]::Zero, $request, 100) } 'target window is required'
    Reject { [Dlugosz.Win32.WindowTextQuery]::Read($empty.Window, $request, 0) } '1 through 30000'
    Reject { [Dlugosz.Win32.WindowTextQuery]::Read($empty.Window, $request, 30001) } '1 through 30000'
    Reject { [Dlugosz.Win32.WindowTextQuery]::Read($empty.Window, '', 100) } 'registered message name'
    Reject { [Dlugosz.Win32.WindowTextQuery]::Read($empty.Window, ("bad" + [char]0 + "name"), 100) } 'registered message name'
    Write-Output ("Window text query passed {0} checks using isolated native processes." -f $script:checks)
} finally {
    $cleanupFailure = $null
    foreach ($child in $children) {
        try { Stop-Probe $child }
        catch { if ($null -eq $cleanupFailure) { $cleanupFailure = $_ } }
        finally { $child.Process.Dispose() }
    }
    if ($null -ne $cleanupFailure) { throw $cleanupFailure }
}
