#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$OutputDirectory = [IO.Path]::GetTempPath(),
    [Alias('ProfileScript')][string]$PortableProfileScript
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (!$PortableProfileScript) { $PortableProfileScript = Join-Path $PSScriptRoot '..\tools\installer\PortableProfile.ps1' }
. $PortableProfileScript
$script:checks = 0
function Check([bool]$Condition, [string]$Message) { $script:checks++; if (!$Condition) { throw $Message } }
function WriteText([string]$Path, [string]$Text) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Reject([scriptblock]$Action, [string]$Reason) {
    $message = ''
    try { & $Action | Out-Null } catch { $message = $_.Exception.Message }
    Check ($message.Contains($Reason)) ('Expected rejection containing "' + $Reason + '"; got "' + $message + '".')
}
function NoStaging([string]$Directory) {
    if ([IO.Directory]::Exists($Directory)) {
        Check (@(Get-ChildItem -LiteralPath $Directory -Force -Filter '.launcher-profile-*').Count -eq 0) 'Profile transfer left staging content.'
    }
}

$run = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('launcher-portable-profile-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($run)
function NewPackage([string]$Name) {
    $directory = Join-Path $run $Name
    [void][IO.Directory]::CreateDirectory($directory)
    foreach ($name in @('Launcher.toml', 'Launcher.runtime.toml', 'Studio.TOML', 'colors.toml',
        'Icon\nested\small.png', 'Icons\large.svg', 'Icons\nested\container.dll')) {
        WriteText (Join-Path $directory $name) ("portable fixture $name")
    }
    foreach ($name in @('Launcher.default.toml', 'Launcher.exe', 'notes.txt',
        'examples\colors.toml', 'artwork\other.png', 'Iconography\ignored.svg', 'presets\other.toml')) {
        WriteText (Join-Path $directory $name) ("excluded fixture $name")
    }
    return $directory
}
function NewUser([string]$Name) { return (Join-Path $run ("profiles\$Name\Launcher")) }

$package = NewPackage 'portable'
$fresh = NewUser 'fresh'
$plan = Get-LauncherPortableProfilePlan $package $fresh Install
Check ($plan.Files.Count -eq 7 -and $plan.CopyFiles.Count -eq 7) 'Fresh transfer did not capture exactly root TOMLs and recursive Icon/Icons files.'
Check (!$plan.Warning) 'A fresh profile unexpectedly warned.'
Check ($plan.Summary.Contains($fresh) -and $plan.Summary.Contains('Icon\nested\small.png')) 'Fresh review omits destination or relative artwork paths.'
Check (!(Test-Path -LiteralPath $fresh)) 'Read-only inspection created the user-data directory.'
$summary = Invoke-LauncherPortableProfileTransfer $plan
Check ($summary.Contains('7') -and $summary.Contains($fresh)) 'Completion summary omits transferred file count or destination.'
foreach ($file in $plan.Files) {
    Check ((Hash $file.DestinationPath) -eq $file.Sha256) "Transfer changed bytes or lost a subtree: $($file.RelativePath)"
}
Check (!(Test-Path -LiteralPath (Join-Path $fresh 'Launcher.default.toml'))) 'Bundled starter was imported as personal data.'
Check (!(Test-Path -LiteralPath (Join-Path $fresh 'examples'))) 'Examples were guessed to be part of the profile.'
Check (!(Test-Path -LiteralPath (Join-Path $fresh 'artwork'))) 'An unrecognized artwork directory was imported.'
NoStaging $fresh

$matching = Get-LauncherPortableProfilePlan $package $fresh Install
Check (!$matching.Warning -and !$matching.CopyFiles.Count) 'An identical existing profile was classified as conflicting.'
$null = Invoke-LauncherPortableProfileTransfer $matching

$empty = Join-Path $run 'empty-package'
WriteText (Join-Path $empty 'Launcher.default.toml') 'bundled starter'
WriteText (Join-Path $empty 'examples\colors.toml') 'sample palette'
$emptyUser = NewUser 'empty'
$emptyPlan = Get-LauncherPortableProfilePlan $empty $emptyUser Install
Check (!$emptyPlan.Files.Count -and $emptyPlan.Summary -like '*no files to transfer*') 'An empty portable profile has no explicit review summary.'
$null = Invoke-LauncherPortableProfileTransfer $emptyPlan
Check (!(Test-Path -LiteralPath $emptyUser)) 'An empty profile created AppData directories.'

# A genuinely matching subset may be completed on fresh Install. Unrelated logs
# are outside the profile scope and do not turn it into another user's profile.
$subset = NewUser 'matching-subset'
WriteText (Join-Path $subset 'Launcher.toml') ([IO.File]::ReadAllText((Join-Path $package 'Launcher.toml')))
WriteText (Join-Path $subset 'Launcher.log') 'existing diagnostic'
$subsetPlan = Get-LauncherPortableProfilePlan $package $subset Install
Check (!$subsetPlan.Warning -and $subsetPlan.CopyFiles.Count -eq 6) 'An identical subset was not eligible for fresh transfer.'
$null = Invoke-LauncherPortableProfileTransfer $subsetPlan
Check ([IO.File]::ReadAllText((Join-Path $subset 'Launcher.log')) -eq 'existing diagnostic') 'Profile transfer altered an unrelated file.'
Check ((Hash (Join-Path $subset 'Icon\nested\small.png')) -eq (Hash (Join-Path $package 'Icon\nested\small.png'))) 'Subset transfer lost nested artwork.'
NoStaging $subset

foreach ($conflictName in @('Launcher.toml', 'older.toml', 'Icons\old.svg')) {
    $conflict = NewUser ('conflict-' + [IO.Path]::GetFileName($conflictName))
    WriteText (Join-Path $conflict $conflictName) 'existing profile must survive'
    $before = Hash (Join-Path $conflict $conflictName)
    $conflictPlan = Get-LauncherPortableProfilePlan $package $conflict Install
    Check ($conflictPlan.Warning.StartsWith('WARNING:') -and $conflictPlan.Warning.Contains($conflictName)) 'A differing or destination-only profile file did not warn.'
    Check (!$conflictPlan.CopyFiles.Count) 'A conflicting profile offered a partial merge.'
    $null = Invoke-LauncherPortableProfileTransfer $conflictPlan
    Check ((Hash (Join-Path $conflict $conflictName)) -eq $before) 'A conflicting existing profile was overwritten.'
    Check (!(Test-Path -LiteralPath (Join-Path $conflict 'Studio.TOML'))) 'A conflict filled a missing file from another profile.'
    NoStaging $conflict
}

$upgrade = Get-LauncherPortableProfilePlan $package $fresh Upgrade
Check (!$upgrade.Warning -and !$upgrade.CopyFiles.Count) 'An identical upgrade warned or offered copying.'
WriteText (Join-Path $package 'Launcher.runtime.toml') 'portable saved settings changed'
WriteText (Join-Path $package 'Icons\added.svg') 'new portable artwork'
$upgrade = Get-LauncherPortableProfilePlan $package $fresh Upgrade
Check ($upgrade.Warning.StartsWith('WARNING:') -and $upgrade.Warning.Contains('NOT copied during upgrades')) 'Upgrade warning omits the explicit no-copy behavior.'
Check ($upgrade.Warning.Contains('Launcher.runtime.toml') -and $upgrade.Warning.Contains('Icons\added.svg')) 'Upgrade warning omits edited or added portable files.'
Check (!$upgrade.Warning.Contains('  Launcher.toml')) 'Upgrade warning includes a file that matches AppData.'
$runtimeBefore = Hash (Join-Path $fresh 'Launcher.runtime.toml')
$null = Invoke-LauncherPortableProfileTransfer $upgrade
Check ((Hash (Join-Path $fresh 'Launcher.runtime.toml')) -eq $runtimeBefore) 'Upgrade changed installed runtime settings.'
Check (!(Test-Path -LiteralPath (Join-Path $fresh 'Icons\added.svg'))) 'Upgrade copied new portable artwork.'
$absentUpgrade = NewUser 'upgrade-absent'
$absentPlan = Get-LauncherPortableProfilePlan $package $absentUpgrade Upgrade
Check ($absentPlan.Warning.Contains('Icons\added.svg') -and !$absentPlan.CopyFiles.Count) 'Upgrade without AppData failed to warn about omitted files.'
$null = Invoke-LauncherPortableProfileTransfer $absentPlan
Check (!(Test-Path -LiteralPath $absentUpgrade)) 'Upgrade created or imported an absent profile.'

# The coordinator refreshes its plan after graceful shutdown. A stale plan must
# still fail safely if an edit occurs between that inspection and transfer.
$staleSource = NewPackage 'stale-source'
$staleUser = NewUser 'stale-source'
$stalePlan = Get-LauncherPortableProfilePlan $staleSource $staleUser Install
$stalePath = Join-Path $staleSource 'Launcher.runtime.toml'
WriteText $stalePath 'saved after review'
Reject { Invoke-LauncherPortableProfileTransfer $stalePlan } $stalePath
Check (!(Test-Path -LiteralPath $staleUser)) 'A stale source plan created destination files.'
$refreshed = Get-LauncherPortableProfilePlan $staleSource $staleUser Install
$null = Invoke-LauncherPortableProfileTransfer $refreshed
Check ([IO.File]::ReadAllText((Join-Path $staleUser 'Launcher.runtime.toml')) -eq 'saved after review') 'A refreshed plan did not transfer the shutdown-saved runtime settings.'

$lateSource = NewPackage 'late-source'
$lateUser = NewUser 'late-source'
$latePlan = Get-LauncherPortableProfilePlan $lateSource $lateUser Install
$latePath = Join-Path $lateSource 'late.toml'
Reject { Invoke-LauncherPortableProfileTransfer $latePlan -BeforePublish { WriteText $latePath 'created during staging' } } $latePath
Check (!(Test-Path -LiteralPath $lateUser)) 'A source addition during staging left a partial profile.'

$racingUser = NewUser 'racing-destination'
$racingPlan = Get-LauncherPortableProfilePlan $package $racingUser Install
$racingPath = Join-Path $racingUser 'Launcher.toml'
Reject { Invoke-LauncherPortableProfileTransfer $racingPlan -BeforePublish { WriteText $racingPath 'concurrently created profile' } } $racingPath
Check ([IO.File]::ReadAllText($racingPath) -eq 'concurrently created profile') 'A racing destination file was overwritten or removed.'
Check (!(Test-Path -LiteralPath (Join-Path $racingUser 'Studio.TOML'))) 'A racing destination caused a partial profile transfer.'
NoStaging $racingUser

# Source bytes cannot change while a transfer is using them, even when a writer
# attempts to save between staging and publication.
$pinnedUser = NewUser 'pinned-source'
$pinnedPlan = Get-LauncherPortableProfilePlan $package $pinnedUser Install
$script:sourceWriteBlocked = $false
$null = Invoke-LauncherPortableProfileTransfer $pinnedPlan -BeforePublish {
    try { [IO.File]::WriteAllText((Join-Path $package 'Launcher.toml'), 'must not replace pinned bytes') }
    catch { $script:sourceWriteBlocked = $true }
}
Check $script:sourceWriteBlocked 'A source file could be modified while the transfer used it.'

# Inject a CREATE_NEW conflict after the first publication. Owned handles allow
# rollback of the first file while the concurrent file remains intact.
$rollbackUser = NewUser 'publication-rollback'
$rollbackPlan = Get-LauncherPortableProfilePlan $package $rollbackUser Install
$script:publishCount = 0
$originalPublish = (Get-Item Function:\Publish-LauncherPortableProfileFile).ScriptBlock
Set-Item Function:\Publish-LauncherPortableProfileFile {
    param($Stage, [string]$DestinationPath)
    $script:publishCount++
    if ($script:publishCount -eq 2) { [IO.File]::WriteAllText($DestinationPath, 'racing publication') }
    & $originalPublish $Stage $DestinationPath
}
try { Reject { Invoke-LauncherPortableProfileTransfer $rollbackPlan } $rollbackPlan.CopyFiles[1].DestinationPath }
finally { Set-Item Function:\Publish-LauncherPortableProfileFile $originalPublish }
Check (!(Test-Path -LiteralPath $rollbackPlan.CopyFiles[0].DestinationPath)) 'Rollback retained an earlier owned profile file.'
Check ([IO.File]::ReadAllText($rollbackPlan.CopyFiles[1].DestinationPath) -eq 'racing publication') 'Rollback deleted a racing file it did not own.'
NoStaging $rollbackUser

# Missing paths are distinct from unreadable files, in both source and AppData.
$locked = Join-Path $package 'Launcher.toml'
$lock = [IO.File]::Open($locked, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
try { Reject { Get-LauncherPortableProfilePlan $package (NewUser 'locked-source') Upgrade } $locked }
finally { $lock.Dispose() }
$locked = Join-Path $fresh 'Launcher.toml'
$lock = [IO.File]::Open($locked, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
try { Reject { Get-LauncherPortableProfilePlan $package $fresh Install } $locked }
finally { $lock.Dispose() }

foreach ($invalidName in @('Icon', 'Icons')) {
    $invalid = Join-Path $run ('invalid-' + $invalidName)
    WriteText (Join-Path $invalid $invalidName) 'a file where an icon directory belongs'
    Reject { Get-LauncherPortableProfilePlan $invalid (NewUser ('invalid-' + $invalidName)) Install } (Join-Path $invalid $invalidName)
}
$invalidToml = Join-Path $run 'directory-toml'
[void][IO.Directory]::CreateDirectory((Join-Path $invalidToml 'Launcher.toml'))
Reject { Get-LauncherPortableProfilePlan $invalidToml (NewUser 'directory-toml') Install } (Join-Path $invalidToml 'Launcher.toml')

$outside = Join-Path $run 'outside'
WriteText (Join-Path $outside 'external.svg') 'must not traverse a junction'
$linked = NewPackage 'linked-icons'
$junction = Join-Path $linked 'Icons\external'
[void](New-Item -ItemType Junction -Path $junction -Target $outside)
Reject { Get-LauncherPortableProfilePlan $linked (NewUser 'linked-icons') Install } $junction
$sourceJunction = Join-Path $run 'linked-package'
[void](New-Item -ItemType Junction -Path $sourceJunction -Target $package)
Reject { Get-LauncherPortableProfilePlan $sourceJunction (NewUser 'linked-package') Install } $sourceJunction
$destinationJunction = Join-Path $run 'linked-user'
[void](New-Item -ItemType Junction -Path $destinationJunction -Target $outside)
Reject { Get-LauncherPortableProfilePlan $package (Join-Path $destinationJunction 'Launcher') Install } $destinationJunction
Check (@(Get-ChildItem -LiteralPath $outside -Force).Count -eq 1) 'Junction inspection wrote into the external tree.'

foreach ($overlap in @($package, (Join-Path $package 'child'), $run)) {
    Reject { Get-LauncherPortableProfilePlan $package $overlap Install } 'non-overlapping'
}

# Setup seeds only the absent default after portable adoption. Templates stay
# program-owned; the new user file has ordinary writable file attributes.
$template = Join-Path $run 'installed program\Launcher.default.toml'
WriteText $template "schema_version=1`n[[tabs]]`nname='Starter'`nitems=[{target='notepad.exe'}]`n"
$templateHash = Hash $template
$seeded = NewUser 'seed-default'
$seededPath = Join-Path $seeded 'Launcher.toml'
$summary = Initialize-LauncherInstalledConfiguration -TemplatePath $template -UserDataDirectory $seeded
Check ($summary.Contains('Created starter configuration:') -and $summary.Contains($seededPath)) 'Seeding completion omits the created default path.'
Check ((Hash $seededPath) -eq $templateHash) 'Installed seeding changed the template bytes.'
Check (@(Get-ChildItem -LiteralPath $seeded -Force).Count -eq 1) 'Seeding copied runtime, palettes, icons or other profile files.'
Check (!((Get-Item -LiteralPath $seededPath).Attributes -band [IO.FileAttributes]::ReadOnly)) 'The seeded default is not writable.'
Check ((Hash $template) -eq $templateHash) 'Seeding changed the installed template.'

# An adopted portable configuration wins, and a repeat seed does not require a
# template to remain readable. Neither contents nor existing attributes change.
$portableHash = Hash (Join-Path $fresh 'Launcher.toml')
$portableWritten = [IO.File]::GetLastWriteTimeUtc((Join-Path $fresh 'Launcher.toml'))
$summary = Initialize-LauncherInstalledConfiguration -TemplatePath (Join-Path $run 'absent-template\Launcher.default.toml') -UserDataDirectory $fresh
Check ($summary.Contains('Existing configuration preserved:')) 'An existing portable profile was not reported as preserved.'
Check ((Hash (Join-Path $fresh 'Launcher.toml')) -eq $portableHash -and
    [IO.File]::GetLastWriteTimeUtc((Join-Path $fresh 'Launcher.toml')) -eq $portableWritten) 'Seeding refreshed or replaced the adopted portable configuration.'
$seededWritten = [IO.File]::GetLastWriteTimeUtc($seededPath)
[IO.File]::SetAttributes($seededPath, [IO.FileAttributes]::ReadOnly)
try {
    $null = Initialize-LauncherInstalledConfiguration -TemplatePath (Join-Path $run 'absent-template\Launcher.default.toml') -UserDataDirectory $seeded
    Check ((([IO.File]::GetAttributes($seededPath) -band [IO.FileAttributes]::ReadOnly) -ne 0) -and
        (Hash $seededPath) -eq $templateHash -and [IO.File]::GetLastWriteTimeUtc($seededPath) -eq $seededWritten) 'Seeding changed an existing read-only configuration or its timestamps.'
} finally { [IO.File]::SetAttributes($seededPath, [IO.FileAttributes]::Normal) }

$readOnlySeed = NewUser 'seed-readonly-template'
[IO.File]::SetAttributes($template, [IO.FileAttributes]::ReadOnly)
try {
    $null = Initialize-LauncherInstalledConfiguration -TemplatePath $template -UserDataDirectory $readOnlySeed
    Check (!((Get-Item -LiteralPath (Join-Path $readOnlySeed 'Launcher.toml')).Attributes -band [IO.FileAttributes]::ReadOnly)) 'Seeding inherited the template read-only flag.'
    Check (([IO.File]::GetAttributes($template) -band [IO.FileAttributes]::ReadOnly) -ne 0) 'Seeding changed the template read-only flag.'
} finally { [IO.File]::SetAttributes($template, [IO.FileAttributes]::Normal) }

# Upgrades still never import trial data, but an absent installed default is
# seeded afterward. Only Launcher.toml is created, not the trial's other files.
$null = Initialize-LauncherInstalledConfiguration -TemplatePath $template -UserDataDirectory $absentUpgrade
Check ((Hash (Join-Path $absentUpgrade 'Launcher.toml')) -eq $templateHash) 'Upgrade did not seed its absent default from the installed template.'
Check (@(Get-ChildItem -LiteralPath $absentUpgrade -Force).Count -eq 1) 'Upgrade seeding imported runtime, icons or trial configurations.'

$concurrent = NewUser 'seed-concurrent-winner'
$winnerPath = Join-Path $concurrent 'Launcher.toml'
$summary = Initialize-LauncherInstalledConfiguration -TemplatePath $template -UserDataDirectory $concurrent -BeforeCreate {
    WriteText $winnerPath 'another first run created this configuration'
}
Check ($summary.Contains('Existing configuration preserved:')) 'A CREATE_NEW concurrent winner was reported as overwritten.'
Check ([IO.File]::ReadAllText($winnerPath) -eq 'another first run created this configuration') 'Seeding overwrote or deleted a concurrent winner.'
Check (@(Get-ChildItem -LiteralPath $concurrent -Force).Count -eq 1) 'Concurrent-winner handling left extra files.'

$failedSeed = NewUser 'seed-copy-error'
Reject { Initialize-LauncherInstalledConfiguration -TemplatePath $template -UserDataDirectory $failedSeed -BeforeCommit { throw 'forced installed seed failure' } } 'forced installed seed failure'
Check (!(Test-Path -LiteralPath (Join-Path $failedSeed 'Launcher.toml'))) 'Failed seeding left its uncommitted configuration.'
Check (!(Test-Path -LiteralPath $failedSeed)) 'Failed seeding left its newly created empty profile directory.'
Check ((Hash $template) -eq $templateHash) 'Failed seeding changed the template.'

$missingSeed = NewUser 'seed-missing-template'
$missingTemplate = Join-Path $run 'no-installed-template\Launcher.default.toml'
Reject { Initialize-LauncherInstalledConfiguration -TemplatePath $missingTemplate -UserDataDirectory $missingSeed } 'no-installed-template'
Check (!(Test-Path -LiteralPath $missingSeed)) 'Missing-template failure created a user profile.'
$templateLock = [IO.File]::Open($template, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
$lockedSeed = NewUser 'seed-locked-template'
try { Reject { Initialize-LauncherInstalledConfiguration -TemplatePath $template -UserDataDirectory $lockedSeed } $template }
finally { $templateLock.Dispose() }
Check (!(Test-Path -LiteralPath $lockedSeed)) 'Unreadable-template failure created a user profile.'

$directorySeed = NewUser 'seed-directory-target'
[void][IO.Directory]::CreateDirectory((Join-Path $directorySeed 'Launcher.toml'))
Reject { Initialize-LauncherInstalledConfiguration -TemplatePath $template -UserDataDirectory $directorySeed } 'regular file'
$linkedTemplate = Join-Path $run 'seed-linked-template'
[void](New-Item -ItemType Junction -Path $linkedTemplate -Target ([IO.Path]::GetDirectoryName($template)))
Reject { Initialize-LauncherInstalledConfiguration -TemplatePath (Join-Path $linkedTemplate 'Launcher.default.toml') -UserDataDirectory (NewUser 'seed-linked-template') } $linkedTemplate
Reject { Initialize-LauncherInstalledConfiguration -TemplatePath $template -UserDataDirectory (Join-Path $destinationJunction 'SeededLauncher') } $destinationJunction
Check (@(Get-ChildItem -LiteralPath $outside -Force).Count -eq 1) 'Installed seeding traversed a linked destination.'

# Install the writable demo before first use. Its exact nested profile stays
# separate from the main configuration, and later installs preserve edits.
$demoTemplate = Join-Path $run 'installed program\examples\demo_many_colors.toml'
WriteText $demoTemplate "schema_version=1`n[[tabs]]`nname='Many colors'`nitems=[{target='notepad.exe'}]`n[logging]`nfile='demo_many_colors.log'`n"
$demoTemplateHash = Hash $demoTemplate
$demoUser = NewUser 'demo-install-time'
$demoMain = Join-Path $demoUser 'Launcher.toml'
$demoMainRuntime = Join-Path $demoUser 'Launcher.runtime.toml'
WriteText $demoMain '# Preserve my customized main configuration.'
WriteText $demoMainRuntime '# Preserve my saved main position.'
$demoMainHash = Hash $demoMain
$demoMainRuntimeHash = Hash $demoMainRuntime
$demoPath = Join-Path $demoUser 'Demos\demo_many_colors.toml'
$demoRuntime = Join-Path $demoUser 'Demos\demo_many_colors.runtime.toml'
[IO.File]::SetAttributes($demoTemplate, [IO.FileAttributes]::ReadOnly)
try {
    $summary = Initialize-LauncherInstalledDemo -TemplatePath $demoTemplate -UserDataDirectory $demoUser
    Check ($summary.Contains('Color demo: Created starter configuration:') -and $summary.Contains($demoPath)) 'Demo installation did not report its separate personal destination.'
    Check ((Hash $demoPath) -ceq $demoTemplateHash) 'Demo installation changed the supplied template bytes.'
    Check (!((Get-Item -LiteralPath $demoPath).Attributes -band [IO.FileAttributes]::ReadOnly)) 'The installed demo inherited read-only source attributes.'
    Check (!(Test-Path -LiteralPath $demoRuntime)) 'Demo installation invented saved runtime settings.'
    Check ((Hash $demoMain) -ceq $demoMainHash -and (Hash $demoMainRuntime) -ceq $demoMainRuntimeHash) 'Demo seeding touched the main profile.'
    Check ((Hash $demoTemplate) -ceq $demoTemplateHash -and (([IO.File]::GetAttributes($demoTemplate) -band [IO.FileAttributes]::ReadOnly) -ne 0)) 'Demo seeding altered the installed reference.'
} finally { [IO.File]::SetAttributes($demoTemplate, [IO.FileAttributes]::Normal) }

WriteText $demoPath '# User changed the demo colors and items.'
WriteText $demoRuntime "[dock]`nposition=0.21`n"
$demoHash = Hash $demoPath
$demoRuntimeHash = Hash $demoRuntime
$demoWritten = [IO.File]::GetLastWriteTimeUtc($demoPath)
$summary = Initialize-LauncherInstalledDemo -TemplatePath (Join-Path $run 'unavailable-demo\demo_many_colors.toml') -UserDataDirectory $demoUser
Check ($summary.Contains('Existing configuration preserved:')) 'A repeat demo installation did not preserve the working copy.'
Check ((Hash $demoPath) -ceq $demoHash -and (Hash $demoRuntime) -ceq $demoRuntimeHash -and
    [IO.File]::GetLastWriteTimeUtc($demoPath) -eq $demoWritten) 'A repeat installation refreshed edited demo preferences.'

$demoConcurrent = NewUser 'demo-concurrent-winner'
$demoWinner = Join-Path $demoConcurrent 'Demos\demo_many_colors.toml'
$summary = Initialize-LauncherInstalledDemo -TemplatePath $demoTemplate -UserDataDirectory $demoConcurrent -BeforeCreate {
    WriteText $demoWinner '# A concurrent first launch created the demo.'
}
Check ($summary.Contains('Existing configuration preserved:') -and
    [IO.File]::ReadAllText($demoWinner) -ceq '# A concurrent first launch created the demo.') 'Demo seeding overwrote a concurrent winner.'

$demoFailed = NewUser 'demo-failed-seed'
Reject { Initialize-LauncherInstalledDemo -TemplatePath $demoTemplate -UserDataDirectory $demoFailed -BeforeCommit { throw 'forced demo seed failure' } } 'forced demo seed failure'
Check (!(Test-Path -LiteralPath $demoFailed)) 'Failed demo creation left a partial file or newly created profile directories.'
Check ((Hash $demoTemplate) -ceq $demoTemplateHash) 'Failed demo creation changed the reference template.'

$demoMissing = NewUser 'demo-missing-template'
Reject { Initialize-LauncherInstalledDemo -TemplatePath (Join-Path $run 'missing-demo\demo_many_colors.toml') -UserDataDirectory $demoMissing } 'missing-demo'
Check (!(Test-Path -LiteralPath $demoMissing)) 'A missing demo template created a profile.'
$demoDirectory = NewUser 'demo-directory-target'
[void][IO.Directory]::CreateDirectory((Join-Path $demoDirectory 'Demos\demo_many_colors.toml'))
Reject { Initialize-LauncherInstalledDemo -TemplatePath $demoTemplate -UserDataDirectory $demoDirectory } 'regular file'
Reject { Initialize-LauncherInstalledDemo -TemplatePath $demoTemplate -UserDataDirectory (Join-Path $destinationJunction 'DemoSeed') } $destinationJunction
Check (@(Get-ChildItem -LiteralPath $outside -Force).Count -eq 1) 'Demo installation traversed a linked profile destination.'
$legacyUser = NewUser 'legacy-demo-migration'
$legacyDemo = Join-Path $legacyUser 'Demos\ManyColors\Launcher.toml'
$legacyRuntime = Join-Path $legacyUser 'Demos\ManyColors\Launcher.runtime.toml'
WriteText $legacyDemo '# My edited legacy demo.'
WriteText $legacyRuntime "[dock]`nposition=0.37`n"
$null = Initialize-LauncherInstalledDemo -TemplatePath (Join-Path $run 'missing-source.toml') -UserDataDirectory $legacyUser
Check ((Hash (Join-Path $legacyUser 'Demos\demo_many_colors.toml')) -ceq (Hash $legacyDemo)) 'Flattening the demo discarded legacy edits.'
Check ((Hash (Join-Path $legacyUser 'Demos\demo_many_colors.runtime.toml')) -ceq (Hash $legacyRuntime)) 'Flattening the demo discarded legacy runtime settings.'
Check ([IO.File]::Exists($legacyDemo) -and [IO.File]::Exists($legacyRuntime)) 'Demo migration removed its original recovery copies.'
Write-Output "Installer portable profile checks passed: $script:checks; fixtures: $run"
