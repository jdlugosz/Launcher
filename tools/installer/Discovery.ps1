#requires -Version 5.1
# Read-only discovery shared by the installer and its focused checks.
# Metadata identifies this product for discovery; it is not an authenticity check.
. (Join-Path $PSScriptRoot 'Processes.ps1')

function Get-LauncherDiscoveryProperty {
    param($Object, [string]$Name)
    if ($null -ne $Object -and $null -ne $Object.PSObject.Properties[$Name]) {
        return $Object.$Name
    }
    return $null
}

function ConvertTo-LauncherDiscoveryPath {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    return [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar)
}

function Get-LauncherDistributionKind {
    param([Parameter(Mandatory)]$Metadata)
    # A release requires explicit package intent. Older binaries without this
    # field remain installable, but must never authorize deleting a fallback.
    switch -CaseSensitive ([string](Get-LauncherDiscoveryProperty $Metadata 'Comments')) {
        'Launcher distribution: Release' { return 'Release' }
        'Launcher distribution: Beta' { return 'Beta' }
        'Launcher distribution: Special' { return 'Special' }
        default { return 'Prerelease' }
    }
}

function Get-LauncherExactVersion {
    param([Parameter(Mandatory)]$Metadata, [switch]$RequireFixed)
    $partNames = @('MajorPart', 'MinorPart', 'BuildPart', 'PrivatePart')
    $prefix = 'Product'
    # Once file-version fields are present they are authoritative. Older metadata
    # providers may expose only the product version, which used to be the build.
    foreach ($name in @('Version') + $partNames) {
        if ($null -ne $Metadata.PSObject.Properties['File' + $name]) { $prefix = 'File'; break }
    }
    $text = [string](Get-LauncherDiscoveryProperty $Metadata ($prefix + 'Version'))
    $parsed = $null
    if ($text -cnotmatch '\A[0-9]+(?:\.[0-9]+){1,3}\z' -or ![version]::TryParse($text, [ref]$parsed)) {
        throw "The executable has no valid $prefix version label."
    }
    $parts = @($parsed.Major, $parsed.Minor, [Math]::Max(0, $parsed.Build), [Math]::Max(0, $parsed.Revision))
    $fixedParts = @($partNames | ForEach-Object { Get-LauncherDiscoveryProperty $Metadata ($prefix + $_) })
    $hasFixed = @($fixedParts | Where-Object { $null -ne $_ }).Count -ne 0
    for ($index = 0; $index -lt $parts.Count; ++$index) {
        $part = 0
        if ($parts[$index] -gt 65535 -or (($prefix -ceq 'File' -or $hasFixed -or $RequireFixed) -and
            ($null -eq $fixedParts[$index] -or ![int]::TryParse([string]$fixedParts[$index], [ref]$part) -or $part -ne $parts[$index]))) {
            throw "The executable's $prefix version label disagrees with its fixed version fields."
        }
    }
    return [version]::new($parts[0], $parts[1], $parts[2], $parts[3])
}

function Get-LauncherVersionLabel {
    param([Parameter(Mandatory)]$Metadata)
    $version = Get-LauncherExactVersion $Metadata
    $label = '{0}.{1}' -f $version.Major, $version.Minor
    if ($version.Build -or $version.Revision) { $label += '.' + $version.Build }
    if ($version.Revision) { $label += '.' + $version.Revision }
    return $label
}

function Get-LauncherPackageExecutable {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$PackageDirectory,
        [scriptblock]$MetadataReader = { param($Path) [Diagnostics.FileVersionInfo]::GetVersionInfo($Path) }
    )
    $package = [IO.Path]::GetFullPath($PackageDirectory)
    $candidates = @(Get-ChildItem -LiteralPath $package -File -Force -ErrorAction Stop | Where-Object {
        $_.Name -ieq 'Launcher.exe' -or $_.Name -imatch '^Launcher-R\d+\.\d+(?:\.\d+){0,2}\.exe$'
    })
    if ($candidates.Count -ne 1) {
        throw 'The package must contain exactly one Launcher-R<version>.exe (or one legacy Launcher.exe). Extract a complete ZIP into its own folder.'
    }
    $candidate = $candidates[0]
    if ($candidate.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'The packaged Launcher executable must be a regular file.'
    }
    $metadata = & $MetadataReader $candidate.FullName
    if ($candidate.Name -imatch '^Launcher-R(.+)\.exe$') {
        $namedVersion = [version]$Matches[1]
        $normalizedName = [version]::new($namedVersion.Major, $namedVersion.Minor,
            [Math]::Max(0, $namedVersion.Build), [Math]::Max(0, $namedVersion.Revision))
        if ($normalizedName -ne (Get-LauncherExactVersion $metadata -RequireFixed)) {
            throw "The packaged executable filename disagrees with its embedded file version: $($candidate.Name)"
        }
    }
    return $candidate.FullName
}

