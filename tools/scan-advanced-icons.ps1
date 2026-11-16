[CmdletBinding()]
param(
    [string[]] $Roots,
    [string] $CatalogPath,
    [switch] $AllDrives,
    [string] $OutputDirectory,
    [switch] $IncludeZip,
    [switch] $ExportAssets,
    [switch] $SvgOnly,
    [ValidateRange(1, 1048576)] [long] $MaxExportMB = 512,
    [string[]] $Exclude,
    [ValidateRange(1, 1024)] [int] $MaxAssetMB = 32,
    [ValidateRange(1, 2147483647)] [int] $MaxFiles,
    [ValidateRange(1, 2147483647)] [int] $MaxSeconds,
    [switch] $BuildOnly
)

# Windows PowerShell 5.1 or PowerShell 7; no Python, SDK or NuGet dependency.
$ErrorActionPreference = 'Stop'
$catalogProject = Split-Path -Parent $PSScriptRoot
$catalogSource = Join-Path $PSScriptRoot 'advanced-icon-catalog\AdvancedIconCatalog.cs'
$catalogManifest = Join-Path $PSScriptRoot 'advanced-icon-catalog\app.manifest'
$catalogHash = (Get-FileHash -LiteralPath $catalogSource -Algorithm SHA256).Hash + (Get-FileHash -LiteralPath $catalogManifest -Algorithm SHA256).Hash
# Separate builds let an existing read-only scan finish while the utility is
# updated. No executable that might still be running is overwritten.
$catalogBuildKey = $catalogHash.Substring(0, 16) + '-' + $catalogHash.Substring(64, 8)
$catalogBuild = Join-Path $catalogProject ('build\tools\advanced-icon-catalog\' + $catalogBuildKey)
$catalogExe = Join-Path $catalogBuild 'AdvancedIconCatalog.exe'
$catalogStamp = Join-Path $catalogBuild 'source-sha256.txt'
if (!(Test-Path -LiteralPath $catalogExe) -or !(Test-Path -LiteralPath $catalogStamp) -or
    (Get-Content -LiteralPath $catalogStamp -Raw).Trim() -ne $catalogHash) {
    $catalogFramework = if ([Environment]::Is64BitOperatingSystem) { 'Framework64' } else { 'Framework' }
    $catalogCompiler = Join-Path ([Environment]::GetFolderPath('Windows')) ("Microsoft.NET\$catalogFramework\v4.0.30319\csc.exe")
    if (!(Test-Path -LiteralPath $catalogCompiler)) { throw 'The Windows .NET Framework C# compiler is unavailable. Use the prebuilt AdvancedIconCatalog.exe.' }
    $null = New-Item -ItemType Directory -Path $catalogBuild -Force
    & $catalogCompiler /nologo /optimize+ /warnaserror+ /target:exe /platform:anycpu "/out:$catalogExe" "/win32manifest:$catalogManifest" `
        /reference:System.Core.dll /reference:System.Xml.dll /reference:System.Xml.Linq.dll `
        /reference:System.IO.Compression.dll /reference:System.IO.Compression.FileSystem.dll `
        /reference:System.Web.Extensions.dll $catalogSource
    if ($LASTEXITCODE -ne 0) { throw 'Advanced icon scanner compilation failed.' }
    Set-Content -LiteralPath $catalogStamp -Value $catalogHash -Encoding ASCII
}
if ($BuildOnly) { Write-Output $catalogExe; return }
if (!$AllDrives -and !$Roots.Count -and !$CatalogPath) { throw 'Choose -Roots, -AllDrives or -CatalogPath (resources.csv).' }
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $catalogProject ('build\icon-catalog\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
}
$catalogArguments = @('--output', [IO.Path]::GetFullPath($OutputDirectory), '--max-mb', $MaxAssetMB)
if ($AllDrives) { $catalogArguments += '--all-drives' }
if ($CatalogPath) { $catalogArguments += @('--from-catalog', [IO.Path]::GetFullPath($CatalogPath)) }
if ($IncludeZip) { $catalogArguments += '--include-zip' }
if ($ExportAssets) { $catalogArguments += @('--export', '--max-export-mb', $MaxExportMB) }
if ($SvgOnly) { $catalogArguments += '--svg-only' }
if ($MaxFiles) { $catalogArguments += @('--max-files', $MaxFiles) }
if ($MaxSeconds) { $catalogArguments += @('--max-seconds', $MaxSeconds) }
foreach ($catalogExclude in $Exclude) { $catalogArguments += @('--exclude', [IO.Path]::GetFullPath($catalogExclude)) }
foreach ($catalogRoot in $Roots) { $catalogArguments += [IO.Path]::GetFullPath($catalogRoot) }
& $catalogExe @catalogArguments
$catalogExit = $LASTEXITCODE
if ($catalogExit -eq 1) { throw 'The scanner failed; consult its error and any partial output.' }
$global:LASTEXITCODE = $catalogExit
