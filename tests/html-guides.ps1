[CmdletBinding()]
param([string]$OutputDirectory = [IO.Path]::GetTempPath(), [string]$SourceDirectory)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$SourceDirectory) { $SourceDirectory = Split-Path -Parent $PSScriptRoot }
$source = [IO.Path]::GetFullPath($SourceDirectory)
$builder = Join-Path $source 'tools\build-html-guides.ps1'
$run = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('html-guides-' + [Guid]::NewGuid().ToString('N'))
$script:checks = 0
function Check([bool]$Condition, [string]$Message) { $script:checks++; if (!$Condition) { throw $Message } }
function WriteText([string]$Path, [string]$Text) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}
function Reject([scriptblock]$Action, [string]$Reason) {
    $message = ''
    try { & $Action | Out-Null } catch { $message = $_.Exception.Message }
    Check ($message.Contains($Reason)) ("Expected '$Reason'; received '$message'.")
}
$sources = @('README.md', 'tools/drop-target/README.md', 'tools/installer/README.md',
    'docs/svg-extractor.md', 'docs/advanced-icon-catalog.md', 'docs/file-locations.md',
    'docs/configuration.md', 'docs/launch-targets.md', 'docs/appearance.md')
$expected = @('Launcher.html', 'LauncherDrop.html', 'Installer.html', 'SvgExtractor.html',
    'advanced-icon-catalog.html', 'FileLocations.html', 'Configuration.html', 'LaunchTargets.html', 'Appearance.html')
$before = @{}
foreach ($relative in $sources) { $before[$relative] = (Get-FileHash -LiteralPath (Join-Path $source $relative) -Algorithm SHA256).Hash }

