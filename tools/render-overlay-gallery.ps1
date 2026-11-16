[CmdletBinding()]
param(
    [string] $ExporterPath,
    [string] $NodePath = 'node',
    [string] $OutputDirectory
)
$ErrorActionPreference = 'Stop'
$galleryProject = Split-Path -Parent $PSScriptRoot
if (!$ExporterPath) { $ExporterPath = Join-Path $galleryProject 'build\windows-x64\Release\LauncherOverlayGallery.exe' }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $galleryProject ('build\previews\overlay-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8)) }
$galleryOutput = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $galleryOutput) { throw 'Choose a new output directory; existing galleries are preserved.' }
if (!(Test-Path -LiteralPath $ExporterPath -PathType Leaf)) { throw 'Build the Release LauncherOverlayGallery target first, or supply -ExporterPath.' }
$galleryNode = (Get-Command $NodePath -ErrorAction Stop).Source
[void](New-Item -ItemType Directory -Path $galleryOutput)
& $ExporterPath (Join-Path $galleryOutput 'native')
if ($LASTEXITCODE -ne 0) { throw 'Native overlay rendering failed.' }
& $galleryNode (Join-Path $PSScriptRoot 'overlay-gallery\assemble.cjs') (Join-Path $galleryOutput 'native') (Join-Path $galleryOutput 'gallery')
if ($LASTEXITCODE -ne 0) { throw 'Gallery assembly failed.' }
Write-Output 'Open gallery.html, then use Save manual PNG or Save manual SVG. No personal settings were accessed.'