function Get-LauncherExecutableIdentity {
    param([string]$Path, [scriptblock]$MetadataReader)

    $publisher = 'D' + [char]0x0142 + 'ugosz'
    $product = $publisher + ' Launcher'
    $identity = [pscustomobject]@{
        State = 'Unknown'; Component = 'Unknown'; ProductName = ''; Copyright = ''
        Version = ''; InternalName = ''; OriginalFilename = ''; Reason = ''
    }
    try {
        $metadata = & $MetadataReader $Path
        if ($null -eq $metadata) { throw 'No version metadata was returned.' }
        $identity.ProductName = [string](Get-LauncherDiscoveryProperty $metadata 'ProductName')
        $identity.Copyright = [string](Get-LauncherDiscoveryProperty $metadata 'LegalCopyright')
        $identity.Version = Get-LauncherVersionLabel $metadata
        $identity.InternalName = [string](Get-LauncherDiscoveryProperty $metadata 'InternalName')
        $identity.OriginalFilename = [string](Get-LauncherDiscoveryProperty $metadata 'OriginalFilename')
        foreach ($component in @('Launcher', 'LauncherDrop', 'LauncherSvgExtractor')) {
            $internalNames = if ($component -eq 'LauncherSvgExtractor') { @($component, ($component + '.exe')) } else { @($component) }
            if ($identity.InternalName -cin $internalNames -and
                $identity.OriginalFilename -ceq ($component + '.exe')) {
                $identity.Component = $component
                break
            }
        }
        $leaf = [IO.Path]::GetFileName($Path)
        $supportedName = ($identity.Component -eq 'Launcher' -and
            $leaf -imatch '^Launcher(?:-R\d+\.\d+(?:\.\d+){0,2})?\.exe$') -or
            ($identity.Component -eq 'LauncherDrop' -and $leaf -ieq 'LauncherDrop.exe') -or
            ($identity.Component -eq 'LauncherSvgExtractor' -and $leaf -ieq 'LauncherSvgExtractor.exe')
        $expectedProduct = if ($identity.Component -eq 'LauncherSvgExtractor') { $publisher + ' Launcher SVG Extractor' } else { $product }
        $special = (Get-LauncherDiscoveryProperty $metadata 'IsSpecialBuild') -or
            (Get-LauncherDiscoveryProperty $metadata 'IsPrivateBuild') -or
            ![string]::IsNullOrWhiteSpace([string](Get-LauncherDiscoveryProperty $metadata 'SpecialBuild')) -or
            ![string]::IsNullOrWhiteSpace([string](Get-LauncherDiscoveryProperty $metadata 'PrivateBuild'))
        if ($special) {
            $identity.State = 'Foreign'
            $identity.Reason = 'Special or instrumented build; not an installable application.'
        } elseif ($identity.Component -eq 'Unknown') {
            $identity.State = 'Foreign'
            $identity.Reason = 'Component metadata does not identify Launcher or a bundled companion.'
        } elseif (!$supportedName) {
            $identity.State = 'Foreign'
            $identity.Component = 'Other'
            $identity.Reason = 'The actual executable filename is not a supported Launcher or companion application name.'
        } elseif ($identity.ProductName -ceq $expectedProduct -and
            $identity.Copyright -cmatch ('^Copyright\s+(?:' + [char]0x00a9 + '|\(c\))\s+\d{4}(?:\s*[-,]\s*\d{4})*\s+John ' + [regex]::Escape($publisher) + '\.?$')) {
            $identity.State = 'Verified'
            $identity.Reason = 'Product, copyright owner, and component metadata match.'
        } elseif ($identity.ProductName -ceq 'Launcher' -and
            [string]::IsNullOrWhiteSpace($identity.Copyright)) {
            $identity.State = 'Legacy'
            $identity.Reason = 'Older Launcher metadata lacks the current product and copyright identity; manual validation is required.'
        } else {
            $identity.State = 'Foreign'
            $identity.Reason = 'Product name or copyright owner does not match.'
        }
    } catch {
        $identity.State = 'Unknown'
        $identity.Reason = 'Cannot read executable metadata: ' + $_.Exception.Message
    }
    return $identity
}

