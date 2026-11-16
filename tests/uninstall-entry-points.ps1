#requires -Version 5.1
[CmdletBinding()]
param([string]$Wrapper, [string]$InstallerDirectory, [string]$SetupExecutable,
    [string]$OutputDirectory = [IO.Path]::GetTempPath())
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$InstallerDirectory) { $InstallerDirectory = Join-Path $PSScriptRoot '..\tools\installer' }
if (!$Wrapper) { $Wrapper = Join-Path $InstallerDirectory 'Uninstall.cmd' }
. (Join-Path $InstallerDirectory 'Registration.ps1')
$script:checks = 0
function Check([bool]$Condition, [string]$Message) { $script:checks++; if (!$Condition) { throw $Message } }
function WriteText([string]$Path, [string]$Text) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}
$run = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('launcher-uninstall-entry-' + [Guid]::NewGuid().ToString('N'))
$install = Join-Path $run ('Own install ' + [char]0x5DE5 + [char]0x5177 + ' %EntryPair% !EntryBang!')
$unrelated = Join-Path $run 'Unrelated working folder'
[void][IO.Directory]::CreateDirectory($unrelated)
$wrapperPath = Join-Path $install 'Uninstall.cmd'
[void][IO.Directory]::CreateDirectory($install)
[IO.File]::Copy([IO.Path]::GetFullPath($Wrapper), $wrapperPath)
# Only this capture script ever runs through either customer entry point. It
# neither loads the real maintenance code nor reads/writes an uninstall key.
$captureScript = @'
[CmdletBinding()]
param([string]$Action, [string]$InstallDirectory, [string]$Scope, [switch]$Interactive, [int]$SetupProcessId)
$ErrorActionPreference = 'Stop'
if ($SetupProcessId) {
    $setup = Get-Process -Id $SetupProcessId -ErrorAction SilentlyContinue
    if ($setup -and !$setup.WaitForExit(10000)) { throw 'Setup remained active during removal.' }
    [IO.File]::Delete((Join-Path $PSScriptRoot 'Launcher Setup.exe'))
}
$capture = [ordered]@{
    Script = $PSCommandPath; Action = $Action; InstallDirectory = $InstallDirectory
    Scope = $Scope; Interactive = [bool]$Interactive
    PowerShellMajor = $PSVersionTable.PSVersion.Major
    Apartment = [Threading.Thread]::CurrentThread.GetApartmentState().ToString()
    WorkingDirectory = (Get-Location).ProviderPath
    ModulePath = $env:PSModulePath
}
[IO.File]::WriteAllText($env:LauncherUninstallCapture, ($capture | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
if ($env:LauncherUninstallRemoveWrapper -eq '1') { [IO.File]::Delete((Join-Path $PSScriptRoot 'Uninstall.cmd')) }
exit [int]$env:LauncherUninstallExitCode
'@
WriteText (Join-Path $install 'Maintenance.ps1') $captureScript
WriteText (Join-Path $unrelated 'Maintenance.ps1') "throw 'The uninstall entry point used the working directory.'"

function Invoke-CaptureProcess($Start, [string]$Case, [int]$ExpectedExit = 37, [switch]$RemoveWrapper) {
    $capture = Join-Path $run ($Case + '.json')
    $Start.UseShellExecute = $false
    $Start.CreateNoWindow = $true
    $Start.WorkingDirectory = $unrelated
    $Start.RedirectStandardInput = $true
    $Start.RedirectStandardOutput = $true
    $Start.RedirectStandardError = $true
    $Start.EnvironmentVariables['LauncherUninstallCapture'] = $capture
    $Start.EnvironmentVariables['LauncherUninstallExitCode'] = [string]$ExpectedExit
    $Start.EnvironmentVariables['LauncherUninstallRemoveWrapper'] = $(if ($RemoveWrapper) { '1' } else { '0' })
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $Start
    try {
        Check ($process.Start()) 'Could not start the isolated uninstall entry point.'
        $process.StandardInput.Close() # Missing-bootstrap PAUSE must see EOF.
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(15000)) {
            $process.Kill() # Only the test-owned CMD or fake maintenance host.
            throw 'The isolated uninstall entry point did not return.'
        }
        [pscustomobject]@{ ExitCode = $process.ExitCode; Capture = $capture; Output = $stdout.Result + $stderr.Result }
    } finally { $process.Dispose() }
}
function Invoke-Wrapper([string]$Case, [string]$Directory = '', [string]$Scope = '', [switch]$DelayedExpansion,
    [int]$ExpectedExit = 37, [switch]$RemoveWrapper) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $env:ComSpec
    $mode = if ($DelayedExpansion) { 'on' } else { 'off' }
    $arguments = '"%LauncherUninstallWrapper%"'
    if ($Directory) { $arguments += ' "%LauncherUninstallDirectory%"' }
    if ($Scope) { $arguments += ' ' + $Scope }
    $start.Arguments = '/d /v:' + $mode + ' /s /c "' + $arguments + '"'
    $start.EnvironmentVariables['LauncherUninstallWrapper'] = $(if ($DelayedExpansion) { $wrapperPath.Replace('!', '^!') } else { $wrapperPath })
    $start.EnvironmentVariables['LauncherUninstallDirectory'] = $(if ($DelayedExpansion) { $Directory.Replace('!', '^!') } else { $Directory })
    $start.EnvironmentVariables['EntryPair'] = 'UNEXPECTED PERCENT EXPANSION'
    $start.EnvironmentVariables['EntryBang'] = 'UNEXPECTED DELAYED EXPANSION'
    $start.EnvironmentVariables['PSModulePath'] = 'Deliberately invalid inherited module path'
    Invoke-CaptureProcess $start $Case $ExpectedExit -RemoveWrapper:$RemoveWrapper
}
function Check-Capture($Result, [string]$Directory, [string]$Scope = '', [int]$ExpectedExit = 37) {
    Check ($Result.ExitCode -eq $ExpectedExit) ("Uninstall lost the maintenance exit status: $($Result.ExitCode). $($Result.Output)")
    Check ([IO.File]::Exists($Result.Capture)) ('The harmless maintenance capture did not run: ' + $Result.Output)
    $capture = [IO.File]::ReadAllText($Result.Capture) | ConvertFrom-Json
    Check ($capture.Script -ceq (Join-Path $install 'Maintenance.ps1')) 'The uninstall entry point selected an unrelated maintenance script.'
    Check ($capture.InstallDirectory -ceq $Directory) 'The uninstall destination lost its exact Unicode, percent, exclamation or directory identity.'
    Check ($capture.Action -ceq 'Uninstall' -and $capture.Interactive) 'The customer entry point did not request visible uninstall feedback.'
    Check ($capture.Scope -ceq $Scope) 'The uninstall scope was lost or changed.'
    Check ($capture.PowerShellMajor -eq 5 -and $capture.Apartment -ceq 'STA') 'The uninstall did not run in Windows PowerShell STA mode.'
    Check ($capture.WorkingDirectory -ceq $unrelated) 'Uninstall unexpectedly changed the caller working directory.'
}