function Prepare-Payload([string]$Directory) {
    foreach ($relative in @('basic_colors.toml', 'extra_colors.toml', 'colors-light.toml', 'colors-dark.toml')) {
        WriteText (Join-Path $Directory ('examples\' + $relative)) ([IO.File]::ReadAllText((Join-Path $source $relative)))
    }
    foreach ($relative in @('examples/demo_many_colors.toml', 'examples/demo_many_colors.cmd')) {
        WriteText (Join-Path $Directory $relative) ([IO.File]::ReadAllText((Join-Path $source $relative)))
    }
    WriteText (Join-Path $Directory 'Launcher.default.toml') ([IO.File]::ReadAllText((Join-Path $source 'Launcher.toml')))
    WriteText (Join-Path $Directory 'LICENSE.txt') 'Fixture selected license'
    WriteText (Join-Path $Directory 'THIRD-PARTY-NOTICES.txt') 'Fixture dependency notices'
}
function Verify-Documents([string]$Directory) {
    $files = @(Get-ChildItem -LiteralPath $Directory -File -Force)
    Check ($files.Count -eq $expected.Count) 'The generated docs directory contains extra runtime, script, or sidecar files.'
    foreach ($name in $expected) {
        $path = Join-Path $Directory $name
        Check ([IO.File]::Exists($path)) "The HTML guide is missing: $name"
        $html = [IO.File]::ReadAllText($path)
        Check ($html.StartsWith('<!doctype html>') -and $html.Contains('<html lang="en">') -and $html.Contains('<meta charset="utf-8">')) "The guide is not a standalone UTF-8 document: $name"
        Check ($html.Contains('<style>') -and $html.Contains('@media print') -and $html.Contains('@media(max-width:540px)')) "Responsive or print styles are missing: $name"
        Check ($html.Contains('aria-label="On this page"')) "The guide has no heading contents navigation: $name"
        Check ($html -notmatch '(?i)<(?:script|link|img|iframe|object|embed)\b|@import|url\s*\(') "The guide requires executable or fetched rendering resources: $name"
        foreach ($match in [regex]::Matches($html, 'href="([^"]+)"')) {
            $href = [Net.WebUtility]::HtmlDecode($match.Groups[1].Value)
            if ($href -match '^(?i:https?://|mailto:)') { continue }
            Check ($href -notmatch '^//|^[A-Za-z][A-Za-z0-9+.-]*:') "The guide has an unsupported link scheme: $name $href"
            $parts = $href.Split('#', 2)
            $target = if ($parts[0]) { [IO.Path]::GetFullPath((Join-Path $Directory ([Uri]::UnescapeDataString($parts[0])))) } else { $path }
            Check ([IO.File]::Exists($target)) "A bundled local link is broken: $name $href"
            if ($parts.Count -gt 1 -and $parts[1] -and [IO.Path]::GetExtension($target) -ieq '.html') {
                $fragment = [Uri]::UnescapeDataString($parts[1])
                $targetHtml = [IO.File]::ReadAllText($target)
                $ids = @([regex]::Matches($targetHtml, '\bid="([^"]+)"') | ForEach-Object { [Net.WebUtility]::HtmlDecode($_.Groups[1].Value) })
                Check ($ids -ccontains $fragment) "A bundled heading link is broken: $name $href"
            }
        }
    }
}

$actual = Join-Path $run 'actual payload'
Prepare-Payload $actual
$actualDocs = Join-Path $actual 'docs'
& $builder -SourceDirectory $source -OutputDirectory $actualDocs | Out-Null
Verify-Documents $actualDocs
$main = [IO.File]::ReadAllText((Join-Path $actualDocs 'Launcher.html'))
Check ($main.Contains('<pre><code class="language-toml">')) 'The Launcher quick start lost its TOML example.'
foreach ($name in @('Configuration.html', 'LaunchTargets.html', 'Appearance.html')) {
    Check ($main.Contains('href="' + $name + '"')) "The Launcher guide does not link to $name offline."
}
Check ($main.Contains('href="FileLocations.html#read-search-order"') -and $main.Contains('href="SvgExtractor.html"')) 'Real bundled guide links were not mapped to their HTML counterparts.'
Check ($main.Contains('(source checkout:') -and $main -notmatch 'href="(?:Agile-Well\.md|examples/Adobe-CC\.toml|docs/version-discovery-design\.md)') 'Unbundled source material remains a broken download link.'
$drop = [IO.File]::ReadAllText((Join-Path $actualDocs 'LauncherDrop.html'))
Check ($drop.Contains('href="FileLocations.html"')) 'A guide nested in the source tree did not map back to the included locations guide.'

# Copy only the Markdown inputs; synthetic additions exercise the engine
# without changing source documents or requiring a checkout beside the output.
$fixture = Join-Path $run ('source ' + [char]0x5DE5 + [char]0x5177)
foreach ($relative in $sources) { WriteText (Join-Path $fixture $relative) ([IO.File]::ReadAllText((Join-Path $source $relative))) }
$fixtureMain = Join-Path $fixture 'README.md'
$unicodeHeading = [string][char]0x5DE5 + [char]0x5177 + ' and paths'
$addition = @'

## Renderer test section

[This section](#renderer-test-section), [SVG usage](docs/svg-extractor.md#use-the-utility),
[Basic palette](basic_colors.toml), [Sampler](examples/demo_many_colors.toml),
[Starter](Launcher.toml), [License](LICENSE.txt), [Code](src/main.cpp).

| Field | Value |
| --- | --- |
| `target` | `<literal>` & text |

```toml
{ target = '$EXE\LauncherDrop.exe', arguments = '<literal & value>' }
```

## Duplicate test
## Duplicate test
'@
$fixtureText = [IO.File]::ReadAllText($fixtureMain) + $addition + "`n## $unicodeHeading`n"
WriteText $fixtureMain $fixtureText
$synthetic = Join-Path $run 'synthetic payload'
Prepare-Payload $synthetic
$syntheticDocs = Join-Path $synthetic 'docs'
& $builder -SourceDirectory $fixture -OutputDirectory $syntheticDocs | Out-Null
Verify-Documents $syntheticDocs
$rendered = [IO.File]::ReadAllText((Join-Path $syntheticDocs 'Launcher.html'))
Check ($rendered.Contains($unicodeHeading)) 'Unicode heading text was corrupted.'
Check ($rendered.Contains('id="duplicate-test"') -and $rendered.Contains('id="duplicate-test-1"')) 'The Markdown engine did not preserve unique duplicate-heading anchors.'
Check ($rendered.Contains('href="../examples/basic_colors.toml"') -and $rendered.Contains('href="../examples/demo_many_colors.toml"')) 'Known bundled examples did not map to the actual examples directory.'
Check ($rendered.Contains('href="../Launcher.default.toml"') -and $rendered.Contains('href="../LICENSE.txt"')) 'Renamed starter or selected license links do not match the payload.'
Check ($rendered.Contains('&lt;literal &amp; value&gt;') -and $rendered.Contains('<code>target</code>')) 'Code or table inline markup was interpreted as raw HTML.'
Check ($rendered -notmatch 'href="src/main\.cpp"' -and $rendered.Contains('(source checkout: <code>src/main.cpp</code>)')) 'Unbundled implementation reference was not visibly identified.'

$stableHashes = @{}
foreach ($name in $expected) { $stableHashes[$name] = (Get-FileHash -LiteralPath (Join-Path $syntheticDocs $name) -Algorithm SHA256).Hash }
WriteText $fixtureMain ($fixtureText + "`n[Broken included anchor](docs/svg-extractor.md#missing-renderer-test-heading)`n")
Reject { & $builder -SourceDirectory $fixture -OutputDirectory $syntheticDocs } 'Missing heading anchor'
foreach ($name in $expected) { Check ((Get-FileHash -LiteralPath (Join-Path $syntheticDocs $name) -Algorithm SHA256).Hash -eq $stableHashes[$name]) 'Failed rendering partially replaced the previous guide set.' }
WriteText $fixtureMain ($fixtureText + "`n![Unavailable preview](missing-preview.svg)`n")
Reject { & $builder -SourceDirectory $fixture -OutputDirectory $syntheticDocs } 'Unsupported embedded asset'
WriteText $fixtureMain ($fixtureText + "`n<script src='https://example.invalid/renderer.js'></script>`n")
Reject { & $builder -SourceDirectory $fixture -OutputDirectory $syntheticDocs } 'Unsupported embedded asset'
WriteText $fixtureMain $fixtureText
foreach ($relative in $sources) { Check ((Get-FileHash -LiteralPath (Join-Path $source $relative) -Algorithm SHA256).Hash -eq $before[$relative]) 'Rendering modified a source Markdown document.' }

# Windows PowerShell remains sufficient for the installer; guide generation
# must fail clearly there, before attempting to create or publish any output.
$windows = [Environment]::GetFolderPath('Windows')
if ($windows) {
    $legacy = Join-Path $windows 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if ([IO.File]::Exists($legacy)) {
        $unsupportedOutput = Join-Path $run 'unsupported engine'
        $message = (& $legacy -NoProfile -ExecutionPolicy Bypass -File $builder -SourceDirectory $source -OutputDirectory $unsupportedOutput 2>&1 | Out-String)
        Check ($LASTEXITCODE -ne 0 -and $message.Contains('requires PowerShell 7 with ConvertFrom-Markdown')) 'The build did not explain its missing Markdown engine.'
        Check (!(Test-Path -LiteralPath $unsupportedOutput)) 'An unsupported engine created partial HTML output.'
    }
}
Write-Output "HTML guide checks passed: $script:checks; generated guides: $actualDocs; fixtures: $run"