function Get-LauncherSupersededExecutables {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Directory,
        [Parameter(Mandatory)][version]$IncomingVersion,
        [scriptblock]$MetadataReader = { param($Path) [Diagnostics.FileVersionInfo]::GetVersionInfo($Path) }
    )
    $incoming = [version]::new($IncomingVersion.Major, $IncomingVersion.Minor,
        [Math]::Max(0, $IncomingVersion.Build), [Math]::Max(0, $IncomingVersion.Revision))
    if (@($incoming.Major, $incoming.Minor, $incoming.Build, $incoming.Revision | Where-Object { $_ -gt 65535 }).Count) {
        throw 'The incoming numeric Windows version must use components from 0 to 65535.'
    }
    $install = [IO.Path]::GetFullPath($Directory).TrimEnd('\', '/')
    if ($install -ieq [IO.Path]::GetPathRoot($install).TrimEnd('\', '/')) { throw 'Executable cleanup requires a dedicated Launcher folder.' }
    for ($current = $install; $current; $current = [IO.Path]::GetDirectoryName($current)) {
        $entry = Get-Item -LiteralPath $current -Force -ErrorAction Stop
        if (!$entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Executable cleanup paths must use real directories: $current"
        }
    }
    foreach ($file in @(Get-ChildItem -LiteralPath $install -Filter 'Launcher*.exe' -File -Force -ErrorAction Stop)) {
        if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
        $namedVersion = $null
        if ($file.Name -imatch '^Launcher-R(?<Version>\d+\.\d+(?:\.\d+){0,2})(?:\.\d{8}-\d{6}\.[0-9a-f]{32}\.backup)?\.exe$') {
            $namedVersion = $Matches.Version
        } elseif ($file.Name -ine 'Launcher-WIP.exe' -and $file.Name -inotmatch '^Launcher\.\d{8}-\d{6}\.[0-9a-f]{32}\.backup\.exe$' -and
            $file.Name -inotmatch '^Launcher-WIP\.legacy\.[0-9a-f]{32}\.backup\.exe$') { continue }
        try {
            $metadata = & $MetadataReader $file.FullName
            if ($null -eq $metadata -or (Get-LauncherDiscoveryProperty $metadata 'IsDebug')) { continue }
            # Backup basenames are deliberately outside normal discovery. Read
            # their real metadata using the canonical component name so only
            # verified main Launcher executables can be cleanup candidates.
            $identityReader = { param($Ignored) $metadata }.GetNewClosure()
            $identity = Get-LauncherExecutableIdentity (Join-Path $install 'Launcher.exe') $identityReader
            if ($identity.State -ne 'Verified' -or $identity.Component -ne 'Launcher') { continue }
            $version = Get-LauncherExactVersion $metadata -RequireFixed
            if ($version -ge $incoming) { continue }
            if ($namedVersion) {
                $named = [version]::Parse($namedVersion)
                $normalizedName = [version]::new($named.Major, $named.Minor, [Math]::Max(0, $named.Build), [Math]::Max(0, $named.Revision))
                if ($version -ne $normalizedName) { continue }
            }
            [pscustomobject]@{ Path = $file.FullName; Sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256 -ErrorAction Stop).Hash; Version = $version }
        } catch { Write-Verbose "Executable could not be verified for cleanup: $($file.FullName). $($_.Exception.Message)" }
    }
}

function Get-LauncherDiscovery {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$PackageDirectory,
        [string]$LocalDestination,
        [string]$MachineDestination,
        [string]$StartupDirectory,
        # The readers permit deterministic checks without launching applications,
        # changing permissions, or depending on which processes happen to run.
        [scriptblock]$MetadataReader = { param($Path) [Diagnostics.FileVersionInfo]::GetVersionInfo($Path) },
        [scriptblock]$DirectoryReader = {
            param($Path)
            try { $entry = Get-Item -LiteralPath $Path -Force -ErrorAction Stop }
            catch {
                if ($_.CategoryInfo.Category -eq 'ObjectNotFound') {
                    return [pscustomobject]@{ Exists = $false; Entries = @() }
                }
                throw
            }
            if (!$entry.PSIsContainer) { throw 'The installation path is not a directory.' }
            # Follow an active Launcher.exe link for metadata. Preserved releases
            # and process image paths may name its versioned target instead.
            $entries = @(Get-ChildItem -LiteralPath $Path -Filter 'Launcher*.exe' -Force -ErrorAction Stop |
                Where-Object { !$_.PSIsContainer })
            return [pscustomobject]@{ Exists = $true; Entries = $entries }
        },
        [scriptblock]$ProcessReader = {
            Get-LauncherProcessCandidates
        },
        [scriptblock]$ProcessOwnerReader = {
            param($Process)
            return [string](Get-LauncherDiscoveryProperty $Process 'Owner')
        },
        [scriptblock]$ProcessOwnerSidReader = {
            param($Process)
            return [string](Get-LauncherDiscoveryProperty $Process 'OwnerSid')
        },
        [scriptblock]$ShortcutReader = {
            param($Path)
            $shell = $null
            $shortcut = $null
            try {
                $shell = New-Object -ComObject WScript.Shell
                # Get-LauncherDiscovery calls this only for an existing file.
                # CreateShortcut reads it; Save is deliberately never called.
                $shortcut = $shell.CreateShortcut($Path)
                return [pscustomobject]@{
                    TargetPath = [string]$shortcut.TargetPath
                    WorkingDirectory = [string]$shortcut.WorkingDirectory
                    Arguments = [string]$shortcut.Arguments
                }
            } finally {
                if ($null -ne $shortcut -and [Runtime.InteropServices.Marshal]::IsComObject($shortcut)) {
                    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut)
                }
                if ($null -ne $shell -and [Runtime.InteropServices.Marshal]::IsComObject($shell)) {
                    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell)
                }
            }
        }
    )

    $publisher = 'D' + [char]0x0142 + 'ugosz'
    if ([string]::IsNullOrWhiteSpace($LocalDestination)) {
        $LocalDestination = [IO.Path]::Combine([Environment]::GetFolderPath('LocalApplicationData'), 'Programs', $publisher, 'Launcher')
    }
    if ([string]::IsNullOrWhiteSpace($MachineDestination)) {
        $programFiles = if ($env:ProgramW6432) { $env:ProgramW6432 } else { [Environment]::GetFolderPath('ProgramFiles') }
        $MachineDestination = [IO.Path]::Combine($programFiles, $publisher, 'Launcher')
    }
    $packagePath = ConvertTo-LauncherDiscoveryPath $PackageDirectory
    $localPath = ConvertTo-LauncherDiscoveryPath $LocalDestination
    $machinePath = ConvertTo-LauncherDiscoveryPath $MachineDestination
    if ([string]::IsNullOrWhiteSpace($StartupDirectory)) {
        $StartupDirectory = [Environment]::GetFolderPath('Startup')
    }
    $issues = [Collections.Generic.List[string]]::new()
    $locations = [Collections.Generic.List[object]]::new()

    foreach ($scope in @('AllUsers', 'CurrentUser')) {
        $directory = if ($scope -eq 'AllUsers') { $machinePath } else { $localPath }
        $location = [pscustomobject]@{
            Scope = $scope; Directory = $directory; State = 'Unknown'; IsInstalled = $false
            CanUpgrade = $false; Identity = $null; Version = ''; Candidates = @(); Issues = @()
        }
        $locationIssues = [Collections.Generic.List[string]]::new()
        try {
            $directoryInfo = & $DirectoryReader $directory
            if (!$directoryInfo.Exists) {
                $location.State = 'Missing'
            } else {
                $candidates = @(
                    foreach ($entry in @($directoryInfo.Entries)) {
                        if ([IO.Path]::GetFileName($entry.FullName) -imatch 'backup') { continue }
                        $path = ConvertTo-LauncherDiscoveryPath $entry.FullName
                        $identity = Get-LauncherExecutableIdentity $path $MetadataReader
                        $name = [IO.Path]::GetFileName($path)
                        $isMainName = $name -imatch '^Launcher(?:-R\d+\.\d+(?:\.\d+){0,2})?\.exe$'
                        $isMain = $isMainName -and $identity.Component -eq 'Launcher'
                        [pscustomobject]@{
                            Path = $path; Name = $name; Identity = $identity
                            IsMain = $isMain; IsActive = ($name -ieq 'Launcher.exe')
                        }
                    }
                )
                $location.Candidates = $candidates
                $verified = @($candidates | Where-Object { $_.IsMain -and $_.Identity.State -eq 'Verified' })
                $unverified = @($candidates | Where-Object {
                    ($_.IsMain -or $_.IsActive -or $_.Name -ilike 'Launcher-R*.exe') -and $_.Identity.State -ne 'Verified'
                })
                foreach ($candidate in $unverified) {
                    $locationIssues.Add(('{0}: {1} ({2})' -f $candidate.Path, $candidate.Identity.State, $candidate.Identity.Reason))
                }
                if ($verified.Count) {
                    $location.IsInstalled = $true
                    $active = @($verified | Where-Object { $_.IsActive })
                    $chosen = if ($active.Count) { $active[0] } else { $verified[0] }
                    $location.Identity = $chosen.Identity
                    $location.Version = $chosen.Identity.Version
                    if (!$unverified.Count -or $active.Count) {
                        $location.State = 'Verified'
                        $location.CanUpgrade = $true
                    } else { $location.State = 'NeedsReview' }
                    if (!$active.Count) {
                        $locationIssues.Add('No verified active Launcher.exe entry was found; the preserved release identifies this installation.')
                    }
                } elseif ($unverified.Count) {
                    $states = @($unverified | ForEach-Object { $_.Identity.State } | Select-Object -Unique)
                    $location.State = if ($states.Count -eq 1) { $states[0] } else { 'NeedsReview' }
                    $location.Identity = $unverified[0].Identity
                    $location.Version = $unverified[0].Identity.Version
                } else {
                    $location.State = 'Available'
                    if ($candidates.Count) {
                        $locationIssues.Add('Only companion or unrelated Launcher-named executables were found; no main Launcher installation was identified.')
                    }
                }
            }
        } catch {
            $location.State = 'Unknown'
            $locationIssues.Add('Cannot inspect the installation directory: ' + $_.Exception.Message)
        }
        $location.Issues = @($locationIssues)
        foreach ($issue in $locationIssues) { $issues.Add($scope + ': ' + $issue) }
        $locations.Add($location)
    }

    $processes = [Collections.Generic.List[object]]::new()
    $processEnumerationSucceeded = $true
    try { $running = @(& $ProcessReader) }
    catch {
        $processEnumerationSucceeded = $false
        $running = @()
        $issues.Add('Cannot enumerate running Launcher candidates: ' + $_.Exception.Message)
    }
    foreach ($process in $running) {
        $name = [string](Get-LauncherDiscoveryProperty $process 'Name')
        if ($name -notlike 'Launcher*.exe') { continue }
        if ($name -imatch 'backup') { continue }
        $rawPath = [string](Get-LauncherDiscoveryProperty $process 'ExecutablePath')
        if ($rawPath -and [IO.Path]::GetFileName($rawPath) -imatch 'backup') { continue }
        $processId = Get-LauncherDiscoveryProperty $process 'ProcessId'
        $processIssues = [Collections.Generic.List[string]]::new()
        foreach ($issue in @(Get-LauncherDiscoveryProperty $process 'Issues')) {
            if ($issue) { $processIssues.Add([string]$issue) }
        }
        $path = $null
        try {
            $path = ConvertTo-LauncherDiscoveryPath $rawPath
            if (!$path) { throw 'The executable path is unavailable (access denied or process exited).' }
        } catch { $processIssues.Add($_.Exception.Message) }
        $identity = if ($path) { Get-LauncherExecutableIdentity $path $MetadataReader } else {
            [pscustomobject]@{ State = 'Unknown'; Component = 'Unknown'; ProductName = ''; Copyright = ''
                Version = ''; InternalName = ''; OriginalFilename = ''; Reason = 'Executable path is unavailable.' }
        }
        if ($identity.State -ne 'Verified') { $processIssues.Add($identity.Reason) }
        $sessionId = Get-LauncherDiscoveryProperty $process 'SessionId'
        if ($null -eq $sessionId) { $processIssues.Add('The process session is unavailable.') }
        $owner = $null
        try {
            $owner = [string](& $ProcessOwnerReader $process)
            if ([string]::IsNullOrWhiteSpace($owner)) { throw 'The process owner is unavailable.' }
        } catch { $processIssues.Add('Cannot read the process owner: ' + $_.Exception.Message) }
        $ownerSid = $null
        try {
            $ownerSid = [string](& $ProcessOwnerSidReader $process)
            if ([string]::IsNullOrWhiteSpace($ownerSid)) { throw 'The process owner SID is unavailable.' }
        } catch { $processIssues.Add('Cannot read the process owner SID: ' + $_.Exception.Message) }
        $creationTime = $null
        try {
            $created = Get-LauncherDiscoveryProperty $process 'CreationDate'
            if ($null -eq $created) { throw 'The process creation time is unavailable.' }
            $creationTime = ([datetime]$created).ToUniversalTime().ToString('o')
        } catch { $processIssues.Add('Cannot read the process creation time: ' + $_.Exception.Message) }
        $directory = if ($path) { ConvertTo-LauncherDiscoveryPath ([IO.Path]::GetDirectoryName($path)) } else { $null }
        $isSource = $null -ne $directory -and $directory -ieq $packagePath
        $isLocal = $null -ne $directory -and $directory -ieq $localPath
        $isMachine = $null -ne $directory -and $directory -ieq $machinePath
        $classification = if (!$directory) { 'Unknown' } elseif ($isSource) { 'Source' }
            elseif ($isLocal) { 'CurrentUser' } elseif ($isMachine) { 'AllUsers' } else { 'Elsewhere' }
        $processes.Add([pscustomobject]@{
            ProcessId = $processId; Name = $name; Path = $path; SessionId = $sessionId; Owner = $owner
            OwnerSid = $ownerSid; CreationTimeUtc = $creationTime
            Identity = $identity; Component = $identity.Component; Location = $classification
            IsSource = $isSource; IsCurrentUserInstall = $isLocal; IsAllUsersInstall = $isMachine
            Issues = @($processIssues)
        })
        foreach ($issue in $processIssues) { $issues.Add(('Process {0} ({1}): {2}' -f $processId, $name, $issue)) }
    }

    $startup = [pscustomobject]@{
        Path = ''; Exists = $false; State = 'Missing'; TargetPath = ''; TargetDirectory = ''
        WorkingDirectory = ''; Arguments = ''; CurrentScope = 'Unknown'; Sha256 = ''; Issues = @()
    }
    try {
        if ([string]::IsNullOrWhiteSpace($StartupDirectory)) { throw 'The current-user Startup folder is unavailable.' }
        $startup.Path = [IO.Path]::Combine((ConvertTo-LauncherDiscoveryPath $StartupDirectory), 'Launcher.lnk')
        $entry = $null
        try { $entry = Get-Item -LiteralPath $startup.Path -Force -ErrorAction Stop }
        catch {
            if ($_.CategoryInfo.Category -ne 'ObjectNotFound') { throw }
        }
        if ($null -ne $entry) {
            $startup.Exists = $true
            if ($entry.PSIsContainer) { throw 'Launcher.lnk is a directory rather than a shortcut file.' }
            if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Launcher.lnk must not be a symbolic link or reparse point.' }
            $shortcutHash = (Get-FileHash -LiteralPath $startup.Path -Algorithm SHA256 -ErrorAction Stop).Hash
            $shortcut = & $ShortcutReader $startup.Path
            if ((Get-FileHash -LiteralPath $startup.Path -Algorithm SHA256 -ErrorAction Stop).Hash -ne $shortcutHash) {
                throw 'The shortcut changed while it was being inspected; refresh discovery.'
            }
            $startup.Sha256 = $shortcutHash
            $target = [Environment]::ExpandEnvironmentVariables([string](Get-LauncherDiscoveryProperty $shortcut 'TargetPath'))
            if ([string]::IsNullOrWhiteSpace($target) -or ![IO.Path]::IsPathRooted($target)) {
                throw 'The shortcut target is unavailable or is not an absolute file path.'
            }
            $startup.TargetPath = ConvertTo-LauncherDiscoveryPath $target
            $startup.TargetDirectory = ConvertTo-LauncherDiscoveryPath ([IO.Path]::GetDirectoryName($startup.TargetPath))
            $startup.WorkingDirectory = [string](Get-LauncherDiscoveryProperty $shortcut 'WorkingDirectory')
            $startup.Arguments = [string](Get-LauncherDiscoveryProperty $shortcut 'Arguments')
            $startup.CurrentScope = if ($startup.TargetDirectory -ieq $machinePath) { 'AllUsers' }
                elseif ($startup.TargetDirectory -ieq $localPath) { 'CurrentUser' }
                elseif ($startup.TargetDirectory -ieq $packagePath) { 'Source' } else { 'Elsewhere' }
            $startup.State = 'Readable'
        }
    } catch {
        $startup.State = 'Unknown'
        if (!$startup.Exists) { $startup.Exists = $null }
        $startup.Issues = @('Cannot inspect the current-user Startup shortcut: ' + $_.Exception.Message)
        foreach ($issue in $startup.Issues) { $issues.Add($issue) }
    }
    return [pscustomobject]@{
        PackageDirectory = $packagePath
        UserSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
        Locations = @($locations)
        Installations = @($locations | Where-Object { $_.IsInstalled })
        LegacyInstallations = @($locations | Where-Object { $_.State -eq 'Legacy' })
        Processes = @($processes)
        ProcessEnumerationSucceeded = $processEnumerationSucceeded
        Startup = $startup
        Issues = @($issues)
    }
}