Check-Capture (Invoke-Wrapper 'double-click') $install
Check-Capture (Invoke-Wrapper 'double-click-delayed' -DelayedExpansion) $install
$explicit = Join-Path $run ('Explicit install ' + [char]0x5DE5 + ' %EntryPair% !EntryBang!')
[void][IO.Directory]::CreateDirectory($explicit)
Check-Capture (Invoke-Wrapper 'explicit-directory' ($explicit + '\') 'CurrentUser') $explicit 'CurrentUser'
Check-Capture (Invoke-Wrapper 'explicit-directory-delayed' $explicit 'AllUsers' -DelayedExpansion) $explicit 'AllUsers'

# Execute the exact registration command as a native process, avoiding CMD so
# literal percent names cannot become environment expansions. Both scopes are
# merely captured; this test never creates or removes native registration.
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
foreach ($scope in @('CurrentUser', 'AllUsers')) {
    $plan = Get-LauncherRegistrationPlan -Directory $install -Scope $scope -UserSid $sid -Version '1.1.0.9'
    Check ($plan.Values.UninstallString -match '^"([^"]+)"\s+(.+)$') 'The registered uninstall command has no quoted executable.'
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Matches[1]
    $start.Arguments = $Matches[2]
    # Windows Settings does not supply a PowerShell 7 module environment.
    $start.EnvironmentVariables.Remove('PSModulePath')
    Check-Capture (Invoke-CaptureProcess $start ('registered-' + $scope)) $install $scope
}

if ($SetupExecutable) {
    $setup = Join-Path $install 'Launcher Setup.exe'
    Copy-Item -LiteralPath $SetupExecutable -Destination $setup
    $plan = Get-LauncherRegistrationPlan -Directory $install -Scope CurrentUser -UserSid $sid -Version '1.1.0.9'
    Check ($plan.Values.UninstallString -ceq ('"{0}" --uninstall CurrentUser' -f $setup)) 'Windows uninstall did not select the installed Setup executable.'
    $start = [Diagnostics.ProcessStartInfo]::new($setup, '--uninstall CurrentUser')
    $result = Invoke-CaptureProcess $start 'native-uninstall' -ExpectedExit 0
    for ($i = 0; $i -lt 100 -and ![IO.File]::Exists($result.Capture); ++$i) { Start-Sleep -Milliseconds 100 }
    Check-Capture $result $install 'CurrentUser' 0
    Check (![IO.File]::Exists($setup)) 'Setup did not release its executable for removal.'
}

Check-Capture (Invoke-Wrapper 'self-removing-wrapper' -ExpectedExit 0 -RemoveWrapper) $install '' 0
Check (![IO.File]::Exists($wrapperPath)) 'The harmless successful-removal fixture did not remove its own wrapper.'
[IO.File]::Copy([IO.Path]::GetFullPath($Wrapper), $wrapperPath)
[IO.File]::Move((Join-Path $install 'Maintenance.ps1'), (Join-Path $install 'Maintenance.ps1.absent'))
$missing = Invoke-Wrapper 'missing-maintenance' -ExpectedExit 2
Check ($missing.ExitCode -eq 2 -and ![IO.File]::Exists($missing.Capture)) 'A missing maintenance script ran another file or lost its failure status.'
Check ($missing.Output -match '(?i)maintenance.*missing' -and $missing.Output -match '(?i)repair') 'Missing-maintenance output did not explain the unavailable helper and recovery.'
Write-Output "Uninstall entry points passed: $script:checks checks; only harmless capture scripts ran. Fixtures: $run"
