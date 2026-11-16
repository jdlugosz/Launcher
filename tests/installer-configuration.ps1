#requires -Version 5.1
[CmdletBinding()]
param([string]$InstallerDirectory,
    [string]$OutputDirectory = [IO.Path]::GetTempPath())

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$InstallerDirectory) { $InstallerDirectory = Join-Path $PSScriptRoot '..\tools\installer' }
. (Join-Path $InstallerDirectory 'PortableProfile.ps1')

# Exercise the production completion function with real profile operations and
# isolated paths, without opening the wizard or reading any personal profile.
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $InstallerDirectory 'Install.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'The installer could not be parsed.' }
$completion = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Complete-Installation'
}, $true)
if (!$completion) { throw 'The installer completion function is missing.' }
. ([scriptblock]::Create($completion.Extent.Text))

$script:checks = 0
function Check([bool]$Condition, [string]$Message) { $script:checks++; if (!$Condition) { throw $Message } }
function Text([string]$Path, [string]$Value) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
    [IO.File]::WriteAllText($Path, $Value, [Text.UTF8Encoding]::new($false))
}
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Clear-InstallWorker { }
function Show-WizardPage([int]$Page) { $script:completedPage = $Page }
function Test-RecoveryShortcutAvailable($Request) { $Request.Scope -eq 'AllUsers' }
function Set-LauncherStartupOption {
    param($Discovery, [string]$Destination, [bool]$StartOnSignIn)
    Check ($Destination -eq $script:installRequest.DestinationDirectory) 'Startup used another installation.'
}