function Get-LauncherStartupPlan {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]$Discovery,
        [Parameter(Mandatory = $true)][string]$SelectedDestination,
        [Alias('StartOnLogin')][bool]$StartOnSignIn
    )

    $destination = ConvertTo-LauncherDiscoveryPath $SelectedDestination
    $selected = @((Get-LauncherDiscoveryProperty $Discovery 'Locations') | Where-Object { $_.Directory -ieq $destination })
    $freshTarget = $selected.Count -eq 1 -and $selected[0].State -in @('Missing', 'Available')
    $startup = Get-LauncherDiscoveryProperty $Discovery 'Startup'
    $matchesSelected = $false
    $matchesExisting = $false
    $targetIsLauncher = $false
    if ($null -ne $startup -and $startup.State -eq 'Readable') {
        $targetIsLauncher = [IO.Path]::GetFileName($startup.TargetPath) -ieq 'Launcher.exe'
        $matchesSelected = $targetIsLauncher -and $startup.TargetDirectory -ieq $destination
        $matchesExisting = $targetIsLauncher -and @($Discovery.Installations | Where-Object {
            $_.CanUpgrade -and $_.Directory -ieq $startup.TargetDirectory -and
            @($_.Candidates | Where-Object {
                $_.IsActive -and $_.Identity.State -eq 'Verified' -and $_.Identity.Component -eq 'Launcher'
            }).Count -gt 0
        }).Count -gt 0
    }
    $action = 'None'
    $explanation = ''
    if ($null -eq $startup -or $startup.State -eq 'Unknown') {
        if ($StartOnSignIn) {
            $action = 'Review'
            $explanation = 'The existing Startup entry could not be identified. Review it before creating or changing a shortcut.'
        } else { $explanation = 'Leave the unreadable or unknown Startup entry unchanged.' }
    } elseif ($startup.State -eq 'Missing') {
        if ($StartOnSignIn) {
            $action = 'Create'
            $explanation = 'Create a current-user Startup shortcut to the selected installation.'
        } else { $explanation = 'No Startup shortcut exists; leave sign-in startup disabled.' }
    } elseif ($matchesSelected -and $matchesExisting) {
        if ($StartOnSignIn) {
            $action = 'Keep'
            $explanation = 'Keep the existing Startup shortcut and its arguments for this installation.'
        } else {
            $action = 'Remove'
            $explanation = 'Remove the current-user Startup shortcut for the selected installation.'
        }
    } elseif (!$StartOnSignIn) {
        $explanation = 'Keep the existing Startup shortcut for another or unrecognized destination unchanged.'
    } elseif ($matchesExisting) {
        $action = 'Retarget'
        $explanation = 'Retarget the current-user Startup shortcut from ' + $startup.TargetPath +
            ' to ' + [IO.Path]::Combine($destination, 'Launcher.exe') + '; preserve its existing arguments.'
    } else {
        $action = 'Review'
        $explanation = 'The existing Startup shortcut does not identify a recognized installation. Review the conflict before changing it.'
    }
    return [pscustomobject]@{
        Action = $action; Explanation = $explanation; Message = $explanation
        MatchesSelectedDestination = $matchesSelected; MatchesExistingInstallation = $matchesExisting
        DefaultChecked = ($freshTarget -or ($matchesSelected -and $matchesExisting))
    }
}

