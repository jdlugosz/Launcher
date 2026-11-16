#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$DiscoveryScript,
    [string]$OutputDirectory = [IO.Path]::GetTempPath()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (!$DiscoveryScript) { $DiscoveryScript = Join-Path $PSScriptRoot '..\tools\installer\Discovery.ps1' }
. $DiscoveryScript

$script:checks = 0
function Check([bool]$Condition, [string]$Message) {
    $script:checks++
    if (!$Condition) { throw $Message }
}
function FileHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)) }
    finally { $algorithm.Dispose(); $stream.Dispose() }
}

$fixtureParent = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
$fixtureRoot = Join-Path $fixtureParent ('launcher-discovery-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($fixtureRoot)
$script:metadata = @{}
$publisher = 'D' + [char]0x0142 + 'ugosz'
$script:product = $publisher + ' Launcher'
$script:copyright = 'Copyright ' + [char]0x00a9 + ' 2026 John ' + $publisher

function New-Scenario([string]$Name) {
    $root = Join-Path $fixtureRoot $Name
    return @{
        PackageDirectory = Join-Path $root 'source'
        LocalDestination = Join-Path $root 'user'
        MachineDestination = Join-Path $root 'machine'
        StartupDirectory = Join-Path $root 'startup'
    }
}

function Add-Executable([string]$Directory, [string]$Name = 'Launcher.exe',
    [string]$State = 'Verified', [string]$Component = 'Launcher') {
    [void][IO.Directory]::CreateDirectory($Directory)
    $path = Join-Path $Directory $Name
    [IO.File]::WriteAllText($path, 'Metadata is supplied by the test reader; this is not executable code.')
    $value = [pscustomobject]@{
        ProductName = $script:product; LegalCopyright = $script:copyright
        ProductVersion = '1.2.0'; InternalName = $Component; OriginalFilename = $Component + '.exe'
        IsSpecialBuild = $false; IsPrivateBuild = $false; SpecialBuild = ''; PrivateBuild = ''
    }
    if ($Component -eq 'LauncherSvgExtractor') {
        $value.ProductName = $publisher + ' Launcher SVG Extractor'
        $value.InternalName = 'LauncherSvgExtractor.exe'
    }
    switch ($State) {
        'Legacy' { $value.ProductName = 'Launcher'; $value.LegalCopyright = '' }
        'Foreign' { $value.ProductName = 'Some unrelated vendor launcher' }
        'Special' { $value.IsSpecialBuild = $true; $value.SpecialBuild = 'test instrumentation' }
        'WrongOwner' { $value.LegalCopyright = 'Copyright (c) 2026 Another Publisher' }
        'WrongComponent' { $value.OriginalFilename = 'AnotherProgram.exe' }
        'Unknown' { $value = $null }
    }
    $script:metadata[$path] = $value
    return $path
}

$reader = {
    param($Path)
    if (!$script:metadata.ContainsKey($Path) -or $null -eq $script:metadata[$Path]) { throw 'Fixture metadata cannot be read.' }
    return $script:metadata[$Path]
}
$noProcesses = { @() }
$ownerReader = {
    param($Process)
    if ($Process.ProcessId -eq 108) { throw 'Fixture owner access denied.' }
    return 'FIXTURE\TestUser'
}
function Read-Scenario($Scenario, [scriptblock]$Processes = $noProcesses) {
    $fixtureProcesses = @(& $Processes)
    foreach ($process in $fixtureProcesses) {
        $process | Add-Member -NotePropertyName CreationDate -NotePropertyValue ([datetime]'2026-10-21T12:00:00Z') -Force
    }
    Get-LauncherDiscovery @Scenario -MetadataReader $reader -ProcessReader { $fixtureProcesses }.GetNewClosure() `
        -ProcessOwnerReader $ownerReader -ProcessOwnerSidReader { 'S-1-5-21-101-102-103-1001' }
}

try {
    $scenario = New-Scenario 'fresh'
    $result = Read-Scenario $scenario
    Check ($result.Installations.Count -eq 0) 'A fresh machine must have no installations.'
    Check (@($result.Locations | Where-Object { $_.State -ne 'Missing' }).Count -eq 0) 'Absent directories must be Missing.'
    Check ($result.Processes.Count -eq 0) 'The empty process provider must produce an empty inventory.'

    $scenario = New-Scenario 'split-version'
    $split = Add-Executable $scenario.PackageDirectory 'Launcher-R1.1.0.38.exe'
    $splitMetadata = $script:metadata[$split]
    $splitMetadata.ProductVersion = '1.1'
    $splitMetadata | Add-Member -NotePropertyMembers @{
        ProductMajorPart = 1; ProductMinorPart = 1; ProductBuildPart = 0; ProductPrivatePart = 0
        FileVersion = '1.1.0.38'; FileMajorPart = 1; FileMinorPart = 1; FileBuildPart = 0; FilePrivatePart = 38
    }
    Check ((Get-LauncherPackageExecutable -PackageDirectory $scenario.PackageDirectory -MetadataReader $reader) -ceq $split) `
        'Package discovery rejected the numbered file version because the product version is the series.'
    $identity = Get-LauncherExecutableIdentity $split $reader
    Check ($identity.State -ceq 'Verified' -and $identity.Version -ceq '1.1.0.38') 'Discovery did not report the exact file version.'
    $splitMetadata.FilePrivatePart = 37
    $rejected = $false
    try { $null = Get-LauncherPackageExecutable -PackageDirectory $scenario.PackageDirectory -MetadataReader $reader }
    catch { $rejected = $true }
    Check $rejected 'Package discovery accepted a disagreement between text and fixed file version.'

    $scenario = New-Scenario 'one'
    [void](Add-Executable $scenario.LocalDestination)
    $result = Read-Scenario $scenario
    Check ($result.Installations.Count -eq 1) 'A nonrunning current-user installation must be found.'
    Check ($result.Installations[0].Scope -eq 'CurrentUser' -and $result.Installations[0].CanUpgrade) 'The current-user installation must be eligible for upgrade.'

    $scenario = New-Scenario 'both'
    [void](Add-Executable $scenario.LocalDestination)
    [void](Add-Executable $scenario.MachineDestination)
    $result = Read-Scenario $scenario
    Check ($result.Installations.Count -eq 2) 'Both nonrunning installations must be found.'
    Check (@($result.Installations | Where-Object { !$_.CanUpgrade }).Count -eq 0) 'Both verified installations must be eligible for upgrade.'

    $scenario = New-Scenario 'versioned'
    [void](Add-Executable $scenario.MachineDestination 'Launcher-R1.2.exe')
    $result = Read-Scenario $scenario
    Check ($result.Installations.Count -eq 1 -and $result.Installations[0].Scope -eq 'AllUsers') 'A preserved versioned main executable must identify the installation even if the active link is absent.'
    Check ($result.Issues.Count -gt 0) 'A missing active executable must be reported.'

    $scenario = New-Scenario 'legacy'
    [void](Add-Executable $scenario.MachineDestination 'Launcher.exe' 'Legacy')
    $result = Read-Scenario $scenario
    Check ($result.Installations.Count -eq 0) 'Legacy metadata must not silently force an upgrade.'
    Check ($result.LegacyInstallations.Count -eq 1) 'Legacy installation evidence must remain visible.'
    Check ($result.Locations[0].State -eq 'Legacy' -and !$result.Locations[0].CanUpgrade) 'Legacy identity must require validation.'

    $scenario = New-Scenario 'current-with-old-release'
    [void](Add-Executable $scenario.MachineDestination)
    [void](Add-Executable $scenario.MachineDestination 'Launcher-R1.0.exe' 'Legacy')
    $result = Read-Scenario $scenario
    Check ($result.Installations.Count -eq 1 -and $result.Installations[0].CanUpgrade) 'A verified active application must remain identified beside its old preserved release.'
    Check ($result.Issues.Count -gt 0) 'The old release must still be reported for review.'

    foreach ($state in @('Foreign', 'Special', 'WrongOwner', 'WrongComponent', 'Unknown')) {
        $scenario = New-Scenario $state
        [void](Add-Executable $scenario.MachineDestination 'Launcher.exe' $state)
        $result = Read-Scenario $scenario
        Check ($result.Installations.Count -eq 0) "$state metadata must not identify an installation."
        Check ($result.Locations[0].State -in @('Foreign', 'Unknown') -and $result.Issues.Count -gt 0) "$state metadata must produce explicit findings."
    }

    $scenario = New-Scenario 'copyright-years'
    $path = Add-Executable $scenario.MachineDestination
    $script:metadata[$path].LegalCopyright = $script:copyright.Replace('2026', '2020-2027')
    $result = Read-Scenario $scenario
    Check ($result.Installations.Count -eq 1) 'The copyright owner identity must not depend on the current year.'
    $script:metadata[$path].IsPrivateBuild = $true
    $result = Read-Scenario $scenario
    Check ($result.Installations.Count -eq 0) 'A private build must not be treated as an installable application.'

    $scenario = New-Scenario 'helpers-only'
    [void](Add-Executable $scenario.MachineDestination 'LauncherDrop.exe' 'Verified' 'LauncherDrop')
    $extractorOnly = Add-Executable $scenario.MachineDestination 'LauncherSvgExtractor.exe' 'Verified' 'LauncherSvgExtractor'
    [void](Add-Executable $scenario.MachineDestination 'LauncherAboutTests.exe')
    $result = Read-Scenario $scenario
    Check ($result.Installations.Count -eq 0 -and $result.Locations[0].State -eq 'Available') 'A helper or test executable alone must not force an upgrade.'
    Check ((Get-LauncherExecutableIdentity $extractorOnly $reader).State -eq 'Verified') 'The C# SVG Extractor identity was not recognized.'
    $script:metadata[$extractorOnly].ProductName = $script:product
    Check ((Get-LauncherExecutableIdentity $extractorOnly $reader).State -eq 'Foreign') 'The SVG Extractor accepted the main application product identity.'
    $script:metadata[$extractorOnly].ProductName = $publisher + ' Launcher SVG Extractor'
    $probe = @($result.Locations[0].Candidates | Where-Object { $_.Name -eq 'LauncherAboutTests.exe' })[0]
    Check ($probe.Identity.State -eq 'Foreign' -and $probe.Identity.Component -eq 'Other') 'A probe with copied main-app metadata must be distinguished by its actual name.'

    $scenario = New-Scenario 'empty-directory'
    [void][IO.Directory]::CreateDirectory($scenario.LocalDestination)
    $result = Read-Scenario $scenario
    Check ($result.Locations[1].State -eq 'Available') 'An existing empty directory must be distinct from inaccessible/legacy content.'

    $scenario = New-Scenario 'directory-denied'
    $result = Get-LauncherDiscovery @scenario -MetadataReader $reader -ProcessReader $noProcesses -DirectoryReader { throw 'Fixture directory access denied.' }
    Check ($result.Installations.Count -eq 0 -and $result.Issues.Count -eq 2) 'Unreadable directories must remain unknown with explicit issues.'
    Check (@($result.Locations | Where-Object { $_.State -eq 'Unknown' }).Count -eq 2) 'Unreadable directories must never be reported absent.'

    $scenario = New-Scenario 'mixed'
    [void](Add-Executable $scenario.MachineDestination 'Launcher-R1.2.exe')
    [void](Add-Executable $scenario.MachineDestination 'Launcher.exe' 'Foreign')
    $result = Read-Scenario $scenario
    Check ($result.Installations.Count -eq 1 -and $result.Installations[0].State -eq 'NeedsReview') 'A foreign active entry beside a verified preserved release must require review.'
    Check (!$result.Installations[0].CanUpgrade) 'Mixed content must prevent automatic upgrade.'

    # A reparse-point active entry must not be filtered out. No symbolic-link
    # creation privileges are needed for this read-provider contract check.
    $scenario = New-Scenario 'active-link'
    $link = Add-Executable $scenario.MachineDestination
    $linkEntry = [pscustomobject]@{ FullName = $link; Attributes = [IO.FileAttributes]::ReparsePoint }
    $machineDirectory = $scenario.MachineDestination
    $linkReader = {
        param($Path)
        if ($Path -eq $machineDirectory) { return [pscustomobject]@{ Exists = $true; Entries = @($linkEntry) } }
        return [pscustomobject]@{ Exists = $false; Entries = @() }
    }.GetNewClosure()
    $result = Get-LauncherDiscovery @scenario -MetadataReader $reader -ProcessReader $noProcesses -DirectoryReader $linkReader
    Check ($result.Installations.Count -eq 1 -and $result.Installations[0].CanUpgrade) 'Active links must be examined rather than discarded as reparse points.'

    $scenario = New-Scenario 'processes'
    $source = Add-Executable $scenario.PackageDirectory
    $user = Add-Executable $scenario.LocalDestination
    $machine = Add-Executable $scenario.MachineDestination 'Launcher-R1.2.exe'
    $other = Add-Executable (Join-Path $fixtureRoot 'elsewhere')
    $helper = Add-Executable $scenario.PackageDirectory 'LauncherDrop.exe' 'Verified' 'LauncherDrop'
    $foreign = Add-Executable (Join-Path $fixtureRoot 'foreign-running') 'Launcher.exe' 'Foreign'
    $legacy = Add-Executable (Join-Path $fixtureRoot 'legacy-running') 'Launcher.exe' 'Legacy'
    $probe = Add-Executable $scenario.PackageDirectory 'LauncherAboutTests.exe'
    $script:fixtureProcesses = @(
        [pscustomobject]@{ ProcessId = 101; Name = 'Launcher.exe'; ExecutablePath = $source; SessionId = 1 },
        [pscustomobject]@{ ProcessId = 102; Name = 'Launcher.exe'; ExecutablePath = $user; SessionId = 1 },
        [pscustomobject]@{ ProcessId = 103; Name = 'Launcher-R1.2.exe'; ExecutablePath = $machine; SessionId = 1 },
        [pscustomobject]@{ ProcessId = 104; Name = 'Launcher.exe'; ExecutablePath = $other; SessionId = 2 },
        [pscustomobject]@{ ProcessId = 105; Name = 'LauncherDrop.exe'; ExecutablePath = $helper; SessionId = 1 },
        [pscustomobject]@{ ProcessId = 106; Name = 'Launcher.exe'; ExecutablePath = $foreign; SessionId = 1 },
        [pscustomobject]@{ ProcessId = 107; Name = 'Launcher.exe'; ExecutablePath = $legacy; SessionId = 1 },
        [pscustomobject]@{ ProcessId = 108; Name = 'Launcher.exe'; ExecutablePath = $null; SessionId = $null },
        [pscustomobject]@{ ProcessId = 109; Name = 'LauncherAboutTests.exe'; ExecutablePath = $probe; SessionId = 1 }
    )
    $result = Read-Scenario $scenario { $script:fixtureProcesses }
    Check ($result.Processes.Count -eq 9) 'All matching process candidates must be retained in the inventory.'
    $byId = @{}
    foreach ($process in $result.Processes) { $byId[$process.ProcessId] = $process }
    Check ($byId[101].Location -eq 'Source' -and $byId[101].IsSource) 'The source process must be classified by its path.'
    Check ($byId[101].OwnerSid -eq 'S-1-5-21-101-102-103-1001' -and
        $byId[101].CreationTimeUtc -eq '2026-10-21T12:00:00.0000000Z') 'Discovery must retain owner SID and UTC creation time for exact process revalidation.'
    Check ($byId[102].IsCurrentUserInstall -and $byId[103].IsAllUsersInstall) 'Both installation process paths must be classified.'
    Check ($byId[104].Location -eq 'Elsewhere' -and $byId[104].SessionId -eq 2) 'Other copies and sessions must remain visible.'
    Check ($byId[105].Component -eq 'LauncherDrop') 'LauncherDrop must be distinguished from the main application.'
    Check ($byId[106].Identity.State -eq 'Foreign' -and $byId[107].Identity.State -eq 'Legacy') 'Foreign and legacy processes must not become verified applications.'
    Check ($byId[108].Location -eq 'Unknown' -and $byId[108].Issues.Count -ge 3) 'An unreadable process must expose its missing path, session, and owner.'
    Check ($byId[109].Component -eq 'Other' -and $byId[109].Identity.State -eq 'Foreign') 'A main-resource probe must not become a normal Launcher process.'
    $report = Format-LauncherDiscovery $result
    Check ($report.Contains('PID 101') -and $report.Contains('FIXTURE\TestUser') -and $report.Contains('<unknown executable path>')) 'The human-readable report must include process identity and unknown evidence.'

    $scenario.PackageDirectory = $scenario.LocalDestination
    $result = Read-Scenario $scenario { $script:fixtureProcesses }
    $overlap = @($result.Processes | Where-Object { $_.ProcessId -eq 102 })[0]
    Check ($overlap.IsSource -and $overlap.IsCurrentUserInstall) 'Source/destination overlap must preserve both independent flags.'

    $result = Get-LauncherDiscovery @scenario -MetadataReader $reader -ProcessReader { throw 'Fixture snapshot access denied.' }
    Check (!$result.ProcessEnumerationSucceeded -and $result.Processes.Count -eq 0 -and
        ($result.Issues -join ' ').Contains('Cannot enumerate')) 'Process enumeration failure must be explicit rather than a false empty finding.'

    $scenario = New-Scenario 'native-process-records'
    $source = Add-Executable $scenario.PackageDirectory
    $nativeRecords = @(
        [pscustomobject]@{
            ProcessId = 301; Name = 'Launcher.exe'; ExecutablePath = $source; SessionId = 1
            CreationDate = [datetime]'2026-10-21T12:00:00Z'; Owner = 'FIXTURE\TestUser'
            OwnerSid = 'S-1-5-21-101-102-103-1001'; Issues = @()
        },
        [pscustomobject]@{
            ProcessId = 302; Name = 'Launcher.exe'; ExecutablePath = $null; SessionId = $null
            CreationDate = $null; Owner = $null; OwnerSid = $null
            Issues = @('Fixture native process inspection access denied.')
        }
    )
    $result = Get-LauncherDiscovery @scenario -MetadataReader $reader -ProcessReader { $nativeRecords }.GetNewClosure()
    Check ($result.ProcessEnumerationSucceeded -and $result.Processes.Count -eq 2) 'One inaccessible process must not turn a successful native snapshot into an empty or globally failed enumeration.'
    $known = @($result.Processes | Where-Object { $_.ProcessId -eq 301 })[0]
    Check ($known.Identity.State -eq 'Verified' -and $known.Owner -eq 'FIXTURE\TestUser' -and
        $known.OwnerSid -eq 'S-1-5-21-101-102-103-1001' -and $known.Issues.Count -eq 0) 'Native process records must provide owner identity directly without CIM owner methods.'
    $unknown = @($result.Processes | Where-Object { $_.ProcessId -eq 302 })[0]
    Check ($unknown.Identity.State -eq 'Unknown' -and $unknown.Location -eq 'Unknown' -and
        ($unknown.Issues -join ' ').Contains('Fixture native process inspection access denied.')) 'An inaccessible native process must remain an unknown candidate with its inspection failure visible.'

    $scenario = New-Scenario 'backup-filter'
    $normal = Add-Executable $scenario.MachineDestination
    $backup = Add-Executable $scenario.MachineDestination 'Launcher.BackUP.exe' 'Unknown'
    $script:fixtureProcesses = @(
        [pscustomobject]@{ ProcessId = 108; Name = 'Launcher.BackUP.exe'; ExecutablePath = $backup; SessionId = 1 },
        [pscustomobject]@{ ProcessId = 108; Name = 'Launcher.exe'; ExecutablePath = $backup; SessionId = 1 }
    )
    $result = Read-Scenario $scenario { $script:fixtureProcesses }
    Check ($result.Locations[0].Candidates.Count -eq 1) 'Backup leaf names must be omitted from directory candidates before metadata lookup.'
    Check ($result.Processes.Count -eq 0 -and $result.Issues.Count -eq 0) 'Backup processes must be omitted before their failing metadata/owner lookups can produce warnings.'
    $scenario = New-Scenario 'Backup parent directory'
    [void](Add-Executable $scenario.MachineDestination)
    $result = Read-Scenario $scenario
    Check ($result.Installations.Count -eq 1) 'The word backup in an ancestor directory must not hide a real installation.'

    $scenario = New-Scenario 'report-symbols'
    $source = Add-Executable $scenario.PackageDirectory
    $user = Add-Executable $scenario.LocalDestination
    $machine = Add-Executable $scenario.MachineDestination
    [void](Add-Executable $scenario.LocalDestination 'Launcher-R1.2.exe')
    $helper = Add-Executable $scenario.LocalDestination 'LauncherDrop.exe' 'Verified' 'LauncherDrop'
    $extractor = Add-Executable $scenario.LocalDestination 'LauncherSvgExtractor.exe' 'Verified' 'LauncherSvgExtractor'
    $other = Add-Executable (Join-Path $fixtureRoot 'portable-report')
    $foreign = Add-Executable $scenario.PackageDirectory 'LauncherOther.exe' 'Foreign'
    $script:fixtureProcesses = @(
        [pscustomobject]@{ ProcessId = 201; Name = 'Launcher.exe'; ExecutablePath = $source; SessionId = 1 },
        [pscustomobject]@{ ProcessId = 202; Name = 'Launcher.exe'; ExecutablePath = $user; SessionId = 1 },
        [pscustomobject]@{ ProcessId = 203; Name = 'Launcher.exe'; ExecutablePath = $machine; SessionId = 1 },
        [pscustomobject]@{ ProcessId = 204; Name = 'LauncherDrop.exe'; ExecutablePath = $helper; SessionId = 1 },
        [pscustomobject]@{ ProcessId = 205; Name = 'Launcher.exe'; ExecutablePath = $other; SessionId = 1 },
        [pscustomobject]@{ ProcessId = 206; Name = 'LauncherOther.exe'; ExecutablePath = $foreign; SessionId = 1 },
        [pscustomobject]@{ ProcessId = 207; Name = 'LauncherSvgExtractor.exe'; ExecutablePath = $extractor; SessionId = 1 }
    )
    $result = Read-Scenario $scenario { $script:fixtureProcesses }
    $report = Format-LauncherDiscovery $result -SelectedDestination $scenario.LocalDestination
    $up = [string][char]0x2191
    $ignored = [string][char]0x2013
    $play = [string][char]0x25b6
    Check ($report.Contains('Local Programs (current user): FOUND | ' + $scenario.LocalDestination) -and $report.Contains($up + ' Targeted upgrade | ' + $user + ' | PID 202')) 'The selected installation must appear in the plain summary and its active process must be marked for upgrade.'
    Check ($report.Contains($ignored + ' Ignored other installation | ' + $machine + ' | PID 203')) 'The other recognized installation must be marked ignored with its full path on the same line.'
    Check ($report.Contains($play + ' Portable currently running | ' + $source + ' | PID 201') -and $report.Contains($play + ' Portable currently running | ' + $other + ' | PID 205')) 'Source and elsewhere running copies must be marked portable.'
    Check ($report.Contains($ignored + ' Ignored companion | ' + $helper + ' | PID 204') -and $report.Contains($ignored + ' Ignored foreign or other program | ' + $foreign + ' | PID 206')) 'Companion and foreign process markers must include accessible text.'
    foreach ($companion in @($helper, $extractor)) {
        Check ($report.Contains($ignored + ' Ignored companion | ' + $companion + ' | version')) 'Both installed companions must have the same recognized companion status.'
    }
    Check ($report.Contains($ignored + ' Ignored companion | ' + $extractor + ' | PID 207')) 'The running SVG Extractor must be labeled as a companion.'
    foreach ($component in @('LauncherDrop', 'LauncherSvgExtractor')) {
        foreach ($state in @('Foreign', 'Special', 'Unknown')) {
            $path = Add-Executable (Join-Path $fixtureRoot ($component + '-' + $state)) ($component + '.exe') $state $component
            $identity = Get-LauncherExecutableIdentity $path $reader
            $marker = Get-LauncherDiscoveryMarker $identity $path $scenario.LocalDestination $false $true
            Check ($marker -ne ($ignored + ' Ignored companion')) 'A companion-shaped executable must pass identity checks before receiving companion status.'
        }
    }
    Check ($report.Contains($ignored + ' Ignored preserved release | ' + (Join-Path $scenario.LocalDestination 'Launcher-R1.2.exe') + ' | version')) 'Immutable archived releases must be marked preserved even in the selected directory.'
    $report = Format-LauncherDiscovery $result -SelectedDestination $scenario.MachineDestination
    Check ($report.Contains($up + ' Targeted upgrade | ' + $machine + ' | PID 203') -and $report.Contains($ignored + ' Ignored other installation | ' + $user + ' | PID 202')) 'Report markers must follow a changed selected destination.'

    $scenario = New-Scenario 'fresh-startup'
    $result = Get-LauncherDiscovery @scenario -MetadataReader $reader -ProcessReader $noProcesses
    $plan = Get-LauncherStartupPlan $result $scenario.LocalDestination -StartOnSignIn $true
    Check ($plan.DefaultChecked -and $plan.Action -eq 'Create') 'A NOT FOUND target must default sign-in startup to checked.'

    $scenario = New-Scenario 'startup'
    $user = Add-Executable $scenario.LocalDestination
    $machine = Add-Executable $scenario.MachineDestination
    $script:shortcutReads = 0
    $script:shortcutTarget = $user
    $shortcutReader = {
        param($Path)
        $script:shortcutReads++
        $link = [pscustomobject]@{ TargetPath = $script:shortcutTarget; WorkingDirectory = 'Fixture working directory'; Arguments = '--config "preserved fixture.toml"' }
        $link | Add-Member -MemberType ScriptMethod -Name Save -Value { throw 'Discovery must not save a shortcut.' }
        return $link
    }
    $result = Get-LauncherDiscovery @scenario -MetadataReader $reader -ProcessReader $noProcesses -ShortcutReader $shortcutReader
    Check ($result.Startup.State -eq 'Missing' -and !$result.Startup.Exists -and $script:shortcutReads -eq 0) 'Absent Startup shortcuts must not be opened or created.'
    $plan = Get-LauncherStartupPlan $result $scenario.LocalDestination -StartOnSignIn $true
    Check ($plan.Action -eq 'Create' -and !$plan.DefaultChecked) 'A checked new sign-in option must plan creation without claiming an existing shortcut.'
    $plan = Get-LauncherStartupPlan $result $scenario.LocalDestination -StartOnLogin $false
    Check ($plan.Action -eq 'None') 'An unchecked absent shortcut must require no action.'
    [void][IO.Directory]::CreateDirectory($scenario.StartupDirectory)
    $shortcutPath = Join-Path $scenario.StartupDirectory 'Launcher.lnk'
    [IO.File]::WriteAllText($shortcutPath, 'Read-only shortcut fixture; metadata supplied by reader.')
    $shortcutHash = FileHash $shortcutPath
    $result = Get-LauncherDiscovery @scenario -MetadataReader $reader -ProcessReader $noProcesses -ShortcutReader $shortcutReader
    Check ($result.Startup.State -eq 'Readable' -and $result.Startup.Exists -and $script:shortcutReads -eq 1) `
        ('An existing Startup shortcut must be read without saving. State={0}; reads={1}; issues={2}' -f
            $result.Startup.State, $script:shortcutReads, ($result.Startup.Issues -join '; '))
    Check ($result.Startup.Sha256 -eq ((Get-FileHash -LiteralPath $shortcutPath -Algorithm SHA256).Hash)) 'Shortcut discovery must retain the reviewed file hash.'
    Check ($result.Startup.CurrentScope -eq 'CurrentUser' -and $result.Startup.Arguments.Contains('preserved fixture.toml')) 'Shortcut discovery must retain scope, working directory, and arguments.'
    $plan = Get-LauncherStartupPlan $result $scenario.LocalDestination -StartOnSignIn $true
    Check ($plan.Action -eq 'Keep' -and $plan.DefaultChecked -and $plan.MatchesSelectedDestination -and $plan.MatchesExistingInstallation) 'A selected recognized shortcut must initialize checked and plan keeping its arguments.'
    $plan = Get-LauncherStartupPlan $result $scenario.LocalDestination -StartOnSignIn $false
    Check ($plan.Action -eq 'Remove') 'Explicitly unchecking startup for its selected recognized installation must plan removal.'
    $script:shortcutTarget = $machine
    $result = Get-LauncherDiscovery @scenario -MetadataReader $reader -ProcessReader $noProcesses -ShortcutReader $shortcutReader
    $plan = Get-LauncherStartupPlan $result $scenario.LocalDestination -StartOnSignIn $false
    Check ($plan.Action -eq 'None' -and !$plan.DefaultChecked -and $plan.MatchesExistingInstallation) 'An unchecked shortcut for the other recognized installation must remain unchanged.'
    $plan = Get-LauncherStartupPlan $result $scenario.LocalDestination -StartOnSignIn $true
    Check ($plan.Action -eq 'Retarget' -and $plan.Explanation.Contains($machine) -and $plan.Explanation.Contains($user) -and $plan.Explanation.Contains('preserve its existing arguments')) 'Explicitly enabling the selected installation must describe the retarget and argument preservation.'
    $script:shortcutTarget = Join-Path $fixtureRoot 'unrecognized\Launcher.exe'
    $result = Get-LauncherDiscovery @scenario -MetadataReader $reader -ProcessReader $noProcesses -ShortcutReader $shortcutReader
    $plan = Get-LauncherStartupPlan $result $scenario.LocalDestination -StartOnSignIn $true
    Check ($plan.Action -eq 'Review' -and !$plan.MatchesExistingInstallation) 'An unrelated readable Startup target must require review before replacement.'
    $plan = Get-LauncherStartupPlan $result $scenario.LocalDestination -StartOnSignIn $false
    Check ($plan.Action -eq 'None') 'An unrelated Startup target must be preserved when unchecked.'
    $result = Get-LauncherDiscovery @scenario -MetadataReader $reader -ProcessReader $noProcesses -ShortcutReader { throw 'Fixture shortcut access denied.' }
    Check ($result.Startup.State -eq 'Unknown' -and $result.Startup.Exists -and $result.Startup.Issues.Count -gt 0) 'Unreadable shortcut evidence must remain unknown and visible.'
    $plan = Get-LauncherStartupPlan $result $scenario.LocalDestination -StartOnSignIn $true
    Check ($plan.Action -eq 'Review') 'An unreadable Startup entry must require review when startup is requested.'
    $plan = Get-LauncherStartupPlan $result $scenario.LocalDestination -StartOnSignIn $false
    Check ($plan.Action -eq 'None') 'An unreadable Startup entry must remain untouched when startup is not requested.'
    Check ((FileHash $shortcutPath) -eq $shortcutHash) 'Startup discovery and planning must leave the existing shortcut file byte-for-byte unchanged.'

    $scenario = New-Scenario 'startup-missing-active'
    [void](Add-Executable $scenario.LocalDestination 'Launcher-R1.2.exe')
    [void][IO.Directory]::CreateDirectory($scenario.StartupDirectory)
    [IO.File]::WriteAllText((Join-Path $scenario.StartupDirectory 'Launcher.lnk'), 'Read-only fixture')
    $script:shortcutTarget = Join-Path $scenario.LocalDestination 'Launcher.exe'
    $result = Get-LauncherDiscovery @scenario -MetadataReader $reader -ProcessReader $noProcesses -ShortcutReader $shortcutReader
    $plan = Get-LauncherStartupPlan $result $scenario.LocalDestination -StartOnSignIn $false
    Check ($plan.Action -eq 'None' -and !$plan.MatchesExistingInstallation -and !$plan.DefaultChecked) 'A missing active EXE must not make a path-matching shortcut owned or removable.'

    Write-Output ('Installer discovery passed: {0} checks.' -f $script:checks)
} finally {
    $resolvedRoot = [IO.Path]::GetFullPath($fixtureRoot)
    if ([IO.Path]::GetDirectoryName($resolvedRoot) -ne $fixtureParent -or
        [IO.Path]::GetFileName($resolvedRoot) -notmatch '^launcher-discovery-[a-f0-9]{32}$') {
        throw 'Refusing to remove an unexpected fixture directory.'
    }
    Remove-Item -LiteralPath $resolvedRoot -Recurse -Force -ErrorAction Stop
}