$run = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('launcher-configuration-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($run)
$script:progressTimer = [pscustomobject]@{}
$script:progressTimer | Add-Member ScriptMethod Stop { }
$script:cancelButton = [pscustomobject]@{ Enabled = $false }
$script:finishLabel = [pscustomobject]@{ Text = '' }
$script:finishDetails = [pscustomobject]@{ Text = ''; Visible = $false }
$script:recoveryHint = [pscustomobject]@{ Visible = $false }
$script:launchCheck = [pscustomobject]@{ Text = ''; Checked = $true }
$script:startupCheck = [pscustomobject]@{ Checked = $false }
$script:restartQueue = [Collections.Generic.List[object]]::new()
$script:reviewedDiscovery = [pscustomobject]@{}
$result = [pscustomobject]@{ StoppedProcessIds = @(); Deployment = $null }

$installedSource = @'
schema_version = 1
[[tabs]]
name = "Launcher"
items = [
    { label = "Launcher guide", icon = { stock = "information" }, target = '$EXE\docs\Launcher.html' },
    { label = "Setup and maintenance guide", icon = { stock = "document" }, target = '$EXE\docs\Installer.html' },
]
[[tabs]]
name = "Examples"
items = [
    { label = "Try the many-colors demo", icon = { stock = "image" }, target = '$SELF', arguments = '"$USERDATA\Demos\demo_many_colors.toml"' },
    { label = "Read the color demo configuration", icon = { stock = "document" }, target = "notepad.exe", arguments = '"$USERDATA\Demos\demo_many_colors.toml"' },
]
'@
$installedSource = ConvertTo-LauncherInstalledStarterText -Text $installedSource
$legacyStockSource = @'
schema_version = 1
# Keep this personal comment and unrelated item.
[[tabs]]
name = "Launcher"
items = [
    { label = "Launcher guide", icon = { stock = "information" }, target = "notepad.exe", arguments = '"$EXE\docs\Launcher.md"' },
    { label = "Setup and maintenance guide", icon = { stock = "document" }, target = "notepad.exe", arguments = '"$EXE\docs\Installer.md"' },
    { label = "My own item", target = 'C:\Custom\Utility.exe' },
]
[[tabs]]
name = "Examples"
items = [
    { label = "Try the many-colors demo", icon = { stock = "image" }, target = '$EXE\examples\demo_many_colors.cmd' },
    { label = "Read the color demo configuration", icon = { stock = "document" }, target = "notepad.exe", arguments = '"$EXE\examples\demo_many_colors.toml"' },
]
'@

foreach ($scenario in @('clean', 'portable', 'existing', 'upgrade', 'repeat', 'stock-links', 'failed-demo', 'failed-icon', 'failed')) {
    $case = Join-Path $run $scenario
    $script:packageDirectory = Join-Path $case 'download'
    $install = Join-Path $case 'Program Files\Launcher'
    $script:userDataDirectory = Join-Path $case 'initiating user\AppData\Launcher'
    $template = Join-Path $install 'Launcher.installed.default.toml'
    $demoTemplate = Join-Path $script:packageDirectory 'examples\demo_many_colors.toml'
    $configuration = Join-Path $script:userDataDirectory 'Launcher.toml'
    $demo = Join-Path $script:userDataDirectory 'Demos\demo_many_colors.toml'
    $demoRuntime = Join-Path $script:userDataDirectory 'Demos\demo_many_colors.runtime.toml'
    $icon = Join-Path $script:userDataDirectory 'Icons\many-colors.svg'
    Text (Join-Path $script:packageDirectory 'Launcher.default.toml') 'download template must not seed the installed profile'
    Text (Join-Path $script:packageDirectory 'Launcher.installed.default.toml') 'download installed-template decoy must not seed the profile'
    Text (Join-Path $install 'examples\demo_many_colors.toml') 'installed demo decoy must not seed the working copy'
    Text (Join-Path $script:packageDirectory 'resources\many-colors.svg') '<svg/>'
    Text (Join-Path $install 'Launcher.default.toml') 'portable template must not seed the installed profile'
    Text $template $installedSource
    Text $demoTemplate "schema_version = 1`n# installed demo for $scenario`n[[tabs]]`nname='Many colors'`n"
    if ($scenario -in @('portable', 'upgrade', 'repeat')) { Text (Join-Path $script:packageDirectory 'Launcher.toml') 'edited portable configuration' }
    if ($scenario -eq 'existing') { Text $configuration 'existing user configuration' }
    if ($scenario -eq 'repeat') { Text $configuration 'existing repeated personal configuration' }
    if ($scenario -eq 'repeat') { Text $icon '<svg><!-- My edited demo icon --></svg>' }
    if ($scenario -eq 'stock-links') { Text $configuration $legacyStockSource }
    if ($scenario -in @('upgrade', 'repeat')) {
        Text $demo '# Preserve my edited demo working copy.'
        Text $demoRuntime "[dock]`nposition=0.42`n"
        $demoHash = Hash $demo
        $demoRuntimeHash = Hash $demoRuntime
    }
    if ($scenario -eq 'failed-demo') { [IO.File]::Delete($demoTemplate) }
    if ($scenario -eq 'failed-icon') { [IO.File]::Delete((Join-Path $script:packageDirectory 'resources\many-colors.svg')) }
    if ($scenario -eq 'failed') { [IO.File]::Delete($template) }
    $script:installRequest = [pscustomobject]@{
        DestinationDirectory = $install; RestartInstances = @()
        QuickRollbackEligible = $false
        Action = if ($scenario -in @('upgrade', 'repeat', 'stock-links')) { 'Upgrade' } else { 'Install' }
        Scope = if ($scenario -in @('clean', 'upgrade')) { 'AllUsers' } else { 'CurrentUser' }
    }
    $script:completedPage = -1
    $script:launchCheck.Checked = $true
    $completed = $result
    if ($scenario -eq 'upgrade') {
        $completed = $result.PSObject.Copy()
        $completed | Add-Member -NotePropertyName RecoveryShortcutWarning -NotePropertyValue 'Fixture shortcut warning.'
    }
    Complete-Installation $completed
    Check ($script:completedPage -eq 5) 'Setup did not finish after profile preparation.'
    Check ($script:recoveryHint.Visible -eq ($script:installRequest.Scope -eq 'AllUsers')) 'Finish did not follow the scoped Recovery shortcut result.'
    if ($scenario -eq 'upgrade') {
        Check ($script:finishLabel.Text.Contains('WARNING: Fixture shortcut warning.')) 'Setup did not display the recovery shortcut warning.'
    }
    if ($scenario -in @('failed', 'failed-demo', 'failed-icon')) {
        Check (!$script:launchCheck.Checked -and $script:finishLabel.Text.Contains('configuration could not be prepared')) 'A preparation failure did not explain the problem and suppress automatic launch.'
        Check (!(Test-Path -LiteralPath $configuration)) 'A missing template left a personal configuration behind.'
        if ($scenario -eq 'failed-demo') { Check (!(Test-Path -LiteralPath $demo)) 'A missing installed demo left a partial working copy.' }
        continue
    }
    Check $script:launchCheck.Checked 'Successful profile preparation unexpectedly disabled launch.'
    Check (Test-Path -LiteralPath $configuration -PathType Leaf) 'Setup finished without creating the initiating user configuration.'
    Check (Test-Path -LiteralPath $demo -PathType Leaf) 'Setup enabled launch before installing the personal demo.'
    Check (Test-Path -LiteralPath $icon -PathType Leaf) 'Setup enabled launch before installing the demo icon.'
    if ($scenario -eq 'repeat') { Check ([IO.File]::ReadAllText($icon).Contains('My edited demo icon')) 'Setup replaced an edited personal icon.' }
    if ($scenario -in @('upgrade', 'repeat')) {
        Check ((Hash $demo) -ceq $demoHash -and (Hash $demoRuntime) -ceq $demoRuntimeHash) 'An upgrade refreshed existing demo edits or saved runtime settings.'
    } else {
        Check ((Hash $demo) -ceq (Hash $demoTemplate)) 'Setup did not use the bundled demo source.'
        Check (!(Test-Path -LiteralPath $demoRuntime)) 'Setup copied or invented runtime settings for a fresh demo.'
    }
    if ($scenario -eq 'portable') {
        Check ((Hash $configuration) -eq (Hash (Join-Path $script:packageDirectory 'Launcher.toml'))) 'Default seeding replaced the transferred portable profile.'
    } elseif ($scenario -eq 'existing') {
        Check ([IO.File]::ReadAllText($configuration) -eq 'existing user configuration') 'Setup changed an existing personal profile.'
    } elseif ($scenario -eq 'repeat') {
        Check ([IO.File]::ReadAllText($configuration) -eq 'existing repeated personal configuration') 'An upgrade changed a customized existing main configuration.'
    } elseif ($scenario -eq 'stock-links') {
        $updated = [IO.File]::ReadAllText($configuration)
        Check ($updated.Contains('$EXE\docs\Launcher.html') -and $updated.Contains('$EXE\docs\Installer.html') -and !$updated.Contains('$EXE\docs\Launcher.md')) 'Setup did not migrate old stock guide links to HTML.'
        Check ($updated.Contains("target = '" + '$SELF' + "'") -and
            $updated.Contains('"$USERDATA\Demos\demo_many_colors.toml"') -and
            !$updated.Contains('$EXE\examples\demo_many_colors.cmd')) 'Setup did not redirect old stock demo launch/read links into AppData.'
        Check ($updated.Contains('# Keep this personal comment and unrelated item.') -and $updated.Contains("target = 'C:\Custom\Utility.exe'")) 'Stock-link migration changed unrelated personal content.'
    } else {
        Check ((Hash $configuration) -eq (Hash $template)) 'Setup seeded from the download rather than the installed template.'
        $installed = [IO.File]::ReadAllText($configuration)
        Check ($installed.Contains('$EXE\docs\Launcher.html') -and $installed.Contains('$EXE\docs\Installer.html') -and
            $installed.Contains("target = '" + '$SELF' + "'") -and
            $installed.Contains('"$USERDATA\Demos\demo_many_colors.toml"')) 'Fresh installed configuration lacks HTML and AppData demo links.'
    }
    if ($scenario -in @('upgrade', 'repeat')) {
        Check ($script:finishLabel.Text.Contains('NOT copied during upgrades')) 'Default seeding hid the upgrade portable-file warning.'
    }
    Check (!(Test-Path -LiteralPath (Join-Path $install 'Launcher.toml'))) 'Setup created the editable profile inside Program Files.'
    Check (!(Test-Path -LiteralPath (Join-Path $script:userDataDirectory 'Launcher.installed.default.toml'))) 'Setup imported a program-owned installed template as personal profile data.'
}
Write-Output "Installer configuration passed: $script:checks checks. Isolated profiles: $run"