function Get-LauncherDiscoveryMarker {
    param($Identity, [string]$Path, [string]$SelectedDestination, [bool]$IsRunning,
        [bool]$IsInstallation = $false)

    if ($Identity.State -eq 'Verified' -and $Identity.Component -in @('LauncherDrop', 'LauncherSvgExtractor')) {
        return [string][char]0x2013 + ' Ignored companion'
    }
    if (!$IsRunning -and $Identity.Component -eq 'Launcher' -and
        $Identity.State -in @('Verified', 'Legacy') -and
        [IO.Path]::GetFileName($Path) -imatch '^Launcher-R\d+\.\d+(?:\.\d+){0,2}\.exe$') {
        return [string][char]0x2013 + ' Ignored preserved release'
    }
    if ($Identity.State -in @('Legacy', 'Unknown')) { return '? Needs review' }
    if ($Identity.State -ne 'Verified' -or $Identity.Component -ne 'Launcher') {
        return [string][char]0x2013 + ' Ignored foreign or other program'
    }
    $directory = ConvertTo-LauncherDiscoveryPath ([IO.Path]::GetDirectoryName($Path))
    if ($SelectedDestination -and $directory -ieq $SelectedDestination) {
        return [string][char]0x2191 + ' Targeted upgrade'
    }
    if ($IsRunning -and !$IsInstallation) { return [string][char]0x25b6 + ' Portable currently running' }
    return [string][char]0x2013 + ' Ignored other installation'
}

