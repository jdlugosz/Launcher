[CmdletBinding()]
param(
    [switch] $Package,
    [string] $OutputDirectory,
    [string] $Version = '1.1.0.38',
    [string] $ProductVersion = '1.1',
    [ValidateSet('Prerelease', 'Beta', 'Special', 'Release')][string] $DistributionKind = 'Prerelease',
    [switch] $ReleasePackage,
    [string] $ReleaseVersion,
    [string] $CMake = 'cmake',
    [string] $Git = 'git'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$extractorProject = Split-Path -Parent $PSScriptRoot
$parsedVersion = [version]::Parse($Version)
if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$' -or
    @($parsedVersion.Major, $parsedVersion.Minor, $parsedVersion.Build, $parsedVersion.Revision | Where-Object { $_ -gt 65535 }).Count) {
    throw 'The extractor requires four Windows version components from 0 to 65535.'
}
$product = [version]::Parse($ProductVersion)
if ($ProductVersion -cne $product.ToString() -or $product.Build -ne -1 -or $product.Revision -ne -1 -or
    $product.Major -ne $parsedVersion.Major -or $product.Minor -ne $parsedVersion.Minor) {
    throw 'The extractor product version must match the first two file version components.'
}
$actualKind = $DistributionKind
if ($ReleasePackage) { $actualKind = 'Release' }
elseif ($actualKind -eq 'Release') { throw 'Release metadata requires -ReleasePackage and the release prerequisites.' }
$licenseName = if ($actualKind -in @('Beta', 'Release')) { 'Release.txt' } else { 'Prerelease.txt' }
$license = Join-Path $extractorProject ('licenses\' + $licenseName)
if ($ReleasePackage) {
    if ($ReleaseVersion -cnotmatch '\A\d+\.\d+\.\d+\z') { throw 'Specify the three-component Launcher version with -ReleaseVersion.' }
    $CMake = (Get-Command $CMake -ErrorAction Stop).Source
    $Git = (Get-Command $Git -ErrorAction Stop).Source
}
function Assert-ExtractorDistribution {
    if ($ReleasePackage) {
        & $CMake "-DSOURCE_DIR=$extractorProject" "-DRELEASE_VERSION=$ReleaseVersion" "-DGIT_EXECUTABLE=$Git" -P (Join-Path $PSScriptRoot 'verify-release.cmake')
        if ($LASTEXITCODE -ne 0) { throw 'SVG Extractor final-release prerequisites were not satisfied.' }
    } elseif ($actualKind -eq 'Beta') {
        & $CMake "-DSOURCE_DIR=$extractorProject" '-DDISTRIBUTION_KIND=Beta' -P (Join-Path $PSScriptRoot 'distribution-license.cmake')
        if ($LASTEXITCODE -ne 0) { throw 'SVG Extractor Beta license is unavailable.' }
    }
}
Assert-ExtractorDistribution
$extractorCore = Join-Path $PSScriptRoot 'advanced-icon-catalog\AdvancedIconCatalog.cs'
$extractorGui = Join-Path $PSScriptRoot 'advanced-icon-catalog\SvgExtractor.cs'
$extractorServer = Join-Path $PSScriptRoot 'advanced-icon-catalog\SvgGalleryServer.cs'
$extractorManifest = Join-Path $PSScriptRoot 'advanced-icon-catalog\svg-extractor.manifest'
$extractorGuide = Join-Path $extractorProject 'docs\svg-extractor.md'
$extractorIcon = Join-Path $extractorProject 'resources\launcher.ico'
$extractorCatalogGuide = Join-Path $extractorProject 'docs\advanced-icon-catalog.md'
$notices = Join-Path $extractorProject 'THIRD-PARTY-NOTICES.txt'
$compilerPath = 'MSBuild\Current\Bin\Roslyn\csc.exe'
$vsInstall = $env:VSINSTALLDIR
if (!$vsInstall -or !(Test-Path -LiteralPath (Join-Path $vsInstall $compilerPath) -PathType Leaf)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $vsInstall = & $vswhere -latest -products '*' -find $compilerPath | Select-Object -First 1
    }
    $compiler = $vsInstall
} else { $compiler = Join-Path $vsInstall $compilerPath }
if (!$compiler -or !(Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw 'Building the SVG Extractor requires the Roslyn C# compiler from Visual Studio Build Tools.'
}

$framework = if ([Environment]::Is64BitOperatingSystem) { 'Framework64' } else { 'Framework' }
$frameworkDirectory = Join-Path ([Environment]::GetFolderPath('Windows')) "Microsoft.NET\$framework\v4.0.30319"
$references = @('mscorlib.dll', 'System.dll', 'System.Core.dll', 'System.Xml.dll', 'System.Xml.Linq.dll',
    'System.IO.Compression.dll', 'System.IO.Compression.FileSystem.dll', 'System.Web.Extensions.dll', 'System.Windows.Forms.dll',
    'System.Drawing.dll') | ForEach-Object { Join-Path $frameworkDirectory $_ }
$extractorHash = (@($PSCommandPath, $compiler, (Join-Path (Split-Path -Parent $compiler) 'Microsoft.CodeAnalysis.CSharp.dll'),
    $extractorCore, $extractorGui, $extractorServer, $extractorManifest, $extractorGuide,
    $extractorIcon, $extractorCatalogGuide, $license, $notices) + $references | ForEach-Object { (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash }) -join ''
$extractorHash += "|$Version|$ProductVersion|$actualKind"
$hasher = [Security.Cryptography.SHA256]::Create()
try { $extractorKey = ([BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($extractorHash)))).Replace('-', '').Substring(0, 20) }
finally { $hasher.Dispose() }
$extractorBuild = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $extractorProject ('build\svg-extractor\' + $extractorKey) }
for ($current = $extractorBuild; $current; $current = [IO.Path]::GetDirectoryName($current)) {
    $entry = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
    if ($entry -and (!$entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint))) { throw "Extractor output must use real directories: $current" }
}
[void][IO.Directory]::CreateDirectory($extractorBuild)
$extractorExe = Join-Path $extractorBuild 'LauncherSvgExtractor.exe'
$extractorStamp = Join-Path $extractorBuild 'source-hash.txt'
$cached = (Test-Path -LiteralPath $extractorExe -PathType Leaf) -and (Test-Path -LiteralPath $extractorStamp -PathType Leaf) -and
    (Get-Content -LiteralPath $extractorStamp -Raw).Trim() -ceq $extractorKey
if (!$cached) {
    $temporary = Join-Path $extractorBuild ('.svg-extractor-' + [Guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($temporary)
    $temporaryExe = Join-Path $temporary 'LauncherSvgExtractor.exe'
    $assemblyInfo = Join-Path $temporary 'build-info.cs'
    $manifest = Join-Path $temporary 'app.manifest'
    try {
        function CsString([string] $Value) { return '"' + $Value.Replace('\', '\\').Replace('"', '\"').Replace([string][char]13, '\r').Replace([string][char]10, '\n') + '"' }
        $attributes = @('using System.Reflection;',
            ('[assembly: AssemblyVersion(' + (CsString $Version) + ')]'),
            ('[assembly: AssemblyFileVersion(' + (CsString $Version) + ')]'),
            ('[assembly: AssemblyInformationalVersion(' + (CsString $ProductVersion) + ')]'),
            ('[assembly: AssemblyDescription(' + (CsString "Launcher distribution: $actualKind") + ')]'),
            ('[assembly: AssemblyMetadata("DistributionKind", ' + (CsString $actualKind) + ')]'))
        [IO.File]::WriteAllLines($assemblyInfo, $attributes, [Text.UTF8Encoding]::new($true))
        $manifestText = (Get-Content -LiteralPath $extractorManifest -Raw) -replace '(?<=<assemblyIdentity version=")[0-9.]+', $Version
        [IO.File]::WriteAllText($manifest, $manifestText, [Text.UTF8Encoding]::new($true))
        # Match the legacy .NET Framework target while removing source-root
        # and generated-directory names from Roslyn's output.
        $pathMap = "/pathmap:$temporary=/_/generated,$extractorProject=/_/source"
        $arguments = @('/nologo', '/noconfig', '/nostdlib+', '/deterministic+', '/langversion:5', '/optimize+', '/warnaserror+',
            '/target:winexe', '/platform:anycpu', '/define:LAUNCHER_BUNDLED_EXTRACTOR', $pathMap,
            '/main:LauncherTools.SvgExtractorProgram', "/out:$temporaryExe", "/win32manifest:$manifest", "/win32icon:$extractorIcon") +
            @($references | ForEach-Object { "/reference:$_" }) + @($extractorCore, $extractorGui, $extractorServer, $assemblyInfo)
        & $compiler @arguments
        if ($LASTEXITCODE -ne 0) { throw 'SVG extractor compilation failed.' }
        $metadata = [Diagnostics.FileVersionInfo]::GetVersionInfo($temporaryExe)
        if ($metadata.FileVersion -cne $Version -or $metadata.ProductVersion -cne $ProductVersion -or $metadata.Comments -cne "Launcher distribution: $actualKind") {
            throw 'Compiled SVG Extractor version or distribution metadata failed verification.'
        }
        if (Test-Path -LiteralPath $extractorExe) { [IO.File]::Replace($temporaryExe, $extractorExe, [NullString]::Value) }
        else { [IO.File]::Move($temporaryExe, $extractorExe) }
        [IO.File]::WriteAllText($extractorStamp, $extractorKey + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    } finally {
        foreach ($path in @($temporaryExe, $assemblyInfo, $manifest)) { if (Test-Path -LiteralPath $path) { [IO.File]::Delete($path) } }
        [IO.Directory]::Delete($temporary)
    }
}
if (!$Package) { Write-Output $extractorExe; return }
Assert-ExtractorDistribution
$archive = Join-Path $extractorBuild ("LauncherSvgExtractor-$Version.zip")
$packageStamp = Join-Path $extractorBuild 'package-source-hash.txt'
if (Test-Path -LiteralPath $archive) {
    if (!(Test-Path -LiteralPath $packageStamp) -or (Get-Content -LiteralPath $packageStamp -Raw).Trim() -cne $extractorKey) {
        throw "A different SVG Extractor package already exists and will not be replaced: $archive"
    }
} else {
    Copy-Item -LiteralPath $extractorGuide -Destination (Join-Path $extractorBuild 'README.txt')
    Copy-Item -LiteralPath $extractorCatalogGuide -Destination (Join-Path $extractorBuild 'advanced-icon-catalog.md')
    Copy-Item -LiteralPath $license -Destination (Join-Path $extractorBuild 'LICENSE.txt')
    Copy-Item -LiteralPath $notices -Destination (Join-Path $extractorBuild 'THIRD-PARTY-NOTICES.txt')
    $payload = @($extractorExe, (Join-Path $extractorBuild 'README.txt'), (Join-Path $extractorBuild 'advanced-icon-catalog.md'),
        (Join-Path $extractorBuild 'LICENSE.txt'), (Join-Path $extractorBuild 'THIRD-PARTY-NOTICES.txt'))
    Compress-Archive -LiteralPath $payload -DestinationPath $archive
    [IO.File]::WriteAllText($packageStamp, $extractorKey + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
}
Write-Output $archive
