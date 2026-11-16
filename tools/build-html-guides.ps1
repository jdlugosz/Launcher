[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputDirectory, [string]$SourceDirectory)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSVersion.Major -lt 7 -or !(Get-Command ConvertFrom-Markdown -ErrorAction SilentlyContinue)) {
    throw 'Building the HTML guides requires PowerShell 7 with ConvertFrom-Markdown. Run this build step with pwsh; the packaged guides need only a browser.'
}
if (!$SourceDirectory) { $SourceDirectory = Split-Path -Parent $PSScriptRoot }
$source = [IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\', '/')
$output = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\', '/')
if ($output -eq [IO.Path]::GetPathRoot($output).TrimEnd('\', '/')) { throw 'HTML guides require a dedicated output directory.' }
$guides = [ordered]@{
    'README.md' = 'Launcher.html'
    'tools/drop-target/README.md' = 'LauncherDrop.html'
    'tools/installer/README.md' = 'Installer.html'
    'docs/svg-extractor.md' = 'SvgExtractor.html'
    'docs/advanced-icon-catalog.md' = 'advanced-icon-catalog.html'
    'docs/file-locations.md' = 'FileLocations.html'
    'docs/configuration.md' = 'Configuration.html'
    'docs/launch-targets.md' = 'LaunchTargets.html'
    'docs/appearance.md' = 'Appearance.html'
}
$bundled = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($relative in $guides.Keys) { $bundled.Add($relative, $guides[$relative]) }
foreach ($relative in @('basic_colors.toml', 'extra_colors.toml', 'colors-light.toml', 'colors-dark.toml')) {
    $bundled.Add($relative, '../examples/' + $relative)
}
foreach ($relative in @('examples/demo_many_colors.toml', 'examples/demo_many_colors.cmd')) { $bundled.Add($relative, '../' + $relative) }
$bundled.Add('Launcher.toml', '../Launcher.default.toml')
$bundled.Add('LICENSE.txt', '../LICENSE.txt')
$bundled.Add('THIRD-PARTY-NOTICES.txt', '../THIRD-PARTY-NOTICES.txt')
$documents = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::OrdinalIgnoreCase)
$headingsPattern = '(?s)<h(?<level>[1-6])\b[^>]*\bid="(?<id>[^"]+)"[^>]*>(?<text>.*?)</h\k<level>>'
function Encode([string]$Value) { [Net.WebUtility]::HtmlEncode($Value) }

# Render everything before publishing any file. The source remains Markdown;
# only the listed documents and their known payload links become HTML.
foreach ($relative in $guides.Keys) {
    $path = Join-Path $source $relative
    $body = (ConvertFrom-Markdown -LiteralPath $path).Html
    $unsupported = [regex]::Match($body, '(?is)<(?:img|picture|video|audio|source|iframe|object|embed|script|link|base|style)\b[^>]*>|<meta\b[^>]*http-equiv\s*=|<[^>]*\son[a-z]+\s*=')
    if ($unsupported.Success) {
        throw "Unsupported embedded asset or executable HTML in $relative`: $($unsupported.Value). Include only self-contained Markdown text in the packaged guides."
    }
    $ids = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $headings = @([regex]::Matches($body, $headingsPattern) | ForEach-Object {
        $id = [Net.WebUtility]::HtmlDecode($_.Groups['id'].Value)
        if (!$ids.Add($id)) { throw "Duplicate heading anchor in $relative`: $id" }
        [pscustomobject]@{ Level = [int]$_.Groups['level'].Value; Id = $id;
            Text = [Net.WebUtility]::HtmlDecode([regex]::Replace($_.Groups['text'].Value, '<[^>]+>', '')) }
    })
    if (!$headings.Count) { throw "The HTML guide has no usable headings: $relative" }
    $documents.Add($relative, [pscustomobject]@{ Source = $relative; Output = $guides[$relative]; Body = $body; Headings = $headings; Ids = $ids; Html = $null })
}

foreach ($relative in $guides.Keys) {
    $document = $documents[$relative]
    $body = [regex]::Replace($document.Body, '(?s)<a\b(?<before>[^>]*?)\bhref="(?<href>[^"]*)"(?<after>[^>]*)>(?<label>.*?)</a>', {
        param($match)
        $href = [Net.WebUtility]::HtmlDecode($match.Groups['href'].Value)
        if ($href -match '^(?i:https?://|mailto:)') { return $match.Value }
        if ($href -match '^[A-Za-z][A-Za-z0-9+.-]*:|^//') { throw "Unsupported link scheme in $relative`: $href" }
        $parts = $href.Split('#', 2)
        $fragment = if ($parts.Count -gt 1) { [Uri]::UnescapeDataString($parts[1]) } else { '' }
        $file = [Uri]::UnescapeDataString($parts[0])
        if ($file.Contains('?')) { throw "Unsupported local query string in $relative`: $href" }
        $target = $relative
        if ($file) {
            $resolved = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetDirectoryName((Join-Path $source $relative))) $file))
            $target = [IO.Path]::GetRelativePath($source, $resolved).Replace('\', '/')
        }
        if (!$bundled.ContainsKey($target)) {
            return '<span class="source-reference">' + $match.Groups['label'].Value +
                ' <small>(source checkout: <code>' + (Encode $href) + '</code>)</small></span>'
        }
        if ($fragment -and $documents.ContainsKey($target) -and !$documents[$target].Ids.Contains($fragment)) {
            throw "Missing heading anchor in packaged guide link from $relative`: $href"
        }
        $mapped = if (!$file) { '' } else { $bundled[$target] }
        if ($fragment) { $mapped += '#' + $parts[1] }
        return '<a' + $match.Groups['before'].Value + 'href="' + (Encode $mapped) + '"' + $match.Groups['after'].Value + '>' + $match.Groups['label'].Value + '</a>'
    })
    $body = $body.Replace('<table>', '<div class="table-scroll"><table>').Replace('</table>', '</table></div>')
    $tocHeadings = @($document.Headings | Where-Object Level -GE 2)
    if (!$tocHeadings.Count) { $tocHeadings = @($document.Headings | Select-Object -First 1) }
    $contents = @($tocHeadings | ForEach-Object {
        '<li class="depth-' + $_.Level + '"><a href="#' + (Encode $_.Id) + '">' + (Encode $_.Text) + '</a></li>'
    }) -join "`n"
    $toc = if ($contents) { '<nav class="contents" aria-label="On this page"><h2>On this page</h2><ol>' + $contents + '</ol></nav>' } else { '<div></div>' }
    $title = Encode $document.Headings[0].Text
    $css = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'html-guides.css'))
    if ($css -match '(?i)@import|url\s*\(') { throw 'Packaged guide styles must not load external resources.' }
    $document.Html = @"
