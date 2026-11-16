[CmdletBinding()]
param(
    [string] $SourcePath,
    [string] $OutputPath
)

$ErrorActionPreference = 'Stop'
if (!$SourcePath) { $SourcePath = Join-Path (Split-Path -Parent $PSScriptRoot) 'colors-light.toml' }
if (!$OutputPath) { $OutputPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'colors-dark.toml' }
if ([IO.Path]::GetFullPath($SourcePath) -eq [IO.Path]::GetFullPath($OutputPath)) {
    throw 'Source and output must be different files.'
}

function ConvertTo-Hsb([string] $hex) {
    $rgb = [Convert]::ToUInt32($hex.TrimStart('#'), 16)
    $r = (($rgb -shr 16) -band 255) / 255.0
    $g = (($rgb -shr 8) -band 255) / 255.0
    $b = ($rgb -band 255) / 255.0
    $maximum = [Math]::Max($r, [Math]::Max($g, $b))
    $minimum = [Math]::Min($r, [Math]::Min($g, $b))
    $delta = $maximum - $minimum
    $hue = 0.0
    if ($delta -gt 0) {
        if ($maximum -eq $r) { $hue = 60.0 * (($g - $b) / $delta) }
        elseif ($maximum -eq $g) { $hue = 60.0 * (2.0 + ($b - $r) / $delta) }
        else { $hue = 60.0 * (4.0 + ($r - $g) / $delta) }
        if ($hue -lt 0) { $hue += 360.0 }
    }
    $saturation = if ($maximum -eq 0) { 0.0 } else { $delta / $maximum }
    [pscustomobject]@{ Hue = $hue; Saturation = $saturation; Brightness = $maximum }
}

function ConvertFrom-Hsb([double] $Hue, [double] $Saturation, [double] $Brightness) {
    $chroma = $Brightness * $Saturation
    $sector = $Hue / 60.0
    $x = $chroma * (1.0 - [Math]::Abs(($sector % 2.0) - 1.0))
    $channels = switch ([int][Math]::Floor($sector)) {
        0 { @($chroma, $x, 0.0) }
        1 { @($x, $chroma, 0.0) }
        2 { @(0.0, $chroma, $x) }
        3 { @(0.0, $x, $chroma) }
        4 { @($x, 0.0, $chroma) }
        5 { @($chroma, 0.0, $x) }
        default { throw 'HSB hue is outside 0 through 360 degrees.' }
    }
    $offset = $Brightness - $chroma
    '#' + (($channels | ForEach-Object {
        ([int][Math]::Floor(($_ + $offset) * 255.0 + 0.5)).ToString('X2')
    }) -join '')
}

$source = [IO.File]::ReadAllText($SourcePath)
$sections = [regex]::Matches($source, '(?ms)(^\[colors\.([^\]\r\n]+)\]\r?\n)(.*?)(?=^\[colors\.|\z)')
if (!$sections.Count) { throw 'The source contains no named palettes.' }
$output = [Text.StringBuilder]::new()
$null = $output.AppendLine('# Select this set with colors = "dark" in Launcher.toml.')
$null = $output.AppendLine('# Copy this file to AppData as colors-custom.toml; select colors = "custom" to edit it.')
$null = $output.AppendLine('# Generated from colors-light.toml by tools/generate-dark-colors.ps1.')
$null = $output.AppendLine('# Each palette keeps its first source page hue, except Yellow at H=60 degrees.')
$null = $output.AppendLine('# Page S decreases from 100% to 80%; B increases from 50% to 70%.')
$null = $output.AppendLine('# Inactive tabs are flat at the midpoint: B=60%, S=90%.')
$null = $output.AppendLine('# Tab hover: B=40%, S=100%; outline: B=75%, S=100%.')
$null = $output.AppendLine('# Icon hover uses white for a strong frame against every hue.')
$null = $output.AppendLine('# text is the tab-label color. Restart Launcher after editing.')
$null = $output.AppendLine()

foreach ($section in $sections) {
    $body = $section.Groups[3].Value
    $page = [regex]::Match($body, '(?ms)^page\.gradient\s*=\s*\[.*?"(#[A-Fa-f0-9]{6})"')
    if (!$page.Success) { throw "Missing page gradient in $($section.Groups[2].Value)." }
    $base = ConvertTo-Hsb $page.Groups[1].Value
    if ($section.Groups[2].Value -eq 'yellow') { $base.Hue = 60.0 }
    $gradients = [regex]::Matches($body, '(?ms)^(tab|page)\.gradient\s*=\s*(\[.*?\])')
    if ($gradients.Count -ne 2) { throw 'Expected tab.gradient and page.gradient.' }
    $body = [regex]::Replace($body, '(?ms)^(tab|page)\.gradient\s*=\s*(\[.*?\])',
        [Text.RegularExpressions.MatchEvaluator]{ param($gradient)
            $kind = $gradient.Groups[1].Value
            $ramp = [regex]::Replace($gradient.Groups[2].Value,
                '(\{\s*at\s*=\s*([0-9.]+)\s*,\s*color\s*=\s*")(#[A-Fa-f0-9]{6})(")',
                [Text.RegularExpressions.MatchEvaluator]{ param($stop)
                    $at = [double]::Parse($stop.Groups[2].Value, [Globalization.CultureInfo]::InvariantCulture)
                    $brightness = if ($kind -eq 'tab') { 0.60 } else { 0.50 + 0.20 * $at }
                    $saturation = if ($kind -eq 'tab') { 0.90 } else { 1.0 - 0.20 * $at }
                    $color = ConvertFrom-Hsb $base.Hue $saturation $brightness
                    return $stop.Groups[1].Value + $color + $stop.Groups[4].Value
                })
            return $kind + '.gradient = ' + $ramp
        })
    $tabHover = ConvertFrom-Hsb $base.Hue 1.0 0.40
    $pageHover = '#FFFFFF'
    $outlineMatch = [regex]::Match($body, '(?m)^outline\s*=\s*"(#[A-Fa-f0-9]{6})"')
    if (!$outlineMatch.Success) { throw 'Missing outline color.' }
    $outline = ConvertFrom-Hsb $base.Hue 1.0 0.75
    $body = [regex]::Replace($body, '(?m)^tab\.hover\s*=\s*"#[A-Fa-f0-9]{6}"', ('tab.hover = "' + $tabHover + '"'))
    $body = [regex]::Replace($body, '(?m)^page\.hover\s*=\s*"#[A-Fa-f0-9]{6}"', ('page.hover = "' + $pageHover + '"'))
    $body = [regex]::Replace($body, '(?m)^outline\s*=\s*"#[A-Fa-f0-9]{6}"', ('outline = "' + $outline + '"'))
    $body = [regex]::Replace($body, '(?m)^text\s*=.*\r?\n?', '')
    $null = $output.AppendLine($section.Groups[1].Value.TrimEnd())
    $null = $output.AppendLine($body.TrimEnd())
    $null = $output.AppendLine('text = "#F2F2F2"')
    $null = $output.AppendLine()
}
[IO.File]::WriteAllText($OutputPath, $output.ToString().TrimEnd().Replace("`r`n", "`n") + "`n", [Text.UTF8Encoding]::new($false))
Write-Output "Generated $($sections.Count) dark palettes: $OutputPath"
