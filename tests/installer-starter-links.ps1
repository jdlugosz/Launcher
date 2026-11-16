#requires -Version 5.1
[CmdletBinding()]
param([string]$ProfileScript, [string]$OutputDirectory = [IO.Path]::GetTempPath())
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$ProfileScript) { $ProfileScript = Join-Path $PSScriptRoot '..\tools\installer\PortableProfile.ps1' }
. $ProfileScript
$script:checks = 0
function Check([bool]$Condition, [string]$Message) { $script:checks++; if (!$Condition) { throw $Message } }
function Text([string]$Path, [string]$Content) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
    [IO.File]::WriteAllText($Path, $Content, [Text.UTF8Encoding]::new($false))
}
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Reject([scriptblock]$Action, [string]$Message) {
    $failed = $false; try { & $Action | Out-Null } catch { $failed = $true }
    Check $failed $Message
}
function NoStage([string]$Directory) {
    Check (@(Get-ChildItem -LiteralPath $Directory -Filter '.launcher-starter-links-*' -Force).Count -eq 0) 'Starter-link update left temporary staging files.'
}
$run = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('launcher-starter-links-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($run)
$legacy = @'
schema_version = 1
# Preserve this comment and the user's other settings.
[[tabs]]
name = "Launcher"
items = [
    { label = "Launcher guide", icon = { stock = "information" }, target = "notepad.exe", arguments = '"$EXE\docs\Launcher.md"' }, # Keep this note.
    { label = "Setup and maintenance guide", icon = { stock = "document" }, target = "notepad.exe", arguments = '"$EXE\docs\Installer.md"' },
    { label = "Try the many-colors demo", icon = { stock = "image" }, target = '$EXE\examples\demo_many_colors.cmd' },
    { label = "Read the color demo configuration", icon = { stock = "document" }, target = "notepad.exe", arguments = '"$EXE\examples\demo_many_colors.toml"' },
    { label = "Launcher Drop guide", icon = { stock = "document" }, target = "notepad.exe", arguments = '"$EXE\docs\LauncherDrop.md"' },
    { label = "SVG Extractor guide", icon = { stock = "document" }, target = "notepad.exe", arguments = '"$EXE\docs\SvgExtractor.md"' },
    { label = "My tool", target = 'C:\My tools\run.exe', arguments = '--keep-this' },
]
%STOP unchanged examples
    { label = "Launcher guide", icon = { stock = "information" }, target = "notepad.exe", arguments = '"$EXE\docs\Launcher.md"' },
'@
$legacy = ([string][char]0xFEFF) + $legacy.Replace("`n", "`r`n") + "`r`n# " + [char]0x4E2D + [char]0x6587 + "`r`n"
$converted = ConvertTo-LauncherInstalledStarterText -Text $legacy
$demo = '"$USERDATA\Demos\demo_many_colors.toml"'
foreach ($name in @('Launcher', 'Installer', 'LauncherDrop', 'SvgExtractor')) {
    Check ($converted.Contains("target = '" + '$EXE\docs\' + $name + ".html'")) "The old stock $name guide did not become direct HTML."
}
Check ($converted.Contains("target = '" + '$SELF' + "', arguments = '" + $demo + "'")) 'The installed demo did not use the running executable with its quoted AppData configuration.'
Check ($converted.Contains('target = "notepad.exe", arguments = ' + [char]39 + $demo + [char]39)) 'Read-demo did not point at the editable AppData working copy.'
Check ($converted.Contains("# Keep this note.`r`n") -and $converted.StartsWith([string][char]0xFEFF)) 'Migration changed the BOM or trailing custom comment.'
Check (($converted.Substring($converted.IndexOf('%STOP'))) -ceq ($legacy.Substring($legacy.IndexOf('%STOP')))) 'Migration changed the stopped example suffix.'
Check ($converted.Contains("{ label = `"My tool`", target = 'C:\My tools\run.exe', arguments = '--keep-this' },")) 'Migration changed an unrelated personal entry.'
Check ([regex]::Matches($converted, "(?<!`r)`n").Count -eq 0) 'Migration changed CRLF line endings.'
Check ((ConvertTo-LauncherInstalledStarterText -Text $converted) -ceq $converted) 'The pure migration is not idempotent.'
$previousDemo = @'
    { label = "Try the many-colors demo", icon = { stock = "image" }, target = '$SELF', arguments = '"$USERDATA\Demos\ManyColors\Launcher.toml"' },
    { label = "Read the color demo configuration", icon = { stock = "document" }, target = "notepad.exe", arguments = '"$USERDATA\Demos\ManyColors\Launcher.toml"' },
'@
$currentDemo = ConvertTo-LauncherInstalledStarterText -Text $previousDemo
Check ($currentDemo.Contains('$USERDATA\Icons\many-colors.svg') -and $currentDemo.Contains('index = 62') -and
    !$currentDemo.Contains('ManyColors\Launcher.toml')) 'An R1.1.0.16 stock demo did not receive the new icon and flat filename.'

$guide = '    { label = "Launcher guide", icon = { stock = "information" }, target = "notepad.exe", arguments = ' + [char]39 + '"$EXE\docs\Launcher.md"' + [char]39 + ' },'
$basic = 'description = """' + "`n" + $guide + "`n" + '"""' + "`n"
$literal = 'description = ' + ([string][char]39 * 3) + "`n" + $guide + "`n" + ([string][char]39 * 3) + "`n"
foreach ($quoted in @($basic, $literal)) {
    $result = ConvertTo-LauncherInstalledStarterText -Text ($quoted + $guide)
    Check ($result.StartsWith($quoted, [StringComparison]::Ordinal)) 'A stock-looking record inside a multiline string was modified.'
    Check ($result.EndsWith("target = '" + '$EXE\docs\Launcher.html' + "' },", [StringComparison]::Ordinal)) 'A real stock record after a multiline string was skipped.'
}
foreach ($custom in @($guide.Replace('Launcher guide', 'My guide'), $guide.Replace('notepad.exe', 'notepad++.exe'),
    $guide.Replace('label =', 'label='), $guide.Replace('information', 'help'), ('# ' + $guide))) {
    Check ((ConvertTo-LauncherInstalledStarterText -Text $custom) -ceq $custom) 'A customized, differently formatted, or commented record was changed.'
}

$directory = Join-Path $run 'Existing personal profile'
$path = Join-Path $directory 'Launcher.toml'
$runtime = Join-Path $directory 'Launcher.runtime.toml'
Text $path $legacy
Text $runtime "[dock]`nposition=0.31`n"
$before = Hash $path; $runtimeHash = Hash $runtime
$summary = Update-LauncherInstalledStarterLinks -UserDataDirectory $directory
$backups = @(Get-ChildItem -LiteralPath (Join-Path $directory 'Backups') -Filter 'Launcher.before-setup-*.toml' -File)
Check ($summary.Contains('Updated stock') -and $backups.Count -eq 1) 'Live migration did not report the exact original backup.'
Check ((Hash $backups[0].FullName) -ceq $before) 'The migration backup differs from the original bytes.'
Check ([IO.File]::ReadAllText($path, [Text.UTF8Encoding]::new($false, $true)).TrimStart([char]0xFEFF) -ceq $converted.TrimStart([char]0xFEFF)) 'The migrated personal configuration differs from the pure transform.'
Check ((Hash $runtime) -ceq $runtimeHash) 'Migration altered saved runtime settings.'
NoStage $directory
$timestamp = [IO.File]::GetLastWriteTimeUtc($path)
$summary = Update-LauncherInstalledStarterLinks -UserDataDirectory $directory
Check ($summary.Contains('no stock links') -and [IO.File]::GetLastWriteTimeUtc($path) -eq $timestamp) 'A repeat migration rewrote the configuration.'
Check (@(Get-ChildItem -LiteralPath (Join-Path $directory 'Backups') -Filter 'Launcher.before-setup-*.toml').Count -eq 1) 'A repeat migration created an unnecessary backup.'

foreach ($failure in @('concurrent', 'callback', 'locked', 'readonly', 'invalid-utf8')) {
    $failedDirectory = Join-Path $run $failure; $failedPath = Join-Path $failedDirectory 'Launcher.toml'
    Text $failedPath $legacy
    $originalHash = Hash $failedPath; $lock = $null
    try {
        if ($failure -eq 'concurrent') {
            Reject { Update-LauncherInstalledStarterLinks -UserDataDirectory $failedDirectory -BeforeCommit { param($File) [IO.File]::WriteAllText($File, 'Concurrent edit must survive.') } } 'A concurrent edit was overwritten.'
            Check ([IO.File]::ReadAllText($failedPath) -ceq 'Concurrent edit must survive.') 'Concurrency handling discarded the newer file.'
        } elseif ($failure -eq 'callback') {
            Reject { Update-LauncherInstalledStarterLinks -UserDataDirectory $failedDirectory -BeforeCommit { throw 'Owned fixture failure.' } } 'A preparation failure was ignored.'
            Check ((Hash $failedPath) -ceq $originalHash) 'A preparation failure changed the configuration.'
        } elseif ($failure -eq 'locked') {
            $lock = [IO.File]::Open($failedPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
            Reject { Update-LauncherInstalledStarterLinks -UserDataDirectory $failedDirectory } 'A sharing violation was ignored.'
        } elseif ($failure -eq 'readonly') {
            [IO.File]::SetAttributes($failedPath, [IO.FileAttributes]::ReadOnly)
            Reject { Update-LauncherInstalledStarterLinks -UserDataDirectory $failedDirectory } 'A read-only configuration was replaced.'
            Check ((Hash $failedPath) -ceq $originalHash) 'A failed publication changed the read-only configuration.'
        } else {
            [IO.File]::WriteAllBytes($failedPath, [byte[]]@(0xFF, 0xFE, 0xA3))
            $originalHash = Hash $failedPath
            Reject { Update-LauncherInstalledStarterLinks -UserDataDirectory $failedDirectory } 'Invalid encoding was silently rewritten.'
            Check ((Hash $failedPath) -ceq $originalHash) 'Invalid encoding bytes were changed.'
        }
    } finally {
        if ($lock) { $lock.Dispose(); Check ((Hash $failedPath) -ceq $originalHash) 'A locked configuration changed.' }
        [IO.File]::SetAttributes($failedPath, [IO.FileAttributes]::Normal)
    }
    NoStage $failedDirectory
}
$missing = Join-Path $run 'Absent profile'
Check ((Update-LauncherInstalledStarterLinks -UserDataDirectory $missing).Contains('No existing')) 'A missing profile did not return a clear no-op result.'
Check (![IO.Directory]::Exists($missing)) 'The migration created a missing profile.'
$directoryAsFile = Join-Path $run 'Directory instead of TOML\Launcher.toml'
[void][IO.Directory]::CreateDirectory($directoryAsFile)
Reject { Update-LauncherInstalledStarterLinks -UserDataDirectory ([IO.Path]::GetDirectoryName($directoryAsFile)) } 'A directory was accepted as a personal configuration.'
$junctionTarget = Join-Path $run 'Real profile behind junction'
Text (Join-Path $junctionTarget 'Launcher.toml') $legacy
$junction = Join-Path $run 'Profile junction'
New-Item -ItemType Junction -Path $junction -Target $junctionTarget -ErrorAction Stop | Out-Null
$junctionHash = Hash (Join-Path $junctionTarget 'Launcher.toml')
Reject { Update-LauncherInstalledStarterLinks -UserDataDirectory $junction } 'A reparse-point profile directory was accepted.'
Check ((Hash (Join-Path $junctionTarget 'Launcher.toml')) -ceq $junctionHash) 'Rejecting a profile junction changed its target.'

# Both program-owned default templates are excluded from transfer and upgrade
# warnings, on the source and destination sides of the profile inventory.
$package = Join-Path $run 'Templates-only package'; $profileDirectory = Join-Path $run 'Templates-only profile'
foreach ($template in @('Launcher.default.toml', 'Launcher.installed.default.toml')) {
    Text (Join-Path $package $template) ('Package-owned ' + $template)
    Text (Join-Path $profileDirectory $template) ('Different program template ' + $template)
}
$plan = Get-LauncherPortableProfilePlan -PackageDirectory $package -UserDataDirectory $profileDirectory -Action Upgrade
Check ($plan.Files.Count -eq 0 -and $plan.DestinationFiles.Count -eq 0 -and !$plan.Warning) 'A program-owned template was inventoried as personal profile data.'
Write-Output "Installed starter-link checks passed: $script:checks; fixtures: $run"
