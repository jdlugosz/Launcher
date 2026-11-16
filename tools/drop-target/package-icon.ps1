[CmdletBinding()]
param([string] $ImageMagickPath = 'C:\Program Files\ImageMagick-7.1.2-Q16-HDRI\magick.exe')

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outputDirectory = Join-Path $projectRoot 'build\drop-icon'
[void](New-Item -ItemType Directory -Path $outputDirectory -Force)
$frames = @()
foreach ($size in @(16, 20, 24, 32, 40, 48, 64, 128, 256)) {
    $frame = Join-Path $outputDirectory "drop-$size.png"
    & $ImageMagickPath -background none -density 384 (Join-Path $PSScriptRoot 'drop-icon.svg') -resize "${size}x${size}" -depth 8 "PNG32:$frame"
    if ($LASTEXITCODE -ne 0) { throw 'Could not render the drop icon.' }
    $frames += $frame
}
& $ImageMagickPath @frames -type TrueColorAlpha (Join-Path $PSScriptRoot 'drop.ico')
if ($LASTEXITCODE -ne 0) { throw 'Could not package the drop icon.' }
Write-Output "Created drop.ico with $($frames.Count) sizes from 16 to 256 pixels."