function Format-LauncherDiscovery {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, ValueFromPipeline = $true)]$Discovery,
        [string]$SelectedDestination
    )

    $selected = ConvertTo-LauncherDiscoveryPath $SelectedDestination
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add('Installation summary:')
    foreach ($location in $Discovery.Locations) {
        $label = if ($location.Scope -eq 'AllUsers') { 'Program Files (all users)' }
            else { 'Local Programs (current user)' }
        $finding = if ($location.State -eq 'Unknown') { 'UNABLE TO CHECK' }
            elseif ($location.State -in @('Legacy', 'NeedsReview', 'Foreign')) { 'NEEDS REVIEW' }
            elseif ($location.IsInstalled) { 'FOUND' }
            elseif ($location.State -in @('Missing', 'Available')) { 'NOT FOUND' }
            else { 'NEEDS REVIEW' }
        $lines.Add(('{0}: {1} | {2}' -f $label, $finding, $location.Directory))
    }
    $lines.Add('')
    $lines.Add('Source: ' + $Discovery.PackageDirectory)
    if ($selected) { $lines.Add('Selected destination: ' + $selected) }
    $lines.Add('')
    $lines.Add('Installed executable details:')
    $candidateCount = 0
    foreach ($location in $Discovery.Locations) {
        foreach ($candidate in $location.Candidates) {
            $candidateCount++
            $marker = Get-LauncherDiscoveryMarker $candidate.Identity $candidate.Path $selected $false $true
            $version = if ($candidate.Identity.Version) { 'version ' + $candidate.Identity.Version } else { 'version unknown' }
            $identityNote = switch ($candidate.Identity.State) {
                'Legacy' { ' | older metadata; needs review' }
                'Unknown' { ' | unable to check identity' }
                'Foreign' { ' | unrecognized identity' }
                default { '' }
            }
            $lines.Add(('{0} | {1} | {2} | {3}{4}' -f $marker, $candidate.Path, $version,
                $candidate.Identity.Component, $identityNote))
        }
    }
    if (!$candidateCount) { $lines.Add('None found (see any findings requiring attention below).') }
    $lines.Add('')
    $lines.Add('Running copies:')
    if (!$Discovery.Processes.Count) {
        $lines.Add($(if ((Get-LauncherDiscoveryProperty $Discovery 'ProcessEnumerationSucceeded') -eq $false) {
            'Unable to check running copies; see findings below.'
        } else { 'None found.' }))
    }
    foreach ($process in $Discovery.Processes) {
        $flags = @()
        if ($process.IsSource) { $flags += 'source' }
        if ($process.IsCurrentUserInstall) { $flags += 'current-user installation' }
        if ($process.IsAllUsersInstall) { $flags += 'all-users installation' }
        if (!$flags.Count) {
            $flags = if ($process.Location -eq 'Unknown') { @('location unknown') } else { @('another location') }
        }
        $owner = if ($process.Owner) { $process.Owner } else { '<unknown owner>' }
        $session = if ($null -ne $process.SessionId) { $process.SessionId } else { '<unknown>' }
        $path = if ($process.Path) { $process.Path } else { '<unknown executable path>' }
        $isInstallation = $process.IsCurrentUserInstall -or $process.IsAllUsersInstall
        $marker = if ($process.Identity.State -eq 'Verified' -and $process.Issues.Count) { '? Needs review' } else {
            Get-LauncherDiscoveryMarker $process.Identity $process.Path $selected $true $isInstallation
        }
        $version = if ($process.Identity.Version) { 'version ' + $process.Identity.Version } else { 'version unknown' }
        $lines.Add(('{0} | {1} | PID {2} | {3} | {4} | {5} | session {6} | {7}' -f $marker, $path,
            $process.ProcessId, $version, $process.Component, ($flags -join ', '), $session, $owner))
    }
    $startup = Get-LauncherDiscoveryProperty $Discovery 'Startup'
    if ($null -ne $startup) {
        $lines.Add('')
        $finding = switch ($startup.State) {
            'Readable' { 'FOUND' }
            'Missing' { 'NOT FOUND' }
            default { 'UNABLE TO CHECK' }
        }
        $lines.Add('Current-user Startup shortcut: ' + $finding + ' | ' + $startup.Path)
        if ($startup.State -eq 'Readable') {
            $lines.Add('  Target: ' + $startup.TargetPath)
            $lines.Add('  Working directory: ' + $startup.WorkingDirectory)
            $lines.Add('  Arguments: ' + $startup.Arguments)
        }
    }
    if ($Discovery.Issues.Count) {
        $lines.Add('')
        $lines.Add('Findings requiring attention:')
        foreach ($issue in $Discovery.Issues) { $lines.Add('  ? ' + $issue) }
    }
    return $lines -join [Environment]::NewLine
}
