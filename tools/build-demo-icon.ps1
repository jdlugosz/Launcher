#requires -Version 5.1
[CmdletBinding()]
param([Parameter(Mandatory)][string]$PalettePath, [Parameter(Mandatory)][string]$OutputPath)
$ErrorActionPreference = 'Stop'
$palette = [IO.File]::ReadAllText($PalettePath)
$svg = [Text.StringBuilder]::new()
[void]$svg.AppendLine('<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128"><title>Launcher basic color swatches</title><defs><linearGradient id="pin" x2="1" y2="1"><stop stop-color="#f5f7f9"/><stop offset="1" stop-color="#737d86"/></linearGradient><clipPath id="card"><rect x="10" y="6" width="28" height="110" rx="7"/></clipPath></defs>')
foreach ($leaf in @(@('blue',81), @('green',54), @('yellow',27), @('red',0))) {
    $name = $leaf[0]; $angle = $leaf[1]
    $section = [regex]::Match($palette, '(?s)\[colors\.' + $name + '\]\s*tab\.gradient\s*=\s*\[(.*?)\]')
    $colors = @([regex]::Matches($section.Groups[1].Value, 'color\s*=\s*"(#[0-9A-Fa-f]{6})"') | ForEach-Object { $_.Groups[1].Value })
    if ($colors.Count -ne 3) { throw "Expected three basic tab colors for $name." }
    [void]$svg.AppendLine('<g transform="rotate(' + $angle + ' 24 102)"><rect x="10" y="6" width="28" height="110" rx="7" fill="#f2f3f4" stroke="#646e78" stroke-width="1.4"/><g clip-path="url(#card)">')
    for ($index = 0; $index -lt 3; $index++) {
        $top = 6 + $index * 28
        [void]$svg.AppendLine('<rect x="10.7" y="' + $top + '" width="26.6" height="26" fill="' + $colors[$index] + '"/>')
    }
    [void]$svg.AppendLine('</g></g>')
}
[void]$svg.AppendLine('<circle cx="24" cy="102" r="5" fill="url(#pin)" stroke="#78828c" stroke-width="1"/></svg>')
[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($OutputPath)))
[IO.File]::WriteAllText($OutputPath, $svg.ToString(), [Text.UTF8Encoding]::new($false))
