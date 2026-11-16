[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $StageDirectory,
    [Parameter(Mandatory)][string] $Archive,
    [Parameter(Mandatory)][string] $SourceDirectory,
    [Parameter(Mandatory)][string] $BuildDirectory,
    [string] $LicenseFile,
    [string] $ArchiveBuildId
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$script:checks = 0
function Check([bool] $Condition, [string] $Message) {
    if (!$Condition) { throw $Message }
    $script:checks++
}
function FileHash([string] $Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function StreamHash([IO.Stream] $Stream) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($Stream)).Replace('-', '') }
    finally { $algorithm.Dispose() }
}

$stage = [IO.Path]::GetFullPath($StageDirectory).TrimEnd('\', '/')
$source = [IO.Path]::GetFullPath($SourceDirectory)
if (!$LicenseFile) { $LicenseFile = Join-Path $source 'licenses\Prerelease.txt' }
$build = [IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\', '/')
$archivePath = [IO.Path]::GetFullPath($Archive)
$rootName = [IO.Path]::GetFileName($stage)
Check ($rootName -cmatch '^Launcher-R([0-9]+\.[0-9]+(?:\.[0-9]+){0,2})-windows-x64$') 'Shipping root must include its executable version.'
$displayVersion = $Matches[1]
$mainName = "Launcher-R$displayVersion.exe"
$expectedVersion = [Version]$displayVersion
$stageEntry = Get-Item -LiteralPath $stage -Force
Check ($stageEntry.PSIsContainer -and !($stageEntry.Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Shipping stage must be a real directory.'
Check (![string]::IsNullOrEmpty($rootName)) 'Shipping stage must have a named top-level folder.'
. (Join-Path $source 'tools\zip-entry-points.ps1')
$entryPointContents = Get-LauncherZipEntryPointContents -PackageName $rootName
$entryPoints = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
$encoding = [Text.UTF8Encoding]::new($false)
foreach ($name in $entryPointContents.Keys) {
    $path = Join-Path (Split-Path -Parent $stage) $name
    $entry = Get-Item -LiteralPath $path -Force
    Check (!$entry.PSIsContainer -and !($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) "ZIP entry point must be a regular file: $name"
    $actual = [Convert]::ToBase64String([IO.File]::ReadAllBytes($path))
    $expected = [Convert]::ToBase64String($encoding.GetBytes($entryPointContents[$name]))
    Check ($actual -ceq $expected) "ZIP entry point is stale or does not target '$rootName': $name"
    $entryPoints.Add($name, [pscustomobject]@{ Length = $entry.Length; Hash = FileHash $path })
}

$required = @(
    $mainName, 'LauncherDrop.exe', 'LauncherSvgExtractor.exe', 'Launcher Setup.exe', 'Launcher.default.toml', 'Launcher.installed.default.toml', 'README.txt', 'LICENSE.txt', 'THIRD-PARTY-NOTICES.txt', 'Install.cmd', 'Install.ps1',
    'Discovery.ps1', 'Presentation.ps1', 'Deployment.ps1', 'Lifecycle.ps1', 'Processes.ps1', 'Worker.ps1', 'PortableProfile.ps1', 'WindowTextQuery.cs',
    'Maintenance.ps1', 'Uninstall.cmd', 'Registration.ps1',
    'examples/basic_colors.toml', 'examples/extra_colors.toml', 'examples/colors-light.toml', 'examples/colors-dark.toml',
    'examples/demo_many_colors.toml', 'examples/demo_many_colors.cmd', 'resources/many-colors.svg', 'docs/Launcher.md', 'docs/LauncherDrop.md', 'docs/Installer.md',
    'docs/SvgExtractor.md', 'docs/advanced-icon-catalog.md',
    'docs/Launcher.html', 'docs/LauncherDrop.html', 'docs/Installer.html', 'docs/SvgExtractor.html', 'docs/advanced-icon-catalog.html', 'docs/FileLocations.html',
    'docs/Configuration.html', 'docs/LaunchTargets.html', 'docs/Appearance.html'
)
if ($ArchiveBuildId) { $required += @('ArchiveRecovery.ps1', 'ArchiveRestoreJournal.ps1', 'ArchiveSigner.cer', 'Launcher.archive.json') }
$allowed = $required
$files = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
$directories = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$pending = [Collections.Generic.Stack[string]]::new()
$pending.Push($stage)
while ($pending.Count) {
    foreach ($entry in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
        Check (!($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) "Shipping stage contains a reparse point: $($entry.FullName)"
        $relative = $entry.FullName.Substring($stage.Length + 1).Replace('\', '/')
        if ($entry.PSIsContainer) {
            Check ($relative -cin @('examples', 'docs', 'resources')) "Unexpected shipping directory: $relative"
            [void]$directories.Add($relative)
            $pending.Push($entry.FullName)
        } else {
            Check ($relative -cin $allowed) "Unexpected shipping file (personal settings and build/test artifacts are excluded): $relative"
            $files.Add($relative, [pscustomobject]@{ Length = $entry.Length; Hash = FileHash $entry.FullName })
        }
    }
}
foreach ($name in $required) { Check ($files.ContainsKey($name)) "Required shipping file is missing: $name" }
Check ($files.ContainsKey('Install.cmd') -eq $files.ContainsKey('Install.ps1')) 'Install.cmd and Install.ps1 must both be present or both be absent.'

# Compare only explicit payload sources, never the rest of the working build tree.
$configuration = Split-Path -Leaf (Split-Path -Parent $stage)
$binaryDirectory = Join-Path $build $configuration
$buildRoot = $build
if (Test-Path -LiteralPath (Join-Path $build 'Launcher.exe') -PathType Leaf) {
    $binaryDirectory = $build
    $buildRoot = Split-Path -Parent $build
}
$sources = @{
    $mainName = Join-Path $binaryDirectory 'Launcher.exe'
    'LauncherDrop.exe' = Join-Path $binaryDirectory 'LauncherDrop.exe'
    'LauncherSvgExtractor.exe' = Join-Path $binaryDirectory 'LauncherSvgExtractor.exe'
    'Launcher Setup.exe' = Join-Path $binaryDirectory 'Launcher Setup.exe'
    'Launcher.default.toml' = Join-Path $source 'Launcher.toml'
    'Launcher.installed.default.toml' = Join-Path $binaryDirectory 'Launcher.installed.default.toml'
    'README.txt' = Join-Path $buildRoot 'generated-resources\README.txt'
    'LICENSE.txt' = [IO.Path]::GetFullPath($LicenseFile)
    'THIRD-PARTY-NOTICES.txt' = Join-Path $source 'THIRD-PARTY-NOTICES.txt'
    'Install.cmd' = Join-Path $source 'tools\installer\Install.cmd'
    'Install.ps1' = Join-Path $source 'tools\installer\Install.ps1'
    'Discovery.ps1' = Join-Path $source 'tools\installer\Discovery.ps1'
    'Presentation.ps1' = Join-Path $source 'tools\installer\Presentation.ps1'
    'Deployment.ps1' = Join-Path $source 'tools\installer\Deployment.ps1'
    'Lifecycle.ps1' = Join-Path $source 'tools\installer\Lifecycle.ps1'
    'Processes.ps1' = Join-Path $source 'tools\installer\Processes.ps1'
    'Worker.ps1' = Join-Path $source 'tools\installer\Worker.ps1'
    'PortableProfile.ps1' = Join-Path $source 'tools\installer\PortableProfile.ps1'
    'WindowTextQuery.cs' = Join-Path $source 'tools\installer\WindowTextQuery.cs'
    'Maintenance.ps1' = Join-Path $source 'tools\installer\Maintenance.ps1'
    'Uninstall.cmd' = Join-Path $source 'tools\installer\Uninstall.cmd'
    'Registration.ps1' = Join-Path $source 'tools\installer\Registration.ps1'
    'examples/basic_colors.toml' = Join-Path $source 'basic_colors.toml'
    'examples/extra_colors.toml' = Join-Path $source 'extra_colors.toml'
    'examples/colors-light.toml' = Join-Path $source 'colors-light.toml'
    'examples/colors-dark.toml' = Join-Path $source 'colors-dark.toml'
    'examples/demo_many_colors.toml' = Join-Path $source 'examples\demo_many_colors.toml'
    'examples/demo_many_colors.cmd' = Join-Path $source 'examples\demo_many_colors.cmd'
    'resources/many-colors.svg' = Join-Path $buildRoot 'generated-resources\many-colors.svg'
    'docs/Launcher.md' = Join-Path $source 'README.md'
    'docs/LauncherDrop.md' = Join-Path $source 'tools\drop-target\README.md'
    'docs/Installer.md' = Join-Path $source 'tools\installer\README.md'
    'docs/SvgExtractor.md' = Join-Path $source 'docs\svg-extractor.md'
    'docs/advanced-icon-catalog.md' = Join-Path $source 'docs\advanced-icon-catalog.md'
    'docs/Launcher.html' = Join-Path $binaryDirectory 'docs\Launcher.html'
    'docs/LauncherDrop.html' = Join-Path $binaryDirectory 'docs\LauncherDrop.html'
    'docs/Installer.html' = Join-Path $binaryDirectory 'docs\Installer.html'
    'docs/SvgExtractor.html' = Join-Path $binaryDirectory 'docs\SvgExtractor.html'
    'docs/advanced-icon-catalog.html' = Join-Path $binaryDirectory 'docs\advanced-icon-catalog.html'
    'docs/FileLocations.html' = Join-Path $binaryDirectory 'docs\FileLocations.html'
    'docs/Configuration.html' = Join-Path $binaryDirectory 'docs\Configuration.html'
    'docs/LaunchTargets.html' = Join-Path $binaryDirectory 'docs\LaunchTargets.html'
    'docs/Appearance.html' = Join-Path $binaryDirectory 'docs\Appearance.html'
}
if ($ArchiveBuildId) {
    $sources['ArchiveRecovery.ps1'] = Join-Path $buildRoot 'generated-resources\ArchiveRecovery.ps1'
    $sources['ArchiveRestoreJournal.ps1'] = Join-Path $source 'tools\installer\ArchiveRestoreJournal.ps1'
    $sources['ArchiveSigner.cer'] = Join-Path $buildRoot 'generated-resources\ArchiveSigner.cer'
    $sources['Launcher.archive.json'] = Join-Path $buildRoot 'generated-resources\Launcher.archive.json'
}
foreach ($name in $required) {
    $original = $sources[$name]
    Check (Test-Path -LiteralPath $original -PathType Leaf) "Shipping source is missing: $original"
    Check ($files[$name].Hash -ceq (FileHash $original)) "Staged file differs from its source: $name"
}

# Use code points so Windows PowerShell 5.1 reads this script independently of
# the system ANSI code page. Check the compiled resource, including its Unicode.
$productName = 'D' + [char]0x0142 + 'ugosz Launcher'
$copyright = 'Copyright ' + [char]0x00a9 + ' 2026 John D' + [char]0x0142 + 'ugosz'
foreach ($name in @($mainName, 'LauncherDrop.exe', 'LauncherSvgExtractor.exe')) {
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $stage $name))
    $expectedProduct = $productName
    $expectedCopyright = $copyright
    if ($name -ceq 'LauncherSvgExtractor.exe') {
        $expectedProduct += ' SVG Extractor'
        $expectedCopyright = 'Copyright (c) 2026 John D' + [char]0x0142 + 'ugosz'
    }
    Check ($version.ProductName -ceq $expectedProduct) "Incorrect product identity in $name."
    Check ($version.LegalCopyright -ceq $expectedCopyright) "Missing or incorrect copyright in $name."
    $originalName = $name
    if ($name -ceq $mainName) { $originalName = 'Launcher.exe' }
    $internalName = [IO.Path]::GetFileNameWithoutExtension($originalName)
    if ($name -ceq 'LauncherSvgExtractor.exe') { $internalName = $originalName }
    Check ($version.InternalName -ceq $internalName) "Incorrect internal name in $name."
    Check ($version.OriginalFilename -ceq $originalName) "Incorrect original filename in $name."
    if ($name -ceq 'LauncherSvgExtractor.exe') { continue }
    Check ($version.ProductVersion -ceq "$($expectedVersion.Major).$($expectedVersion.Minor)") "Incorrect product series in $name."
    Check ($version.ProductMajorPart -eq $expectedVersion.Major -and $version.ProductMinorPart -eq $expectedVersion.Minor -and
        $version.ProductBuildPart -eq 0 -and $version.ProductPrivatePart -eq 0) "Incorrect numeric product version in $name."
    Check ($version.FileVersion -ceq $displayVersion -and $version.FileMajorPart -eq $expectedVersion.Major -and
        $version.FileMinorPart -eq $expectedVersion.Minor -and $version.FileBuildPart -eq [Math]::Max(0, $expectedVersion.Build) -and
        $version.FilePrivatePart -eq [Math]::Max(0, $expectedVersion.Revision)) "Incorrect file version in $name."
    Check ($version.Comments -cin @('Launcher distribution: Prerelease', 'Launcher distribution: Beta', 'Launcher distribution: Release', 'Launcher distribution: Special')) "Missing distribution kind in $name."
    Check ($version.IsPreRelease -eq ($version.Comments -cne 'Launcher distribution: Release')) "Incorrect prerelease flag in $name."
}

$extractorMetadata = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $stage 'LauncherSvgExtractor.exe'))
Check ($extractorMetadata.ProductVersion -ceq '1.1') 'Incorrect SVG Extractor product version.'
Check ($extractorMetadata.FileVersion -cmatch '^1\.1\.0\.[1-9][0-9]*$') 'SVG Extractor needs its own four-part file version.'
$mainMetadata = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $stage $mainName))
Check ($extractorMetadata.Comments -ceq $mainMetadata.Comments) 'SVG Extractor distribution differs from Launcher.'

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    $archiveNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $archiveFiles = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $archiveEntryPoints = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $roots = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $payloadDirectoryEntry = $false
    foreach ($entry in $zip.Entries) {
        $name = $entry.FullName
        Check ($archiveNames.Add($name)) "Duplicate ZIP entry: $name"
        Check (!$name.Contains('\') -and !$name.Contains('//') -and !$name.Contains(':')) "Noncanonical ZIP entry: $name"
        if ($entryPoints.ContainsKey($name)) {
            [void]$archiveEntryPoints.Add($name)
            [void]$roots.Add($name)
            Check ($entry.Length -eq $entryPoints[$name].Length) "ZIP entry-point length differs: $name"
            $stream = $entry.Open()
            try { Check ((StreamHash $stream) -ceq $entryPoints[$name].Hash) "ZIP entry-point bytes differ: $name" }
            finally { $stream.Dispose() }
            continue
        }
        Check ($name.StartsWith($rootName + '/', [StringComparison]::Ordinal)) "Unexpected ZIP root entry: $name"
        [void]$roots.Add($rootName)
        $relative = $name.Substring($rootName.Length + 1)
        Check (@($relative.Split('/') | Where-Object { $_ -eq '.' -or $_ -eq '..' }).Count -eq 0) "Unsafe ZIP path: $name"
        if ($name.EndsWith('/', [StringComparison]::Ordinal)) {
            Check ($relative.Length -eq 0 -or $directories.Contains($relative.TrimEnd('/'))) "ZIP contains an unstaged directory: $name"
            if ($relative.Length -eq 0) { $payloadDirectoryEntry = $true }
            continue
        }
        Check ($files.ContainsKey($relative)) "ZIP contains an unstaged file: $name"
        [void]$archiveFiles.Add($relative)
        Check ($entry.Length -eq $files[$relative].Length) "ZIP file length differs from stage: $relative"
        $stream = $entry.Open()
        try { Check ((StreamHash $stream) -ceq $files[$relative].Hash) "ZIP file bytes differ from stage: $relative" }
        finally { $stream.Dispose() }
    }
    Check ($archiveFiles.Count -eq $files.Count) 'ZIP and stage have different file counts.'
    foreach ($name in $files.Keys) { Check ($archiveFiles.Contains($name)) "Staged file is missing from ZIP: $name" }
    Check ($archiveEntryPoints.Count -eq 2) 'ZIP must include both outer entry-point files.'
    foreach ($name in $entryPoints.Keys) { Check ($archiveEntryPoints.Contains($name)) "ZIP is missing the outer entry point: $name" }
    Check ($payloadDirectoryEntry) 'ZIP must contain its explicit versioned payload directory.'
    Check ($roots.Count -eq 3 -and $roots.Contains($rootName) -and $roots.Contains('Install.cmd') -and $roots.Contains('Portable.txt')) 'ZIP root must contain exactly the payload folder, Install.cmd and Portable.txt.'
} finally { $zip.Dispose() }

Write-Output "Shipping layout verified: $($files.Count) payload files, Install.cmd and Portable.txt at ZIP root, $script:checks checks."
