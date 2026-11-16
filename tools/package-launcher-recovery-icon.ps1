[CmdletBinding()]
param(
    [string] $ImageMagickPath = 'C:\Program Files\ImageMagick-7.1.2-Q16-HDRI\magick.exe'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$source = Join-Path $projectRoot 'resources\launcher.ico'
$output = Join-Path $projectRoot 'resources\launcher-recovery.ico'
$intermediate = Join-Path $projectRoot 'build\launcher-recovery-icon'
[void] (New-Item -ItemType Directory -Path $intermediate -Force)

function Invoke-IconMagick([string[]] $CommandArguments) {
    & $ImageMagickPath @CommandArguments
    if ($LASTEXITCODE -ne 0) { throw "ImageMagick failed with exit code $LASTEXITCODE." }
}

# Start with each authored size so the small icon keeps its simplified artwork.
$sourceSizes = @(Invoke-IconMagick @('identify', '-format', '%wx%h\n', $source))
$frames = for ($index = 0; $index -lt $sourceSizes.Count; ++$index) {
    if ($sourceSizes[$index] -notmatch '^([0-9]+)x\1$') { throw 'Expected a square source frame.' }
    $size = [int] $Matches[1]
    $border = [Math]::Max(1, [int] [Math]::Floor($size / 24))
    $inner = $size - 4 * $border
    $last = $size - 1
    $near = $border - 1
    $far = $size - $border
    $outline = "rectangle 0,0 $last,$near rectangle 0,$far $last,$last rectangle 0,0 $near,$last rectangle $far,0 $last,$last"
    $frame = Join-Path $intermediate "$size.png"
    Invoke-IconMagick @("$source[$index]", '-filter', 'Lanczos', '-resize', "${inner}x$inner",
        '-background', 'none', '-gravity', 'center', '-extent', "${size}x$size", '+gravity',
        '-fill', '#D72F40', '+antialias', '-draw', $outline, '-depth', '8', "PNG32:$frame")
    $frame
}
Invoke-IconMagick ($frames + @('-type', 'TrueColorAlpha', $output))
$outputSizes = @(Invoke-IconMagick @('identify', '-format', '%wx%h\n', $output))
if (($sourceSizes -join ',') -ne ($outputSizes -join ',')) {
    throw 'The recovery icon frame sizes must match the source icon.'
}
Write-Output "Packed the red recovery frame at $($outputSizes.Count) icon sizes."