<!doctype html>
<html lang="en">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>$title — Launcher documentation</title><style>
$css
</style></head>
<body><a class="skip-link" href="#guide-content">Skip to guide</a><header class="document-header"><a href="Launcher.html">Launcher</a><span>Included documentation · available offline</span></header>
<div class="document-layout">$toc<main id="guide-content">$body</main></div>
<footer class="document-footer">Included guide. References marked “source checkout” describe material outside this download.</footer>
</body></html>
"@
}

for ($current = $output; $current; $current = [IO.Path]::GetDirectoryName($current)) {
    $entry = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
    if ($entry -and (!$entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint))) { throw "HTML guide output requires real directories: $current" }
}
[void][IO.Directory]::CreateDirectory($output)
foreach ($document in $documents.Values) {
    $path = Join-Path $output $document.Output
    $entry = Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    if ($entry -and ($entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint))) { throw "HTML guide output must be a regular file: $path" }
    $temporary = Join-Path $output ('.html-guide-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [IO.File]::WriteAllText($temporary, $document.Html, [Text.UTF8Encoding]::new($false))
        if ([IO.File]::Exists($path)) { [IO.File]::Move($temporary, $path, $true) }
        else { [IO.File]::Move($temporary, $path) }
    } finally { if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) } }
    Write-Output "HTML guide: $path"
}
