#requires -Version 5.1
[CmdletBinding()]
param(
    [switch]$ValidateUi,
    [switch]$DiscoveryOnly,
    [string]$RenderDirectory,
    [switch]$RenderDetails,
    [ValidateSet(0, 96, 120, 144, 192)][int]$RenderDpi = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:packageDirectory = $PSScriptRoot
. (Join-Path $PSScriptRoot 'Discovery.ps1')
$publisher = 'D' + [char]0x0142 + 'ugosz'
$script:userDataDirectory = [IO.Path]::Combine([Environment]::GetFolderPath('LocalApplicationData'), $publisher, 'Launcher')
$script:localDestination = [System.IO.Path]::Combine(
    [Environment]::GetFolderPath('LocalApplicationData'), 'Programs', $publisher, 'Launcher')
$script:machineDestination = [System.IO.Path]::Combine(
    $(if ($env:ProgramW6432) { $env:ProgramW6432 } else { [Environment]::GetFolderPath('ProgramFiles') }), $publisher, 'Launcher')
if ($DiscoveryOnly) {
    Get-LauncherDiscovery -PackageDirectory $script:packageDirectory |
        ConvertTo-Json -Depth 12
    return
}
. (Join-Path $PSScriptRoot 'Deployment.ps1')
. (Join-Path $PSScriptRoot 'PortableProfile.ps1')
. (Join-Path $PSScriptRoot 'Lifecycle.ps1')
. (Join-Path $PSScriptRoot 'Worker.ps1')
. (Join-Path $PSScriptRoot 'Presentation.ps1')
$script:previousDpiContext = Initialize-LauncherPresentation
try {
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-LauncherDpiFormType
[System.Windows.Forms.Application]::EnableVisualStyles()
[System.Windows.Forms.Application]::SetCompatibleTextRenderingDefault($false)

$script:pageIndex = 0
$script:pages = [System.Collections.Generic.List[object]]::new()
$script:worker = $null
$script:workerDirectory = $null
$script:installRequest = $null
$script:reviewedDiscovery = $null
$script:workerElevated = $false
$script:retainWorkerFiles = $false
$script:installing = $false
$script:installedExecutable = $null
$script:installedDeployment = $null
$script:restartQueue = [Collections.Generic.List[object]]::new()
$script:permissionFallbackNote = ''
$script:profileReviewError = ''
$script:profileReviewText = ''
$script:discovery = $null
$script:applyingDiscovery = $false
$script:updatingStartupCheck = $false
$script:startupChoices = @{}
$script:startupPlan = $null

function New-ValidationDiscovery {
    param(
        [string[]]$InstalledScopes = @(),
        [string[]]$BlockedScopes = @(),
        [string]$StartupScope
    )
    $locations = foreach ($scope in @('AllUsers', 'CurrentUser')) {
        $directory = if ($scope -eq 'AllUsers') { $script:machineDestination } else { $script:localDestination }
        $state = if ($InstalledScopes -contains $scope) { 'Verified' }
            elseif ($BlockedScopes -contains $scope) { 'Foreign' } else { 'Missing' }
        [pscustomobject]@{
            Scope = $scope; Directory = $directory; State = $state
            IsInstalled = $state -eq 'Verified'; CanUpgrade = $state -eq 'Verified'
            Identity = $null; Version = if ($state -eq 'Verified') { '1.0.0' } else { $null }
            Candidates = if ($state -eq 'Verified') {
                @([pscustomobject]@{
                    Name = 'Launcher.exe'; Path = Join-Path $directory 'Launcher.exe'
                    IsActive = $true
                    Identity = [pscustomobject]@{ State = 'Verified'; Component = 'Launcher'; Version = '1.0.0' }
                })
            } else { @() }
        }
    }
    return [pscustomobject]@{
        PackageDirectory = $script:packageDirectory; Locations = @($locations)
        Installations = @($locations | Where-Object IsInstalled)
        LegacyInstallations = @(); Processes = @(); Issues = @()
        Startup = [pscustomobject]@{
            Path = 'C:\Validation\Startup\Launcher.lnk'
            Exists = -not [string]::IsNullOrWhiteSpace($StartupScope)
            State = if ($StartupScope) { 'Readable' } else { 'Missing' }
            TargetDirectory = if ($StartupScope -eq 'AllUsers') { $script:machineDestination }
                elseif ($StartupScope -eq 'CurrentUser') { $script:localDestination } else { $null }
            TargetPath = if ($StartupScope -eq 'AllUsers') { Join-Path $script:machineDestination 'Launcher.exe' }
                elseif ($StartupScope -eq 'CurrentUser') { Join-Path $script:localDestination 'Launcher.exe' } else { $null }
            WorkingDirectory = ''; Arguments = ''; CurrentScope = $StartupScope; Issues = @()
        }
    }
}

function Add-PageControl {
    param(
        [System.Windows.Forms.TableLayoutPanel]$Layout,
        [System.Windows.Forms.Control]$Control
    )

    $row = $Layout.RowCount
    $Layout.RowCount++
    [void]$Layout.RowStyles.Add([System.Windows.Forms.RowStyle]::new(
        [System.Windows.Forms.SizeType]::AutoSize))
    $Layout.Controls.Add($Control, 0, $row)
}

function Add-PageText {
    param(
        [System.Windows.Forms.TableLayoutPanel]$Layout,
        [string]$Text,
        [switch]$Muted
    )

    $label = [System.Windows.Forms.Label]::new()
    $label.Text = $Text
    $label.AutoSize = $true
    $label.Dock = 'Fill'
    $label.Margin = [System.Windows.Forms.Padding]::new(0, 0, 0, 16)
    if ($Muted) { $label.ForeColor = [System.Drawing.SystemColors]::GrayText }
    Add-PageControl $Layout $label
    return $label
}

function New-WizardPage {
    param([string]$Title, [string]$Description)

    $panel = [System.Windows.Forms.Panel]::new()
    $panel.Dock = 'Fill'
    $panel.AutoScroll = $true
    $panel.Visible = $false

    $layout = [System.Windows.Forms.TableLayoutPanel]::new()
    $layout.Dock = 'Top'
    $layout.AutoSize = $true
    $layout.AutoSizeMode = 'GrowAndShrink'
    $layout.ColumnCount = 1
    $layout.RowCount = 0
    $layout.Padding = [System.Windows.Forms.Padding]::new(0, 8, 12, 0)
    [void]$layout.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new(
        [System.Windows.Forms.SizeType]::Percent, 100))
    $panel.Controls.Add($layout)
    $script:pageHost.Controls.Add($panel)

    $page = [pscustomobject]@{
        Title = $Title
        Description = $Description
        Panel = $panel
        Layout = $layout
        FirstControl = $null
    }
    $script:pages.Add($page)
    return $page
}

function Get-SelectedScope {
    if ($script:allUsersRadio.Checked) { return 'All users' }
    return 'Just me'
}

function Get-SelectedLocation {
    $scope = if ($script:allUsersRadio.Checked) { 'AllUsers' } else { 'CurrentUser' }
    return @($script:discovery.Locations | Where-Object Scope -eq $scope)[0]
}

function Get-SelectedDestination {
    return (Get-SelectedLocation).Directory
}

function Get-SelectedAction {
    if ((Get-SelectedLocation).IsInstalled) { return 'Upgrade' }
    return 'Install'
}

function Get-DiscoverySummary {
    $count = @($script:discovery.Installations).Count
    $summary = if ($count -eq 0) { 'No verified Launcher installation was found.' }
        elseif ($count -eq 1) { 'An existing Launcher installation was found. Setup will upgrade it.' }
        else { 'Both personal and all-users installations were found. Setup will upgrade your personal installation.' }
    $running = @($script:discovery.Processes | Where-Object { $_.Identity.State -eq 'Verified' }).Count
    $summary += if ((Get-LauncherDiscoveryProperty $script:discovery 'ProcessEnumerationSucceeded') -eq $false) {
        "`r`nUnable to check running Launcher / LauncherDrop copies."
    } else { "`r`n$running verified running Launcher / LauncherDrop process(es)." }
    $elsewhere = @($script:discovery.Processes | Where-Object {
        $_.Identity.State -eq 'Verified' -and -not $_.IsSource -and
        -not $_.IsCurrentUserInstall -and -not $_.IsAllUsersInstall
    }).Count
    if ($elsewhere -gt 0) {
        $summary += "`r`n$elsewhere running copy/copies are outside the source and installation folders; see details."
    }
    $blocked = @($script:discovery.Locations | Where-Object { $_.State -notin @('Verified', 'Missing', 'Available') })
    if ($blocked.Count -gt 0) {
        $summary += "`r`nSome application folders require manual review; see Discovery details."
    }
    if (@($script:discovery.Issues).Count -gt 0) {
        $summary += "`r`n$(@($script:discovery.Issues).Count) finding(s) need attention; see Discovery details."
    }
    return $summary
}

function Apply-Discovery {
    param([object]$Discovery)
    $previousPersonalChoice = $null -ne $script:discovery -and $script:justMeRadio.Checked
    $script:applyingDiscovery = $true
    try {
        $script:discovery = $Discovery
        $installs = @($Discovery.Installations)
        foreach ($scopeName in @('AllUsers', 'CurrentUser')) {
            $location = @($Discovery.Locations | Where-Object Scope -eq $scopeName)[0]
            $radio = if ($scopeName -eq 'AllUsers') { $script:allUsersRadio } else { $script:justMeRadio }
            $allowed = $location.State -in @('Verified', 'Missing', 'Available')
            $radio.Enabled = $allowed -and ($installs.Count -eq 0 -or $location.IsInstalled)
            if ($installs.Count -gt 1 -and $scopeName -eq 'AllUsers') { $radio.Enabled = $false }
            $label = if ($scopeName -eq 'AllUsers') { '&All users' } else { '&Just me' }
            $radio.Text = if ($installs.Count -gt 1 -and $scopeName -eq 'AllUsers') { "$label - Existing installation (left alone)" }
                elseif ($location.IsInstalled) { "$label - Upgrade" }
                elseif (-not $allowed) { "$label - Review required" }
                elseif ($scopeName -eq 'AllUsers') { "$label (recommended)" } else { $label }
        }
        # When both exist, update the personal installation and leave the
        # machine installation alone; it may be managed by an administrator.
        $choosePersonal = $script:justMeRadio.Enabled -and (
            -not $script:allUsersRadio.Enabled -or ($installs.Count -eq 0 -and $previousPersonalChoice))
        $script:allUsersRadio.Checked = -not $choosePersonal -and $script:allUsersRadio.Enabled
        $script:justMeRadio.Checked = $choosePersonal
        $script:scopeNote.Text = if ($installs.Count -eq 1) {
            'Upgrade the existing installation in its current folder. Its location determines the installation scope.'
        } elseif ($installs.Count -gt 1) {
            'Upgrade your personal installation in its current folder. The all-users installation will be left alone.'
        } elseif (-not $script:allUsersRadio.Enabled -and -not $script:justMeRadio.Enabled) {
            'The application folders need manual review before installation can continue. Read the discovery details on the first page.'
        } else {
            'Program Files is recommended for a fresh installation. A personal installation is available when administrator approval is unavailable.'
        }
        $script:pages[1].Title = if ($installs.Count -gt 0) { 'Upgrade Launcher' } else { 'Who will use Launcher?' }
        $script:pages[1].Description = if ($installs.Count -gt 0) {
            'An existing installation will be upgraded in place.'
        } else { 'Choose an installation for your account or for everyone on this computer.' }
        $script:findingsLabel.Text = Get-DiscoverySummary
    } finally { $script:applyingDiscovery = $false }
    Update-InstallationChoices
}

function Refresh-Discovery {
    $script:refreshButton.Enabled = $false
    try {
        $discovered = Get-LauncherDiscovery -PackageDirectory $script:packageDirectory `
            -LocalDestination $script:localDestination -MachineDestination $script:machineDestination
        Apply-Discovery $discovered
    } finally { $script:refreshButton.Enabled = $true }
}

function Update-InstallationChoices {
    if ($script:applyingDiscovery -or $null -eq $script:discovery) { return }
    $selected = Get-SelectedLocation
    $script:destinationBox.Text = Get-SelectedDestination
    $script:destinationNote.Text = if ($selected.IsInstalled) {
        'Upgrade the existing installation in this folder. Setup will preserve your configuration.'
    } elseif ($selected.State -notin @('Missing', 'Available')) {
        'This folder requires manual review. Setup will not choose it automatically.'
    } elseif ($script:allUsersRadio.Checked) {
        'Setup will request administrator approval only if this folder requires it.'
    } else {
        'A personal installation uses your local application folder.'
    }
    $startupKey = $selected.Directory.TrimEnd('\').ToLowerInvariant()
    $defaultPlan = Get-LauncherStartupPlan -Discovery $script:discovery `
        -SelectedDestination $selected.Directory -StartOnSignIn $false
    $script:updatingStartupCheck = $true
    try {
        $script:startupCheck.Checked = if ($script:startupChoices.ContainsKey($startupKey)) {
            [bool]$script:startupChoices[$startupKey]
        } else { [bool]$defaultPlan.DefaultChecked }
    } finally { $script:updatingStartupCheck = $false }
    $script:startupPlan = Get-LauncherStartupPlan -Discovery $script:discovery `
        -SelectedDestination $selected.Directory -StartOnSignIn $script:startupCheck.Checked
    $script:startupNote.Text = $script:startupPlan.Explanation
    $profileError = ''
    try {
        $profile = Get-LauncherPortableProfilePlan -PackageDirectory $script:packageDirectory `
            -UserDataDirectory $script:userDataDirectory -Action (Get-SelectedAction)
        $profileReview = $profile.Summary
        if ($profile.Warning) { $profileReview += "`r`n`r`n" + $profile.Warning }
    } catch {
        $profileError = $_.Exception.Message
        $profileReview = 'WARNING: Portable files could not be checked. ' + $profileError
    }
    $script:profileReviewError = $profileError
    $script:profileReviewText = $profileReview
    $packageReview = 'Package distribution: Prerelease (validation fixture). The previous executable will be retained.'
    if (!$ValidateUi) {
        try {
            $packageExecutable = Get-LauncherPackageExecutable -PackageDirectory $script:packageDirectory
            $metadata = [Diagnostics.FileVersionInfo]::GetVersionInfo($packageExecutable)
            $kind = Get-LauncherDistributionKind -Metadata $metadata
            $retention = if ($kind -eq 'Release') { 'After installation, Setup removes verified older executables. It keeps one exact copy of the previous build for recovery when available. Equal or newer versions and personal settings are preserved.' }
                else { 'The previous executable will be retained.' }
            $packageReview = 'Package: ' + [IO.Path]::GetFileName($packageExecutable) + "`r`nDistribution: " + $kind + "`r`n" + $retention
        } catch {
            $script:profileReviewError = $_.Exception.Message
            $profileError = $script:profileReviewError
            $packageReview = 'WARNING: The package cannot be installed. ' + $profileError
        }
    }
    $script:discoveryDetails.Text = (Format-LauncherDiscovery -Discovery $script:discovery `
        -SelectedDestination $selected.Directory) -replace '\r?\n', "`r`n"
    $startup = if ($script:startupCheck.Checked) { 'Yes' } else { 'No' }
    $approval = if ($script:allUsersRadio.Checked) {
        [Environment]::NewLine + 'Windows will ask for administrator approval for Launcher Setup.'
    } else { '' }
    $summary = @"
Action: $(Get-SelectedAction)

$packageReview

Portable configuration, palettes and icons:
$profileReview

Installation scope: $(Get-SelectedScope)$approval

Application folder:
$(Get-SelectedDestination)

Start Launcher when I sign in: $startup

Startup shortcut: $($script:startupPlan.Action)
$($script:startupPlan.Explanation)

Package folder:
$script:packageDirectory

Discovery findings:
$(Get-DiscoverySummary)

$script:permissionFallbackNote
"@
    # WinForms TextBox expects CRLF even when the source file uses LF endings.
    $script:reviewBox.Text = $summary -replace '\r?\n', "`r`n"
    if ($script:pageIndex -eq 1) {
        $script:nextButton.Enabled = ($script:allUsersRadio.Checked -and $script:allUsersRadio.Enabled) -or
            ($script:justMeRadio.Checked -and $script:justMeRadio.Enabled)
    } elseif ($script:pageIndex -in @(2, 3)) {
        $script:nextButton.Enabled = $script:startupPlan.Action -ne 'Review' -and !$profileError
    }
}

function Show-WizardPage {
    param([int]$Index)

    if ($Index -lt 0 -or $Index -ge $script:pages.Count) {
        throw "Invalid wizard page: $Index"
    }
    # Timer and window callbacks can enter PowerShell with a different DPI
    # context. Lay out page controls in the same context as the wizard window.
    $previous = Initialize-LauncherPresentation
    try {
        $script:pageIndex = $Index
        for ($i = 0; $i -lt $script:pages.Count; $i++) {
            $script:pages[$i].Panel.Visible = ($i -eq $Index)
        }
        $page = $script:pages[$Index]
        $page.Panel.BringToFront()
        $script:titleLabel.Text = $page.Title
        $script:descriptionLabel.Text = $page.Description
        $script:descriptionLabel.Visible = [bool]$page.Description
        $script:stepLabel.Text = 'Step {0} of {1}' -f ($Index + 1), $script:pages.Count
        $script:backButton.Enabled = $Index -gt 0 -and $Index -lt 4
        $script:nextButton.Enabled = $Index -ne 4
        if ($Index -eq 1) {
            $script:nextButton.Enabled = ($script:allUsersRadio.Checked -and $script:allUsersRadio.Enabled) -or
                ($script:justMeRadio.Checked -and $script:justMeRadio.Enabled)
        } elseif ($Index -in @(2, 3)) {
            $script:nextButton.Enabled = $script:startupPlan.Action -ne 'Review' -and !$script:profileReviewError
        }
        $script:nextButton.Text = switch ($Index) {
            3 { if ((Get-SelectedAction) -eq 'Upgrade') { '&Upgrade' } else { '&Install' } }
            5 { '&Finish' }
            default { '&Next >' }
        }
        $script:cancelButton.Visible = $Index -ne 5
        if (-not $RenderDirectory) {
            if ($null -ne $page.FirstControl) { [void]$page.FirstControl.Focus() }
            elseif ($script:nextButton.Enabled) { [void]$script:nextButton.Focus() }
        }
    } finally { Restore-LauncherPresentation $previous }
}

function Clear-InstallWorker {
    if ($null -ne $script:worker -and -not $script:worker.HasExited) { throw 'The installation helper is still working.' }
    if ($null -ne $script:worker) { $script:worker.Dispose(); $script:worker = $null }
    if ($script:workerDirectory -and !$script:retainWorkerFiles) {
        # Delete only our two transport files and the now-empty owned folder.
        try {
            foreach ($name in @('request.json', 'result.json')) {
                $path = Join-Path $script:workerDirectory $name
                if ([IO.File]::Exists($path)) { [IO.File]::Delete($path) }
            }
            if ([IO.Directory]::Exists($script:workerDirectory)) { [IO.Directory]::Delete($script:workerDirectory, $false) }
        } catch {
            Write-Verbose ('Setup transport files were retained: ' + $script:workerDirectory)
        }
    }
    $script:workerDirectory = $null
    $script:retainWorkerFiles = $false
}

function Show-InstallFailure {
    param([string]$Message)
    if ($null -ne $script:worker -and -not $script:worker.HasExited) {
        $script:progressLabel.Text = 'Waiting for the installation helper to finish...'
        return
    }
    $script:progressTimer.Stop()
    $script:installing = $false
    $script:cancelButton.Enabled = $true
    Clear-InstallWorker
    [void][System.Windows.Forms.MessageBox]::Show($script:form, $Message, 'Launcher Setup', 'OK', 'Error')
    Refresh-Discovery
    Show-WizardPage 3
}

function Show-PermissionFallback {
    param([string]$Message)
    if (-not (Test-LauncherCanFallBackToPersonal -Request $script:installRequest)) {
        if ($script:installRequest.Action -eq 'Upgrade') { $Message += "`r`n`r`nThis existing installation must be upgraded in its current folder." }
        Show-InstallFailure $Message
        return
    }
    $script:progressTimer.Stop()
    $script:installing = $false
    $script:cancelButton.Enabled = $true
    Clear-InstallWorker
    $script:startupChoices[$script:localDestination.TrimEnd('\').ToLowerInvariant()] = $script:startupCheck.Checked
    Refresh-Discovery
    if ($script:discovery.Installations.Count -gt 0 -or -not $script:justMeRadio.Enabled) {
        Show-InstallFailure ($Message + "`r`n`r`nThe personal destination also requires review. See the discovery findings.")
        return
    }
    $script:permissionFallbackNote = 'Program Files could not be written with the available permissions. Review the personal application folder before continuing.'
    $script:justMeRadio.Checked = $true
    Update-InstallationChoices
    Show-WizardPage 3
    [void][System.Windows.Forms.MessageBox]::Show($script:form,
        ($Message + "`r`n`r`nSetup has selected Just me. Review the new application folder, then select Install to continue."),
        'Personal installation available', 'OK', 'Information')
}

function Start-InstallWorker {
    param([switch]$Elevated)
    Clear-InstallWorker
    $script:workerDirectory = [IO.Path]::Combine([IO.Path]::GetTempPath(), '.launcher-setup-' + [Guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($script:workerDirectory)
    $requestPath = Join-Path $script:workerDirectory 'request.json'
    $resultPath = Join-Path $script:workerDirectory 'result.json'
    [IO.File]::WriteAllText($requestPath, ($script:installRequest | ConvertTo-Json -Depth 15), [Text.UTF8Encoding]::new($false))
    $powershell = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if (-not [Environment]::Is64BitProcess) {
        $powershell = Join-Path $env:windir 'Sysnative\WindowsPowerShell\v1.0\powershell.exe'
    }
    $workerScript = Join-Path $script:packageDirectory 'Worker.ps1'
    $arguments = '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" -RequestPath "{1}" -ResultPath "{2}"' -f $workerScript, $requestPath, $resultPath
    $options = @{ FilePath = $powershell; ArgumentList = $arguments; WorkingDirectory = $script:packageDirectory; WindowStyle = 'Hidden'; PassThru = $true }
    if ($Elevated) {
        $options.FilePath = Join-Path $script:packageDirectory 'Launcher Setup.exe'
        $options.ArgumentList = '--worker "{0}" "{1}"' -f $requestPath, $resultPath
        $options.WorkingDirectory = [Environment]::SystemDirectory
        $options.Verb = 'RunAs'
    }
    $script:workerElevated = [bool]$Elevated
    try { $script:worker = Start-Process @options }
    catch {
        if ($Elevated -and (Test-LauncherPermissionFailure -Failure $_)) {
            Show-PermissionFallback 'Windows did not grant administrator approval.'
            return
        }
        throw
    }
    $script:progressTimer.Start()
}

function Test-LauncherUnchangedUpgradeProfile {
    param($Request, $Discovery, [bool]$StartOnSignIn)
    if ($Request.Action -cne 'Upgrade') { return $false }
    try {
        $startup = Get-LauncherStartupPlan $Discovery $Request.DestinationDirectory -StartOnSignIn $StartOnSignIn
        if ($startup.Action -cnotin @('None', 'Keep')) { return $false }
        foreach ($relative in @('Launcher.toml', 'Demos\demo_many_colors.toml', 'Icons\many-colors.svg')) {
            $path = Join-Path $script:userDataDirectory $relative
            [void](Assert-LauncherSetupDirectory ([IO.Path]::GetDirectoryName($path)))
            $entry = Get-LauncherSetupEntry $path
            if (!$entry -or $entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) { return $false }
        }
        $configuration = Join-Path $script:userDataDirectory 'Launcher.toml'
        if ([IO.FileInfo]::new($configuration).Length -gt 1MB) { return $false }
        $bytes = [IO.File]::ReadAllBytes($configuration)
        $text = [Text.UTF8Encoding]::new($false, $true).GetString($bytes)
        return $text -ceq (ConvertTo-LauncherInstalledStarterText -Text $text)
    } catch { return $false }
}

function Start-Installation {
    try {
        $payload = Get-LauncherPayload -PackageDirectory $script:packageDirectory
        # Check again when Install/Upgrade is selected; the trial can be edited
        # while the review page is open. Profile writes remain in this process.
        $profile = Get-LauncherPortableProfilePlan -PackageDirectory $script:packageDirectory `
            -UserDataDirectory $script:userDataDirectory -Action (Get-SelectedAction)
        $profileReview = $profile.Summary
        if ($profile.Warning) { $profileReview += "`r`n`r`n" + $profile.Warning }
        if ($profileReview -cne $script:profileReviewText) {
            Update-InstallationChoices
            Show-WizardPage 3
            return
        }
        $script:reviewedDiscovery = $script:discovery
        $script:installRequest = [pscustomobject]@{
            PackageDirectory = $script:packageDirectory; DestinationDirectory = Get-SelectedDestination
            Scope = if ($script:allUsersRadio.Checked) { 'AllUsers' } else { 'CurrentUser' }
            Action = Get-SelectedAction; LocalDestination = $script:localDestination; MachineDestination = $script:machineDestination
            UserSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
            SessionId = [Diagnostics.Process]::GetCurrentProcess().SessionId
            UpdaterTransactionId = (Get-LauncherUpdaterTransactionId $script:packageDirectory).ToString('N')
            QuickRollbackEligible = $false
            StartupDirectory = [Environment]::GetFolderPath('Startup'); Payload = $payload; RestartInstances = @()
        }
        $current = Get-LauncherDiscovery -PackageDirectory $script:packageDirectory -LocalDestination $script:localDestination `
            -MachineDestination $script:machineDestination -StartupDirectory $script:installRequest.StartupDirectory
        $script:installRequest.RestartInstances = @(Get-LauncherInstallationRestartPlan -Discovery $current `
            -PackageDirectory $script:packageDirectory -DestinationDirectory $script:installRequest.DestinationDirectory `
            -UserSid $script:installRequest.UserSid -SessionId $script:installRequest.SessionId)
        $profileBackup = Backup-LauncherFirstDevelopmentConfiguration -PackageDirectory $script:packageDirectory `
            -DestinationDirectory $script:installRequest.DestinationDirectory -UserDataDirectory $script:userDataDirectory
        $script:installRequest.QuickRollbackEligible = !$profileBackup -and
            (Test-LauncherUnchangedUpgradeProfile $script:installRequest $current $script:startupCheck.Checked)
        $script:installing = $true
        $script:cancelButton.Enabled = $false
        $script:progressBar.Value = 0
        $script:progressLabel.Text = 'Validating the package and preparing the application folder...'
        Show-WizardPage 4
        Start-InstallWorker -Elevated:($script:installRequest.Scope -eq 'AllUsers')
    } catch { Show-InstallFailure $_.Exception.Message }
}

function Test-RecoveryShortcutAvailable($Request) {
    try {
        $plan = Get-LauncherRecoveryShortcutPlan ([pscustomobject]@{
            Scope = $Request.Scope; Directory = $Request.DestinationDirectory
        })
        # Setup can create the shortcut before the shared client is installed.
        # Show the callout only when the scoped shortcut has a usable target.
        $target = Get-LauncherSetupEntry $plan.TargetPath
        return [bool]($target -and !$target.PSIsContainer -and
            !($target.Attributes -band [IO.FileAttributes]::ReparsePoint) -and
            (Read-LauncherOwnedStartMenuShortcut $plan))
    } catch { return $false }
}

function Complete-Installation {
    param($Result)
    $script:progressTimer.Stop()
    $script:installing = $false
    $script:cancelButton.Enabled = $true
    $script:installedExecutable = Join-Path $script:installRequest.DestinationDirectory 'Launcher.exe'
    $script:installedDeployment = $Result.Deployment
    $script:restartQueue.Clear()
    foreach ($instance in @($script:installRequest.RestartInstances)) {
        if ($instance.ProcessId -in @($Result.StoppedProcessIds)) { $script:restartQueue.Add($instance) }
    }
    if (!$script:restartQueue.Count) { $script:restartQueue.Add([pscustomobject]@{ Arguments = '' }) }
    $script:launchCheck.Text = if ($script:restartQueue.Count -gt 1) { '&Restart all Launcher instances now' } else { '&Start Launcher now' }
    Clear-InstallWorker
    $script:finishLabel.Text = 'Launcher is installed in:' + "`r`n" + $script:installRequest.DestinationDirectory
    $script:finishDetails.Text = ''
    if ($Result.Deployment -and $null -ne $Result.Deployment.PSObject.Properties['CleanupWarnings']) {
        foreach ($warning in @($Result.Deployment.CleanupWarnings)) {
            $script:finishLabel.Text += "`r`n`r`nWARNING: " + $warning
        }
    }
    if ($null -ne $Result.PSObject.Properties['RecoveryShortcutWarning'] -and $Result.RecoveryShortcutWarning) {
        $script:finishLabel.Text += "`r`n`r`nWARNING: " + $Result.RecoveryShortcutWarning
    }
    $script:recoveryHint.Visible = Test-RecoveryShortcutAvailable $script:installRequest
    if ($script:installRequest.QuickRollbackEligible) {
        $script:finishDetails.Text = 'Existing Launcher profile and sign-in startup settings were preserved.'
        Show-WizardPage 5
        return
    }
    try {
        # Stopped instances have now saved their final runtime adjustments.
        # Re-read those bytes before adopting a fresh portable profile.
        $profile = Get-LauncherPortableProfilePlan -PackageDirectory $script:packageDirectory `
            -UserDataDirectory $script:userDataDirectory -Action $script:installRequest.Action
        $profileResult = Invoke-LauncherPortableProfileTransfer -Plan $profile
        $script:finishDetails.Text = $profileResult
        if ($profile.Warning) { $script:finishLabel.Text += "`r`n`r`n" + $profile.Warning }
        # Working examples belong to the initiating profile before its starter
        # links can be offered for launch. The installed reference stays read-only.
        $demoResult = Initialize-LauncherInstalledDemo `
            -TemplatePath (Join-Path $script:packageDirectory 'examples\demo_many_colors.toml') `
            -UserDataDirectory $script:userDataDirectory
        $script:finishDetails.Text += "`r`n`r`n" + $demoResult
        $iconResult = Initialize-LauncherInstalledDemoIcon -PackageDirectory $script:packageDirectory -UserDataDirectory $script:userDataDirectory
        $script:finishDetails.Text += "`r`n`r`n" + $iconResult
        # Adopt a portable configuration first. Otherwise seed only a missing
        # default for the initiating user, never the elevated worker's account.
        $configurationResult = Initialize-LauncherInstalledConfiguration `
            -TemplatePath (Join-Path $script:installRequest.DestinationDirectory 'Launcher.installed.default.toml') `
            -UserDataDirectory $script:userDataDirectory
        $script:finishDetails.Text += "`r`n`r`n" + $configurationResult
        $linksResult = Update-LauncherInstalledStarterLinks -UserDataDirectory $script:userDataDirectory
        $script:finishDetails.Text += "`r`n`r`n" + $linksResult
    } catch {
        $script:launchCheck.Checked = $false
        $script:finishLabel.Text += "`r`n`r`nThe application files were installed, but your configuration could not be prepared:`r`n" + $_.Exception.Message
    }
    try {
        $null = Set-LauncherStartupOption -Discovery $script:reviewedDiscovery `
            -Destination $script:installRequest.DestinationDirectory -StartOnSignIn $script:startupCheck.Checked
    } catch {
        $script:finishLabel.Text += "`r`n`r`nThe application files were installed, but the Startup option could not be applied:`r`n" + $_.Exception.Message
    }
    Show-WizardPage 5
}

function Update-InstallProgress {
    if ($null -eq $script:worker) { return }
    try {
        # Sample exit first: a Running receipt read just before exit must not be
        # mistaken for the final receipt. The next tick reads after actual exit.
        $exited = $script:worker.HasExited
        $resultPath = Join-Path $script:workerDirectory 'result.json'
        $result = $null
        if ([IO.File]::Exists($resultPath)) {
            $stream = [IO.File]::Open($resultPath, [IO.FileMode]::Open, [IO.FileAccess]::Read,
                [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
            $reader = [IO.StreamReader]::new($stream)
            try { $result = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
            $script:progressLabel.Text = $result.Message
            $script:progressBar.Value = [Math]::Max(0, [Math]::Min(100, [int]$result.Progress))
        }
        if (-not $exited) { return }
        if ($null -eq $result -and $script:workerElevated -and $script:worker.ExitCode -eq 2) {
            throw 'Launcher Setup could not verify or start the installation helper. Extract the complete download again and retry.'
        }
        if ($null -eq $result -or $result.Status -eq 'Running') { throw 'The installation helper exited without a completion report. No further changes will be attempted.' }
        switch ($result.Status) {
            'Succeeded' { Complete-Installation $result }
            'PermissionDenied' {
                if ($script:installRequest.Scope -eq 'AllUsers' -and -not $script:workerElevated) { Start-InstallWorker -Elevated }
                else { Show-PermissionFallback $result.Message }
            }
            default {
                $recoveryErrors = @(Restore-LauncherInstallationInstances -Instances $script:installRequest.RestartInstances `
                    -StoppedProcessIds @($result.StoppedProcessIds))
                $message = $result.Message
                if ($recoveryErrors.Count) {
                    $script:retainWorkerFiles = $true
                    $message += "`r`n`r`n" + ($recoveryErrors -join "`r`n") +
                        "`r`nRestart information was retained in: " + $script:workerDirectory
                }
                Show-InstallFailure $message
            }
        }
    } catch { Show-InstallFailure $_.Exception.Message }
}

$script:form = [LauncherSetup.DpiForm]::new()
$script:form.RenderWithoutActivation = -not [string]::IsNullOrWhiteSpace($RenderDirectory)
$script:form.Text = 'Launcher Setup'
$script:form.StartPosition = 'CenterScreen'
$script:form.AutoScaleMode = 'None'
$script:form.Font = [System.Drawing.Font]::new('Segoe UI', [single](10 * 96 / 72),
    [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$script:form.ClientSize = [System.Drawing.Size]::new(1000, 740)
$script:form.MinimumSize = [System.Drawing.Size]::new(860, 650)
$script:form.MaximizeBox = $true

$script:artworkPath = Get-LauncherArtworkPath -PackageDirectory $script:packageDirectory
$script:artworkIcon = Get-LauncherResourceIcon -ExecutablePath $script:artworkPath -Size 256
$script:artworkBitmap = $script:artworkIcon.ToBitmap()
$script:form.Icon = $script:artworkIcon
$script:form.TaskbarIconResource = $script:artworkPath + ',-101'
$shell = [System.Windows.Forms.TableLayoutPanel]::new()
$shell.Dock = 'Fill'
$shell.ColumnCount = 2
$shell.RowCount = 1
$shell.Margin = [System.Windows.Forms.Padding]::new(0)
$script:artworkColumn = [System.Windows.Forms.ColumnStyle]::new([System.Windows.Forms.SizeType]::Absolute, 200)
[void]$shell.ColumnStyles.Add($script:artworkColumn)
[void]$shell.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new([System.Windows.Forms.SizeType]::Percent, 100))
[void]$shell.RowStyles.Add([System.Windows.Forms.RowStyle]::new([System.Windows.Forms.SizeType]::Percent, 100))
$script:form.Controls.Add($shell)
$artPanel = [System.Windows.Forms.Panel]::new()
$artPanel.Dock = 'Fill'
$artPanel.Margin = [System.Windows.Forms.Padding]::new(0)
$artPanel.BackColor = [System.Drawing.Color]::FromArgb(235, 243, 250)
$shell.Controls.Add($artPanel, 0, 0)
$script:artworkBox = [System.Windows.Forms.PictureBox]::new()
$script:artworkBox.Image = $script:artworkBitmap
$script:artworkBox.SizeMode = 'Zoom'
$script:artworkBox.Size = [System.Drawing.Size]::new(160, 160)
$script:artworkBox.Location = [System.Drawing.Point]::new(20, 70)
$artPanel.Controls.Add($script:artworkBox)
$brand = [System.Windows.Forms.Label]::new()
$brand.Text = $publisher + ' Launcher'
$brand.TextAlign = 'TopCenter'
$brand.AutoSize = $false
$brand.Location = [System.Drawing.Point]::new(12, 248)
$brand.Size = [System.Drawing.Size]::new(176, 70)
$brand.Font = [System.Drawing.Font]::new('Segoe UI', 20,
    [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$artPanel.Controls.Add($brand)
$script:form.Add_LayoutDpiChanged({
    # TableLayout absolute styles are independent of Control.Scale. Artwork is
    # capped at its embedded 256px resolution rather than magnifying a tiny icon.
    $factor = $script:form.LayoutDpi / 96.0
    $script:artworkColumn.Width = [single](200 * $factor)
    $side = [int][Math]::Min(256, 160 * $factor)
    $script:artworkBox.Size = [System.Drawing.Size]::new($side, $side)
    $script:artworkBox.Left = [int](($script:artworkColumn.Width - $side) / 2)
})

$root = [System.Windows.Forms.TableLayoutPanel]::new()
$root.Dock = 'Fill'
$root.Padding = [System.Windows.Forms.Padding]::new(24)
$root.ColumnCount = 1
$root.RowCount = 4
[void]$root.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new(
    [System.Windows.Forms.SizeType]::Percent, 100))
foreach ($size in @('AutoSize', 'AutoSize', 'Percent', 'AutoSize')) {
    $style = [System.Windows.Forms.RowStyle]::new([System.Windows.Forms.SizeType]::$size)
    if ($size -eq 'Percent') { $style.Height = 100 }
    [void]$root.RowStyles.Add($style)
}
$shell.Controls.Add($root, 1, 0)

$banner = [System.Windows.Forms.Label]::new()
$banner.AutoSize = $true
$banner.Dock = 'Fill'
$banner.Padding = [System.Windows.Forms.Padding]::new(12)
$banner.Margin = [System.Windows.Forms.Padding]::new(0, 0, 0, 20)
$banner.BackColor = [System.Drawing.Color]::FromArgb(255, 244, 204)
$banner.ForeColor = [System.Drawing.Color]::FromArgb(81, 61, 0)
$banner.Text = 'Install or upgrade Launcher'
$root.Controls.Add($banner, 0, 0)

$heading = [System.Windows.Forms.TableLayoutPanel]::new()
$heading.AutoSize = $true
$heading.Dock = 'Fill'
$heading.ColumnCount = 1
$heading.RowCount = 2
$heading.Margin = [System.Windows.Forms.Padding]::new(0, 0, 0, 12)
[void]$heading.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new(
    [System.Windows.Forms.SizeType]::Percent, 100))
$script:titleLabel = [System.Windows.Forms.Label]::new()
$script:titleLabel.AutoSize = $true
$script:titleLabel.Dock = 'Fill'
$script:titleLabel.Font = [System.Drawing.Font]::new('Segoe UI', 24,
    [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$script:titleLabel.Margin = [System.Windows.Forms.Padding]::new(0)
$script:descriptionLabel = [System.Windows.Forms.Label]::new()
$script:descriptionLabel.AutoSize = $true
$script:descriptionLabel.Dock = 'Fill'
$script:descriptionLabel.Margin = [System.Windows.Forms.Padding]::new(0, 8, 0, 0)
$heading.Controls.Add($script:titleLabel, 0, 0)
$heading.Controls.Add($script:descriptionLabel, 0, 1)
$root.Controls.Add($heading, 0, 1)

$script:pageHost = [System.Windows.Forms.Panel]::new()
$script:pageHost.Dock = 'Fill'
$script:pageHost.Margin = [System.Windows.Forms.Padding]::new(0)
$root.Controls.Add($script:pageHost, 0, 2)

$footer = [System.Windows.Forms.TableLayoutPanel]::new()
$footer.AutoSize = $true
$footer.Dock = 'Fill'
$footer.ColumnCount = 4
$footer.RowCount = 1
$footer.Margin = [System.Windows.Forms.Padding]::new(0, 20, 0, 0)
[void]$footer.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new(
    [System.Windows.Forms.SizeType]::Percent, 100))
for ($i = 0; $i -lt 3; $i++) {
    [void]$footer.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new(
        [System.Windows.Forms.SizeType]::AutoSize))
}
$script:stepLabel = [System.Windows.Forms.Label]::new()
$script:stepLabel.AutoSize = $true
$script:stepLabel.Anchor = 'Left'
$script:stepLabel.Margin = [System.Windows.Forms.Padding]::new(0)
$script:stepLabel.ForeColor = [System.Drawing.SystemColors]::GrayText
$footer.Controls.Add($script:stepLabel, 0, 0)
$script:backButton = [System.Windows.Forms.Button]::new()
$script:nextButton = [System.Windows.Forms.Button]::new()
$script:cancelButton = [System.Windows.Forms.Button]::new()
$buttons = @($script:backButton, $script:nextButton, $script:cancelButton)
for ($i = 0; $i -lt $buttons.Count; $i++) {
    $buttons[$i].AutoSize = $true
    $buttons[$i].MinimumSize = [System.Drawing.Size]::new(100, 36)
    $buttons[$i].Margin = [System.Windows.Forms.Padding]::new(8, 0, 0, 0)
    $buttons[$i].TabIndex = $i
    $footer.Controls.Add($buttons[$i], ($i + 1), 0)
}
$script:backButton.Text = '< &Back'
$script:cancelButton.Text = 'Cancel'
$script:cancelButton.DialogResult = 'Cancel'
$script:form.AcceptButton = $script:nextButton
$script:form.CancelButton = $script:cancelButton
$root.Controls.Add($footer, 0, 3)

$welcome = New-WizardPage 'Welcome to Launcher setup' 'Choose how Launcher should be installed on this computer.'
[void](Add-PageText $welcome.Layout 'Setup checks for existing installations and running copies before you choose the application folder and startup option.')
[void](Add-PageText $welcome.Layout 'You can also use Launcher directly from the extracted ZIP folder without installing it. The included documentation explains portable use.')
$script:findingsLabel = Add-PageText $welcome.Layout 'Checking this computer...'
$discoveryActions = [System.Windows.Forms.FlowLayoutPanel]::new()
$discoveryActions.AutoSize = $true
$discoveryActions.Dock = 'Fill'
$discoveryActions.Margin = [System.Windows.Forms.Padding]::new(0, 0, 0, 12)
$script:refreshButton = [System.Windows.Forms.Button]::new()
$script:refreshButton.Text = '&Refresh findings'
$script:refreshButton.AutoSize = $true
$script:refreshButton.MinimumSize = [System.Drawing.Size]::new(130, 34)
$script:refreshButton.Margin = [System.Windows.Forms.Padding]::new(0, 0, 16, 0)
$discoveryActions.Controls.Add($script:refreshButton)
$script:detailsToggle = [System.Windows.Forms.CheckBox]::new()
$script:detailsToggle.Text = 'Show &discovery details'
$script:detailsToggle.AutoSize = $true
$script:detailsToggle.Margin = [System.Windows.Forms.Padding]::new(0, 8, 0, 0)
$discoveryActions.Controls.Add($script:detailsToggle)
Add-PageControl $welcome.Layout $discoveryActions
$script:discoveryDetails = [System.Windows.Forms.TextBox]::new()
$script:discoveryDetails.Multiline = $true
$script:discoveryDetails.ReadOnly = $true
$script:discoveryDetails.WordWrap = $true
$script:discoveryDetails.ScrollBars = 'Vertical'
$script:discoveryDetails.Height = 210
$script:discoveryDetails.Dock = 'Fill'
$script:discoveryDetails.Margin = [System.Windows.Forms.Padding]::new(0, 0, 0, 12)
$script:discoveryDetails.Visible = $false
Add-PageControl $welcome.Layout $script:discoveryDetails
[void](Add-PageText $welcome.Layout 'Your configuration, saved adjustments, and edited palettes are preserved during upgrades.' -Muted)
$welcome.FirstControl = $script:refreshButton

$scope = New-WizardPage 'Who will use Launcher?' 'Choose an installation for your account or for everyone on this computer.'
$script:allUsersRadio = [System.Windows.Forms.RadioButton]::new()
$script:allUsersRadio.Text = '&All users (recommended)'
$script:allUsersRadio.AutoSize = $true
$script:allUsersRadio.Checked = $true
$script:allUsersRadio.Margin = [System.Windows.Forms.Padding]::new(0, 0, 0, 6)
Add-PageControl $scope.Layout $script:allUsersRadio
[void](Add-PageText $scope.Layout 'Install in Program Files. Administrator approval may be required.')
$script:justMeRadio = [System.Windows.Forms.RadioButton]::new()
$script:justMeRadio.Text = '&Just me'
$script:justMeRadio.AutoSize = $true
$script:justMeRadio.Margin = [System.Windows.Forms.Padding]::new(0, 0, 0, 6)
Add-PageControl $scope.Layout $script:justMeRadio
[void](Add-PageText $scope.Layout 'A personal installation uses your local application folder and does not require administrator approval.')
$script:scopeNote = Add-PageText $scope.Layout '' -Muted
$scope.FirstControl = $script:allUsersRadio

$destinationLabel = Add-PageText $scope.Layout 'Application folder:'
$destinationLabel.Margin = [System.Windows.Forms.Padding]::new(0, 0, 0, 4)
$script:destinationBox = [System.Windows.Forms.TextBox]::new()
$script:destinationBox.ReadOnly = $true
$script:destinationBox.Multiline = $true
$script:destinationBox.WordWrap = $true
$script:destinationBox.ScrollBars = 'Vertical'
$script:destinationBox.Height = 64
$script:destinationBox.Dock = 'Fill'
$script:destinationBox.Margin = [System.Windows.Forms.Padding]::new(0, 0, 0, 12)
Add-PageControl $scope.Layout $script:destinationBox
$script:destinationNote = Add-PageText $scope.Layout '' -Muted

$startupPage = New-WizardPage 'Startup option' 'Choose whether Launcher starts automatically when you sign in.'
$script:startupCheck = [System.Windows.Forms.CheckBox]::new()
$script:startupCheck.Text = '&Start Launcher when I sign in'
$script:startupCheck.AutoSize = $true
$script:startupCheck.Margin = [System.Windows.Forms.Padding]::new(0, 8, 0, 16)
Add-PageControl $startupPage.Layout $script:startupCheck
$script:startupNote = Add-PageText $startupPage.Layout ''
[void](Add-PageText $startupPage.Layout 'This option applies to your account, including when the program is installed for all users.' -Muted)
$startupPage.FirstControl = $script:startupCheck

$review = New-WizardPage 'Review your choices' 'Check the portable files, application folder and Startup shortcut action before continuing.'
$script:reviewBox = [System.Windows.Forms.TextBox]::new()
$script:reviewBox.Multiline = $true
$script:reviewBox.ReadOnly = $true
$script:reviewBox.WordWrap = $true
$script:reviewBox.ScrollBars = 'Vertical'
$script:reviewBox.Dock = 'Fill'
$script:reviewBox.Height = 360
$script:reviewBox.Margin = [System.Windows.Forms.Padding]::new(0, 0, 0, 16)
Add-PageControl $review.Layout $script:reviewBox
[void](Add-PageText $review.Layout 'Setup will close affected Launcher copies after validating the package and folder permissions. Other installations will be left running.' -Muted)

$progress = New-WizardPage 'Installing Launcher' 'Please wait while setup prepares and updates the application files.'
$script:progressLabel = Add-PageText $progress.Layout 'Validating the package...'
$script:progressBar = [System.Windows.Forms.ProgressBar]::new()
$script:progressBar.Dock = 'Fill'
$script:progressBar.Height = 28
$script:progressBar.Style = 'Continuous'
$script:progressBar.Margin = [System.Windows.Forms.Padding]::new(0, 0, 0, 16)
Add-PageControl $progress.Layout $script:progressBar

$finish = New-WizardPage 'Launcher is ready'
$script:finishLabel = Add-PageText $finish.Layout ''
$script:finishLabel.Margin = [System.Windows.Forms.Padding]::new(0, 0, 0, 8)
$script:launchCheck = [System.Windows.Forms.CheckBox]::new()
$script:launchCheck.Text = '&Start Launcher now'
$script:launchCheck.AutoSize = $true
$script:launchCheck.Checked = $true
Add-PageControl $finish.Layout $script:launchCheck
$script:launchHint = [System.Windows.Forms.TableLayoutPanel]::new()
$script:launchHint.AutoSize = $true
$script:launchHint.AutoSizeMode = 'GrowAndShrink'
$script:launchHint.Dock = 'Top'
$script:launchHint.ColumnCount = 1
$script:launchHint.Margin = [System.Windows.Forms.Padding]::new(0, 10, 0, 0)
[void]$script:launchHint.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new(
    [System.Windows.Forms.SizeType]::Percent, 100))
$lookUp = Add-PageText $script:launchHint 'Look up'
$lookUp.Font = [System.Drawing.Font]::new($script:form.Font.FontFamily, 20,
    [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
$lookUp.Margin = [System.Windows.Forms.Padding]::new(0, 0, 0, 8)
$launchInstructions = Add-PageText $script:launchHint 'After you select Finish, look for Launcher''s tabs at the top of your screen. Point to a tab to open Launcher.'
$launchInstructions.Margin = [System.Windows.Forms.Padding]::new(0, 0, 0, 8)
$tabsIllustration = [LauncherSetup.TopTabsIllustration]::new()
$tabsIllustration.Dock = 'Fill'
$tabsIllustration.Margin = [System.Windows.Forms.Padding]::new(0, 0, 0, 8)
Add-PageControl $script:launchHint $tabsIllustration
Add-PageControl $finish.Layout $script:launchHint
$script:launchCheck.Add_CheckedChanged({ $script:launchHint.Visible = $script:launchCheck.Checked })
$script:recoveryHint = [System.Windows.Forms.TableLayoutPanel]::new()
$script:recoveryHint.AutoSize = $true
$script:recoveryHint.AutoSizeMode = 'GrowAndShrink'
$script:recoveryHint.Dock = 'Top'
$script:recoveryHint.ColumnCount = 2
$script:recoveryHint.RowCount = 1
$script:recoveryHint.Padding = [System.Windows.Forms.Padding]::new(8, 4, 12, 4)
$script:recoveryHint.Margin = [System.Windows.Forms.Padding]::new(0, 4, 0, 12)
$script:recoveryHint.Visible = $false
if ([System.Windows.Forms.SystemInformation]::HighContrast) {
    $script:recoveryHint.BackColor = [System.Drawing.SystemColors]::Window
    $script:recoveryHint.ForeColor = [System.Drawing.SystemColors]::WindowText
} else {
    $script:recoveryHint.BackColor = [System.Drawing.Color]::FromArgb(232, 244, 255)
    $script:recoveryHint.ForeColor = [System.Drawing.Color]::FromArgb(30, 76, 121)
}
[void]$script:recoveryHint.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new(
    [System.Windows.Forms.SizeType]::AutoSize))
[void]$script:recoveryHint.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new(
    [System.Windows.Forms.SizeType]::Percent, 100))
$recoveryArrow = [LauncherSetup.RecoveryArrow]::new()
$recoveryArrow.Anchor = 'Top, Left'
$recoveryArrow.Margin = [System.Windows.Forms.Padding]::new(0, 0, 8, 0)
$script:recoveryHint.Controls.Add($recoveryArrow, 0, 0)
$recoveryText = [System.Windows.Forms.TableLayoutPanel]::new()
$recoveryText.AutoSize = $true
$recoveryText.AutoSizeMode = 'GrowAndShrink'
$recoveryText.Dock = 'Fill'
$recoveryText.ColumnCount = 1
$recoveryText.Margin = [System.Windows.Forms.Padding]::new(0, 4, 0, 4)
[void]$recoveryText.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new(
    [System.Windows.Forms.SizeType]::Percent, 100))
$searchStart = Add-PageText $recoveryText 'To review or roll back an update, search Start for'
$searchStart.Margin = [System.Windows.Forms.Padding]::new(0, 0, 0, 3)
$recoveryTitle = Add-PageText $recoveryText 'Launcher Update History and Recovery'
$recoveryTitle.Font = [System.Drawing.Font]::new($script:form.Font.FontFamily, 17,
    [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
$recoveryTitle.Margin = [System.Windows.Forms.Padding]::new(0)
$script:recoveryHint.Controls.Add($recoveryText, 1, 0)
Add-PageControl $finish.Layout $script:recoveryHint
$finishGuide = Add-PageText $finish.Layout 'Keep the extracted folder if your profile was left there or still uses files there. See docs\Installer.md for repair, recovery and removal.' -Muted
$finishGuide.Margin = [System.Windows.Forms.Padding]::new(0, 0, 0, 8)
$script:finishDetailsToggle = [System.Windows.Forms.CheckBox]::new()
$script:finishDetailsToggle.Text = 'Show installation &details'
$script:finishDetailsToggle.AutoSize = $true
$script:finishDetailsToggle.Margin = [System.Windows.Forms.Padding]::new(0, 0, 0, 8)
$script:finishDetailsToggle.Visible = $false
Add-PageControl $finish.Layout $script:finishDetailsToggle
$script:finishDetails = Add-PageText $finish.Layout ''
$script:finishDetails.Visible = $false
$script:finishDetailsToggle.Add_CheckedChanged({ $script:finishDetails.Visible = $script:finishDetailsToggle.Checked })
$script:finishDetails.Add_TextChanged({ $script:finishDetailsToggle.Visible = [bool]$script:finishDetails.Text })
$finish.FirstControl = $script:launchCheck

$script:progressTimer = [System.Windows.Forms.Timer]::new()
$script:progressTimer.Interval = 200
$script:progressTimer.Add_Tick({ Update-InstallProgress })
$script:justMeRadio.Add_CheckedChanged({ Update-InstallationChoices })
$script:allUsersRadio.Add_CheckedChanged({ Update-InstallationChoices })
$script:startupCheck.Add_CheckedChanged({
    if ($script:updatingStartupCheck -or $script:applyingDiscovery -or $null -eq $script:discovery) { return }
    $startupKey = (Get-SelectedDestination).TrimEnd('\').ToLowerInvariant()
    $script:startupChoices[$startupKey] = $script:startupCheck.Checked
    Update-InstallationChoices
})
$script:detailsToggle.Add_CheckedChanged({
    $script:discoveryDetails.Visible = $script:detailsToggle.Checked
})
$script:refreshButton.Add_Click({ Refresh-Discovery })
$script:backButton.Add_Click({ Show-WizardPage ($script:pageIndex - 1) })
$script:nextButton.Add_Click({
    if ($script:installing) { return }
    if ($script:pageIndex -eq 5) {
        if ($script:installedExecutable) {
            try {
                [string[]]$restartArguments = @()
                if ($script:launchCheck.Checked) {
                    $restartArguments = @($script:restartQueue | ForEach-Object { $_.Arguments })
                }
                $handedOff = Write-LauncherUpdaterStartupPlan -Request $script:installRequest `
                    -Deployment $script:installedDeployment -RestartArguments $restartArguments
                if (!$handedOff -and $script:launchCheck.Checked) {
                    # Remove each successful start before the next one: retrying Finish
                    # after a later failure must not duplicate already restored docks.
                    while ($script:restartQueue.Count) {
                        Start-LauncherInstallationInstance -Executable $script:installedExecutable -Arguments $script:restartQueue[0].Arguments
                        $script:restartQueue.RemoveAt(0)
                    }
                }
            } catch {
                [void][System.Windows.Forms.MessageBox]::Show($script:form, $_.Exception.Message, 'Cannot finish Launcher Setup', 'OK', 'Error')
                return
            }
        }
        $script:form.Close()
    } elseif ($script:pageIndex -eq 3) {
        Start-Installation
    } else {
        Update-InstallationChoices
        Show-WizardPage ($script:pageIndex + 1)
    }
})
$script:cancelButton.Add_Click({ $script:form.Close() })
$script:form.Add_FormClosing({
    param($sender, $event)
    if ($script:installing) { $event.Cancel = $true; return }
    $script:progressTimer.Stop()
})

try {
    if ($ValidateUi) {
        # As with discovery, validate the review using synthetic profiles. Do
        # not depend on or inspect the desktop user's actual AppData contents.
        $script:validationProfileError = $false
        function Get-LauncherPortableProfilePlan {
            param([string]$PackageDirectory, [string]$UserDataDirectory, [string]$Action)
            if ($script:validationProfileError) { throw 'Fixture profile cannot be read: Icons\custom.svg' }
            if ($Action -eq 'Upgrade') {
                return [pscustomobject]@{
                    Summary = 'Upgrade preserves the installed profile. Portable profile files are not copied.'
                    Warning = "WARNING: Edited or added portable profile files are NOT copied during upgrades.`r`n  Launcher.toml`r`n  Icons\custom.svg"
                }
            }
            return [pscustomobject]@{
                Summary = "Portable profile files to copy into $UserDataDirectory`:`r`n  Launcher.toml`r`n  Icons\custom.svg"
                Warning = ''
            }
        }
        Apply-Discovery (New-ValidationDiscovery)
    }
    else { Refresh-Discovery }
    Show-WizardPage 0
    if ($ValidateUi) {
        if (-not $script:allUsersRadio.Checked -or $script:justMeRadio.Checked -or
            $script:destinationBox.Text -ne $script:machineDestination) {
            throw 'A fresh installation did not default to All users in Program Files.'
        }
        # Construct and lay out every page without opening a window or using a timer.
        [void]$script:form.Handle
        if (!$script:form.HasSetupTaskbarIdentity()) {
            throw 'Windows did not retain the Setup window taskbar identity and icon resource.'
        }
        for ($i = 0; $i -lt $script:pages.Count; $i++) {
            Show-WizardPage $i
            $script:form.PerformLayout()
            if ($script:titleLabel.Text -ne $script:pages[$i].Title) {
                throw "The title for page $i did not update."
            }
        }
        $script:allUsersRadio.Checked = $true
        if ($script:destinationBox.Text -ne $script:machineDestination) {
            throw 'The all-users destination did not update.'
        }
        $script:justMeRadio.Checked = $true
        $script:startupCheck.Checked = $true
        if ($script:destinationBox.Text -ne $script:localDestination -or
            $script:reviewBox.Text -notmatch 'Start Launcher when I sign in: Yes') {
            throw 'The personal installation choices did not update.'
        }
        if ($script:reviewBox.Lines.Count -lt 9) {
            throw 'The review summary did not retain its line breaks.'
        }
        if ([string]::IsNullOrWhiteSpace($script:packageDirectory) -or
            -not $script:reviewBox.Text.Contains($script:packageDirectory)) {
            throw 'The review summary did not retain the package directory.'
        }
        foreach ($installedScope in @('AllUsers', 'CurrentUser')) {
            Apply-Discovery (New-ValidationDiscovery -InstalledScopes @($installedScope))
            $expected = if ($installedScope -eq 'AllUsers') { $script:machineDestination } else { $script:localDestination }
            if ((Get-SelectedAction) -ne 'Upgrade' -or (Get-SelectedDestination) -ne $expected -or
                $script:allUsersRadio.Enabled -eq $script:justMeRadio.Enabled -or
                $script:pages[1].Title -ne 'Upgrade Launcher') {
                throw "A single $installedScope installation did not force an in-place upgrade."
            }
            if ($script:reviewBox.Text.Contains('administrator approval for Launcher Setup') -ne ($installedScope -eq 'AllUsers')) {
                throw "The review did not accurately describe the administrator prompt for $installedScope."
            }
        }
        Apply-Discovery (New-ValidationDiscovery -InstalledScopes @('AllUsers', 'CurrentUser'))
        if ($script:allUsersRadio.Enabled -or -not $script:justMeRadio.Enabled -or
            -not $script:justMeRadio.Checked -or (Get-SelectedDestination) -ne $script:localDestination) {
            throw 'Two existing installations did not select the personal installation for upgrade.'
        }
        if ((Get-SelectedAction) -ne 'Upgrade' -or -not $script:reviewBox.Text.Contains('Action: Upgrade')) {
            throw 'The selected existing installation was not marked Upgrade in the review.'
        }
        if (!$script:reviewBox.Text.Contains('WARNING:') -or
            !$script:reviewBox.Text.Contains('NOT copied during upgrades') -or
            !$script:reviewBox.Text.Contains('Icons\custom.svg')) {
            throw 'Upgrade review did not warn about uncopied portable files and identify the icons.'
        }
        $script:validationProfileError = $true
        Update-InstallationChoices
        Show-WizardPage 3
        if ($script:nextButton.Enabled -or !$script:reviewBox.Text.Contains('Fixture profile cannot be read')) {
            throw 'A portable-file inspection failure did not block installation visibly.'
        }
        $script:validationProfileError = $false
        Update-InstallationChoices
        if (-not $script:discoveryDetails.Text.Contains('Selected destination: ' + $script:localDestination) -or
            -not $script:discoveryDetails.Text.Contains([string][char]0x2191 + ' Targeted upgrade | ' + (Join-Path $script:localDestination 'Launcher.exe')) -or
            -not $script:discoveryDetails.Text.Contains([string][char]0x2013 + ' Ignored other installation | ' + (Join-Path $script:machineDestination 'Launcher.exe'))) {
            throw 'The discovery symbols did not identify the selected personal upgrade and ignored machine installation.'
        }
        $script:startupChoices.Clear()
        $matchingStartup = New-ValidationDiscovery -InstalledScopes @('CurrentUser') -StartupScope 'CurrentUser'
        Apply-Discovery $matchingStartup
        if (-not $script:startupCheck.Checked -or $script:startupPlan.Action -ne 'Keep') {
            throw 'An existing Startup shortcut for the selected installation did not initialize checked.'
        }
        $script:startupCheck.Checked = $false
        if ($script:startupPlan.Action -ne 'Remove' -or
            -not $script:reviewBox.Text.Contains('Startup shortcut: Remove')) {
            throw 'Unchecking the selected installation Startup shortcut did not explicitly plan removal.'
        }
        Show-WizardPage 3
        Show-WizardPage 2
        Apply-Discovery $matchingStartup
        if ($script:startupCheck.Checked -or $script:startupPlan.Action -ne 'Remove') {
            throw 'Navigation or refreshed discovery discarded the explicit Startup choice.'
        }
        $script:startupChoices.Clear()
        Apply-Discovery (New-ValidationDiscovery -InstalledScopes @('AllUsers', 'CurrentUser') -StartupScope 'AllUsers')
        if ($script:startupCheck.Checked -or $script:startupPlan.Action -ne 'None' -or
            $script:startupPlan.Explanation -notmatch 'Keep the existing Startup shortcut') {
            throw 'An unchecked Startup shortcut for the other installation was not preserved.'
        }
        $script:startupCheck.Checked = $true
        if ($script:startupPlan.Action -ne 'Retarget' -or
            -not $script:reviewBox.Text.Contains('Startup shortcut: Retarget')) {
            throw 'Changing the other installation Startup shortcut was not disclosed as retargeting.'
        }
        $script:startupChoices.Clear()
        Apply-Discovery (New-ValidationDiscovery)
        $script:justMeRadio.Checked = $true
        if (!$script:startupCheck.Checked) { throw 'A NOT FOUND target did not default Startup to checked.' }
        $script:startupCheck.Checked = $false
        $script:allUsersRadio.Checked = $true
        if (!$script:startupCheck.Checked) { throw 'A disabled Startup choice leaked into another fresh destination.' }
        $script:justMeRadio.Checked = $true
        if ($script:startupCheck.Checked) { throw 'Changing scope discarded the personal Startup choice.' }
        Apply-Discovery (New-ValidationDiscovery)
        if (-not $script:justMeRadio.Checked -or $script:startupCheck.Checked) {
            throw 'Refreshing discovery discarded the fresh-install scope or its explicit Startup choice.'
        }
        $script:startupChoices.Clear()
        $unknownStartup = New-ValidationDiscovery -InstalledScopes @('CurrentUser')
        $unknownStartup.Startup.State = 'Unknown'
        $unknownStartup.Startup.Exists = $true
        Apply-Discovery $unknownStartup
        $script:startupCheck.Checked = $true
        Show-WizardPage 2
        if ($script:startupPlan.Action -ne 'Review' -or $script:nextButton.Enabled) {
            throw 'An unidentified Startup shortcut did not require review before replacement.'
        }
        $script:startupCheck.Checked = $false
        if ($script:startupPlan.Action -ne 'None' -or -not $script:nextButton.Enabled) {
            throw 'Preserving an unidentified Startup shortcut did not allow the preview to continue.'
        }
        Apply-Discovery (New-ValidationDiscovery -BlockedScopes @('AllUsers', 'CurrentUser'))
        Show-WizardPage 1
        if ($script:allUsersRadio.Enabled -or $script:justMeRadio.Enabled -or $script:nextButton.Enabled) {
            throw 'Unverified application folders were offered as automatic installation destinations.'
        }
        $script:startupChoices.Clear()
        Apply-Discovery (New-ValidationDiscovery)
        $unavailableProcesses = New-ValidationDiscovery
        $unavailableProcesses | Add-Member -NotePropertyName ProcessEnumerationSucceeded -NotePropertyValue $false
        $unavailableProcesses.Issues = @('Cannot enumerate running Launcher candidates: validation failure.')
        Apply-Discovery $unavailableProcesses
        if (-not $script:findingsLabel.Text.Contains('Unable to check running Launcher / LauncherDrop copies.') -or
            $script:findingsLabel.Text.Contains('0 verified running') -or
            -not $script:discoveryDetails.Text.Contains('? Cannot enumerate running Launcher candidates: validation failure.')) {
            throw 'An unavailable process inventory must be explicit and marked for review, without implying zero running copies.'
        }
        Apply-Discovery (New-ValidationDiscovery)
        if (-not [LauncherSetup.NativePresentation]::AreDpiAwarenessContextsEqual(
            [LauncherSetup.NativePresentation]::GetWindowDpiAwarenessContext($script:form.Handle), [IntPtr]::new(-4))) {
            throw 'The setup window is not per-monitor DPI aware.'
        }
        if ($script:artworkBitmap.Width -lt 256 -or $script:artworkBitmap.Height -lt 256) {
            throw 'The setup artwork did not use the full-size embedded icon.'
        }
        $nativeDpi = [int][LauncherSetup.NativePresentation]::GetDpiForWindow($script:form.Handle)
        $script:form.SetRenderDpi(192)
        $area = [System.Windows.Forms.Screen]::FromHandle($script:form.Handle).WorkingArea
        if ([Math]::Abs($script:form.Font.Size - (10 * 192 / 72)) -gt 0.05 -or
            $script:artworkBox.Width -gt 256 -or $script:form.Width -gt $area.Width -or
            $script:form.Height -gt $area.Height) {
            throw 'The 200% DPI layout did not scale fonts, preserve artwork resolution, or fit the display.'
        }
        $script:form.SetRenderDpi($nativeDpi)
        Write-Output ('Validated {0} wizard pages, installation selection, discovery symbols, Startup keep/remove/retarget/review choices, embedded artwork and native DPI awareness. No window was shown and no installation changes were made.' -f $script:pages.Count)
    }
    if ($RenderDirectory) {
        if (-not [System.IO.Path]::IsPathRooted($RenderDirectory)) {
            throw 'RenderDirectory must be an absolute folder path.'
        }
        [void][System.IO.Directory]::CreateDirectory($RenderDirectory)
        # WinForms omits hidden descendants from DrawToBitmap. Create their
        # native controls in a fully transparent window, then render
        # pixels directly; the preview never becomes visible on the desktop.
        $script:form.ShowInTaskbar = $false
        $script:form.Opacity = 0
        $script:form.Show()
        [System.Windows.Forms.Application]::DoEvents()
        if ($RenderDetails) { $script:detailsToggle.Checked = $true }
        if ($RenderDpi -gt 0) { $script:form.SetRenderDpi($RenderDpi) }
        $rendered = @()
        for ($i = 0; $i -lt $script:pages.Count; $i++) {
            if ($ValidateUi -and $i -eq 5) {
                # Setup completion can be dispatched by a DPI-unaware timer.
                $previous = [LauncherSetup.NativePresentation]::SetThreadDpiAwarenessContext([IntPtr]::new(-1))
                if ($previous -eq [IntPtr]::Zero) { throw 'Could not simulate a DPI-unaware Setup callback.' }
                try { Show-WizardPage $i } finally { Restore-LauncherPresentation $previous }
                if ($script:recoveryHint.Visible) { throw 'Finish promoted a Recovery shortcut before its availability was confirmed.' }
                # Render a successful install with the scoped Recovery shortcut available.
                $script:finishLabel.Text = "Launcher is installed in:`r`n" + $script:machineDestination
                $script:finishDetails.Text = "The existing Launcher configuration was preserved.`r`n`r`nThe installed examples are ready to use."
                if (!$script:finishDetailsToggle.Visible -or $script:finishDetails.Visible) {
                    throw 'Routine installation details should be available and collapsed on Finish.'
                }
                $script:finishDetailsToggle.Checked = $true
                if (!$script:finishDetails.Visible) { throw 'Installation details did not expand on Finish.' }
                $script:finishDetailsToggle.Checked = $RenderDetails
                $script:recoveryHint.Visible = $true
                $script:launchCheck.Checked = $false
                if (!$script:recoveryHint.Visible -or $script:launchHint.Visible) {
                    throw 'Declining to start Launcher must leave the Recovery hint visible on Finish.'
                }
                $script:launchCheck.Checked = $true
            } else { Show-WizardPage $i }
            $script:form.PerformLayout()
            $client = $script:form.RectangleToScreen($script:form.ClientRectangle)
            $next = $script:nextButton.RectangleToScreen($script:nextButton.ClientRectangle)
            if (!$client.Contains($next)) { throw "The navigation button for wizard page $($i + 1) is outside the window." }
            $bitmap = [System.Drawing.Bitmap]::new($script:form.Width, $script:form.Height)
            try {
                $script:form.DrawToBitmap($bitmap, [System.Drawing.Rectangle]::new(0, 0, $bitmap.Width, $bitmap.Height))
                $file = Join-Path $RenderDirectory ('page-{0}.png' -f ($i + 1))
                $bitmap.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
            } finally { $bitmap.Dispose() }
            $rendered += [pscustomobject]@{
                Page = $i + 1; Title = $script:pages[$i].Title; File = $file
                PageClientWidth = $script:pages[$i].Panel.ClientSize.Width
                PageContentWidth = $script:pages[$i].Layout.Width
                PageClientHeight = $script:pages[$i].Panel.ClientSize.Height
                PageContentHeight = $script:pages[$i].Layout.Height
            }
        }
        $details = [pscustomobject]@{
            NativeWindowDpi = [LauncherSetup.NativePresentation]::GetDpiForWindow($script:form.Handle)
            PerMonitorV2 = [LauncherSetup.NativePresentation]::AreDpiAwarenessContextsEqual(
                [LauncherSetup.NativePresentation]::GetWindowDpiAwarenessContext($script:form.Handle), [IntPtr]::new(-4))
            LayoutDpi = $script:form.LayoutDpi; SimulatedLayoutDpi = $RenderDpi
            BodyFontPixels = $script:form.Font.Size
            SelectedDestination = Get-SelectedDestination
            Action = Get-SelectedAction
            StartupChecked = $script:startupCheck.Checked
            StartupPlanAction = $script:startupPlan.Action
            StartupPlanExplanation = $script:startupPlan.Explanation
            ArtworkSource = $script:artworkPath; EmbeddedArtworkPixels = $script:artworkBitmap.Width
            Pages = $rendered
        }
        $details | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $RenderDirectory 'dpi-layout.json') -Encoding UTF8
        $script:form.Hide()
        Write-Output ('Rendered {0} wizard pages to {1}.' -f $script:pages.Count, $RenderDirectory)
    } elseif (-not $ValidateUi) {
        [void]$script:form.ShowDialog()
    }
} finally {
    $script:progressTimer.Stop()
    $script:progressTimer.Dispose()
    $script:form.Dispose()
    $script:artworkBitmap.Dispose()
    $script:artworkIcon.Dispose()
}
} finally { Restore-LauncherPresentation $script:previousDpiContext }
