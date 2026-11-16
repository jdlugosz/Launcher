[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Stage', 'Zip')][string] $Action,
    [Parameter(Mandatory)][string] $BuildDirectory,
    [Parameter(Mandatory)][string] $Configuration,
    [Parameter(Mandatory)][string] $PackageName,
    [Parameter(Mandatory)][string] $CMake,
    [switch] $ReleasePackage,
    [string] $Version,
    [string] $Git,
    [string] $ArchiveBuildId
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'installed-launcher.ps1')
. (Join-Path $PSScriptRoot 'zip-entry-points.ps1')
if ($Configuration -ne 'Release') { throw 'Shipping targets require the Release configuration.' }
if ($PackageName -notmatch '^Launcher-R([0-9]+\.[0-9]+(?:\.[0-9]+){0,2})-windows-x64$') { throw 'Invalid shipping package name.' }
$displayVersion = $Matches[1]
$mainName = "Launcher-R$displayVersion.exe"
$expectedVersion = [Version]$displayVersion
$expectedPatch = [Math]::Max(0, $expectedVersion.Build)
$expectedRevision = [Math]::Max(0, $expectedVersion.Revision)
$build = [IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\', '/')
if (!(Test-Path -LiteralPath (Join-Path $build 'CMakeCache.txt') -PathType Leaf)) { throw 'BuildDirectory must be a configured CMake build directory.' }
$repository = Split-Path -Parent $PSScriptRoot
if ($ReleasePackage) {
    & $CMake "-DSOURCE_DIR=$repository" "-DRELEASE_VERSION=$Version" "-DGIT_EXECUTABLE=$Git" -P (Join-Path $PSScriptRoot 'verify-release.cmake')
    if ($LASTEXITCODE -ne 0) { throw 'Public-release prerequisites were not satisfied.' }
    $releaseVersion = [Version]::Parse($Version)
    $displayVersion = "$($releaseVersion.Major).$($releaseVersion.Minor)"
    if ($releaseVersion.Build -ne 0) { $displayVersion += ".$($releaseVersion.Build)" }
    if ($PackageName -cne "Launcher-R$displayVersion-windows-x64") { throw 'Package name does not match the verified release version.' }
}
$output = Join-Path $build ('shipping\' + $Configuration)
$stage = Join-Path $output $PackageName
$archive = Join-Path $output ($PackageName + '.zip')

function Assert-NoReparseAncestors([string] $Path) {
    $current = [IO.Path]::GetFullPath($Path)
    while ($current) {
        $entry = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
        if ($entry -and ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Shipping paths must not cross a reparse point: $current" }
        $current = [IO.Path]::GetDirectoryName($current)
    }
}

function Get-ShippingFiles([string] $Directory) {
    Assert-NoReparseAncestors $Directory
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($Directory)
    while ($pending.Count) {
        foreach ($entry in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
            if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Shipping folder contains a reparse point: $($entry.FullName)" }
            if ($entry.PSIsContainer) { $pending.Push($entry.FullName) }
            else { $entry }
        }
    }
}

function Remove-OwnedStage([string] $Directory) {
    $full = [IO.Path]::GetFullPath($Directory).TrimEnd('\', '/')
    $parent = [IO.Path]::GetDirectoryName($full)
    $leaf = [IO.Path]::GetFileName($full)
    if ($parent -ne $output -or ($leaf -ne $PackageName -and $leaf -notmatch '^\.stage-[0-9a-f]{32}$')) {
        throw "Refusing to remove a directory outside this build's shipping stage: $full"
    }
    if (Test-Path -LiteralPath $full) {
        $null = @(Get-ShippingFiles $full)
        Remove-Item -LiteralPath $full -Recurse -Force
    }
}

function Assert-CleanPayload([string] $Directory) {
    $files = @(Get-ShippingFiles $Directory)
    foreach ($entry in $files) {
        if ($entry.Name -like '*.runtime.toml' -or $entry.Extension -in @('.log', '.pdb') -or
            ($entry.DirectoryName -eq $Directory -and $entry.Name -eq 'Launcher.toml')) {
            throw "Personal settings or build artifacts cannot be shipped: $($entry.FullName)"
        }
    }
    if (Test-Path -LiteralPath (Join-Path $Directory 'Launcher.exe')) { throw 'The portable package must not contain a Launcher.exe alias.' }
    $required = @($mainName, 'LauncherDrop.exe', 'LauncherSvgExtractor.exe', 'Launcher.default.toml', 'Launcher.installed.default.toml', 'README.txt', 'LICENSE.txt', 'THIRD-PARTY-NOTICES.txt',
        'Launcher Setup.exe', 'Install.cmd', 'Install.ps1', 'Discovery.ps1', 'Presentation.ps1', 'Deployment.ps1', 'Lifecycle.ps1', 'Processes.ps1', 'Worker.ps1', 'PortableProfile.ps1', 'WindowTextQuery.cs',
        'Maintenance.ps1', 'Uninstall.cmd', 'Registration.ps1',
        'docs\Launcher.md', 'docs\LauncherDrop.md', 'docs\Installer.md', 'docs\SvgExtractor.md', 'docs\advanced-icon-catalog.md',
        'docs\Launcher.html', 'docs\LauncherDrop.html', 'docs\Installer.html', 'docs\SvgExtractor.html', 'docs\advanced-icon-catalog.html', 'docs\FileLocations.html',
        'docs\Configuration.html', 'docs\LaunchTargets.html', 'docs\Appearance.html',
        'examples\basic_colors.toml', 'examples\extra_colors.toml', 'examples\colors-light.toml', 'examples\colors-dark.toml',
        'examples\demo_many_colors.toml', 'examples\demo_many_colors.cmd', 'resources\many-colors.svg')
    if ($ArchiveBuildId) {
        $required += @('ArchiveRecovery.ps1', 'ArchiveRestoreJournal.ps1', 'ArchiveSigner.cer', 'Launcher.archive.json')
        $metadata = Get-Content -LiteralPath (Join-Path $Directory 'Launcher.archive.json') -Raw | ConvertFrom-Json
        if ($metadata.SchemaVersion -ne 1 -or $metadata.Package -cne 'launcher' -or $metadata.Build -cne $ArchiveBuildId -or
            $metadata.Platform -cne 'windows-x64' -or $metadata.Version -cne $displayVersion) {
            throw 'The shipping package has the wrong signed archive metadata.'
        }
    }
    foreach ($entry in $files) {
        $relative = $entry.FullName.Substring($Directory.Length + 1)
        if ($relative -cnotin $required) { throw "Unexpected shipping payload file: $relative" }
    }
    foreach ($name in $required) {
        if (!(Test-Path -LiteralPath (Join-Path $Directory $name) -PathType Leaf)) { throw "Shipping payload is missing $name." }
    }
    foreach ($name in @($mainName, 'LauncherDrop.exe', 'LauncherSvgExtractor.exe')) {
        $file = Join-Path $Directory $name
        Assert-LauncherNormalBuild $file
        if ([Diagnostics.FileVersionInfo]::GetVersionInfo($file).IsDebug) { throw "Debug executable cannot be shipped: $file" }
    }
    if ((Get-FileHash -LiteralPath (Join-Path $Directory 'LICENSE.txt')).Hash -ne (Get-FileHash -LiteralPath $license).Hash) {
        throw 'Staged license does not match the selected source license.'
    }
}

function Get-ZipEntryPointFiles {
    $contents = Get-LauncherZipEntryPointContents -PackageName $PackageName
    $encoding = [Text.UTF8Encoding]::new($false)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        foreach ($name in $contents.Keys) {
            $path = Join-Path $output $name
            Assert-NoReparseAncestors $path
            $entry = Get-Item -LiteralPath $path -Force -ErrorAction Stop
            if ($entry.PSIsContainer) { throw "ZIP entry point must be a regular file: $path" }
            $expected = [BitConverter]::ToString($algorithm.ComputeHash($encoding.GetBytes($contents[$name]))).Replace('-', '')
            $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            if ($hash -cne $expected) { throw "ZIP entry point is stale or does not match $PackageName`: $path. Stage this package again." }
            [pscustomobject]@{ Path = $path; Name = $name; Length = $entry.Length; Hash = $hash }
        }
    } finally { $algorithm.Dispose() }
}

function Assert-ArchiveFiles([string] $Path, $Expected) {
    $zip = [IO.Compression.ZipFile]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        $count = 0
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName
            if (!$names.Add($name)) { throw "Duplicate generated ZIP entry: $name" }
            if ($name -ceq ($PackageName + '/')) { continue }
            if (!$Expected.ContainsKey($name)) { throw "Unexpected generated ZIP entry: $name" }
            $expectedFile = $Expected[$name]
            if ($entry.Length -ne $expectedFile.Length) { throw "Generated ZIP file length differs: $name" }
            $stream = $entry.Open()
            try { $hash = [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '') }
            finally { $stream.Dispose() }
            if ($hash -cne $expectedFile.Hash) { throw "File changed while creating the ZIP: $name" }
            $count++
        }
        if (!$names.Contains($PackageName + '/') -or $count -ne $Expected.Count) { throw 'Generated ZIP is missing its payload folder or an expected file.' }
    } finally { $algorithm.Dispose(); $zip.Dispose() }
}

# Validate binaries before replacing any prior stage, including instrumentation
# recorded from compiler/linker flags rather than just the CMake scenario option.
$distributionComment = ''
foreach ($name in @('Launcher.exe', 'LauncherDrop.exe', 'LauncherSvgExtractor.exe')) {
    $file = Join-Path (Join-Path $build $Configuration) $name
    Assert-LauncherNormalBuild $file
    if ([Diagnostics.FileVersionInfo]::GetVersionInfo($file).IsDebug) { throw "Debug executable cannot be shipped: $file" }
    $binaryVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($file)
    if ($name -ne 'LauncherSvgExtractor.exe') {
        $expectedProduct = "$($expectedVersion.Major).$($expectedVersion.Minor)"
        if ($binaryVersion.ProductVersion -cne $expectedProduct -or
            $binaryVersion.ProductMajorPart -ne $expectedVersion.Major -or $binaryVersion.ProductMinorPart -ne $expectedVersion.Minor -or
            $binaryVersion.ProductBuildPart -ne 0 -or $binaryVersion.ProductPrivatePart -ne 0) {
            throw "Executable product version does not match the Launcher series: $file"
        }
        if ($binaryVersion.FileVersion -cne $displayVersion -or
            $binaryVersion.FileMajorPart -ne $expectedVersion.Major -or $binaryVersion.FileMinorPart -ne $expectedVersion.Minor -or
            $binaryVersion.FileBuildPart -ne $expectedPatch -or $binaryVersion.FilePrivatePart -ne $expectedRevision) {
            throw "Executable file version does not match the package name: $file"
        }
    }
    if ($binaryVersion.Comments -cnotin @('Launcher distribution: Prerelease', 'Launcher distribution: Beta', 'Launcher distribution: Release', 'Launcher distribution: Special')) {
        throw "Executable distribution kind is missing or invalid: $file"
    }
    if ($distributionComment -and $binaryVersion.Comments -cne $distributionComment) { throw 'Launcher and its companions have different distribution kinds.' }
    $distributionComment = $binaryVersion.Comments
    if ($ReleasePackage) {
        if ($binaryVersion.IsPreRelease -or $binaryVersion.Comments -cne 'Launcher distribution: Release') {
            throw "Executable does not match the verified release version: $file"
        }
    } elseif ($binaryVersion.Comments -ceq 'Launcher distribution: Release') {
        throw 'Final-release executables require the explicit release package gates.'
    }
}
$licenseName = if ($distributionComment -cin @('Launcher distribution: Beta', 'Launcher distribution: Release')) { 'Release.txt' } else { 'Prerelease.txt' }
$license = Join-Path $repository ('licenses\' + $licenseName)
Assert-NoReparseAncestors $output
[void][IO.Directory]::CreateDirectory($output)
# Ordinary revision-zero checks may refresh their generated local payload. A
# numbered or final distribution is immutable once its archive exists.
$immutable = $ReleasePackage -or ([Version]$displayVersion).Revision -gt 0 -or $distributionComment -ceq 'Launcher distribution: Special'
if ($immutable -and (Test-Path -LiteralPath $archive)) {
    throw "This distribution archive already exists and will not be replaced: $archive. Run the package driver to reserve another output."
}

if ($Action -eq 'Stage') {
    $fresh = Join-Path $output ('.stage-' + [Guid]::NewGuid().ToString('N'))
    try {
        & $CMake --install $build --config $Configuration --prefix $fresh
        if ($LASTEXITCODE -ne 0) { throw 'Staging the explicit CMake install payload failed.' }
        Assert-CleanPayload $fresh
        foreach ($name in @($mainName, 'LauncherDrop.exe', 'LauncherSvgExtractor.exe')) {
            $sourceName = $name
            if ($name -ceq $mainName) { $sourceName = 'Launcher.exe' }
            $source = Join-Path (Join-Path $build $Configuration) $sourceName
            if ((Get-FileHash -LiteralPath (Join-Path $fresh $name)).Hash -ne (Get-FileHash -LiteralPath $source).Hash) {
                throw "Staged executable differs from the build: $name"
            }
        }
        Remove-OwnedStage $stage
        Move-Item -LiteralPath $fresh -Destination $stage
        Write-LauncherZipEntryPoints -OutputDirectory $output -PackageName $PackageName
        $null = @(Get-ZipEntryPointFiles)
        Write-Output "Extracted-download stage: $stage"
    } finally { Remove-OwnedStage $fresh }
} else {
    Assert-CleanPayload $stage
    # These files are shared by local stages. Never zip a wrapper left pointing
    # at another version, and never enumerate the surrounding shipping folder.
    $entryPoints = @(Get-ZipEntryPointFiles)
    $payloadFiles = @(Get-ShippingFiles $stage)
    $expected = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
    foreach ($file in $entryPoints) { $expected.Add($file.Name, $file) }
    foreach ($file in $payloadFiles) {
        $relative = $PackageName + '/' + $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
        $expected.Add($relative, [pscustomobject]@{
            Path = $file.FullName; Length = $file.Length; Hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        })
    }
    Assert-NoReparseAncestors $archive
    if (Test-Path -LiteralPath $archive -PathType Container) { throw 'The ZIP output path is a directory.' }
    $temporary = Join-Path $output ('.archive-' + [Guid]::NewGuid().ToString('N') + '.zip')
    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [AppContext]::SetSwitch('Switch.System.IO.Compression.ZipFile.UseBackslash', $false)
        $zip = [IO.Compression.ZipFile]::Open($temporary, [IO.Compression.ZipArchiveMode]::Create)
        try {
            [void]$zip.CreateEntry($PackageName + '/')
            foreach ($name in $expected.Keys) {
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $expected[$name].Path, $name, [IO.Compression.CompressionLevel]::Optimal)
            }
        } finally { $zip.Dispose() }
        Assert-ArchiveFiles $temporary $expected
        if (Test-Path -LiteralPath $archive) { [IO.File]::Replace($temporary, $archive, [NullString]::Value) }
        else { [IO.File]::Move($temporary, $archive) }
        Write-Output "Download ZIP: $archive"
        Write-Output "SHA-256: $((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash)"
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
}
