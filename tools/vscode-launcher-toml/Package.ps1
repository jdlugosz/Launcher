[CmdletBinding()]
param([string]$OutputDirectory)

$ErrorActionPreference = 'Stop'
if (!$OutputDirectory) { $OutputDirectory = Join-Path $PSScriptRoot '..\..\build\vscode-extension' }
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$package = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'package.json') -Raw | ConvertFrom-Json
$output = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($output) | Out-Null
$target = Join-Path $output "$($package.name)-$($package.version).vsix"
$files = @('package.json', 'extension.js', 'pragmas.js', 'language-configuration.json', 'README.md',
    'syntaxes/launcher-toml.tmLanguage.json', 'syntaxes/pragma.tmLanguage.json')
$manifest = @"
<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011">
  <Metadata>
    <Identity Language="en-US" Id="$($package.name)" Version="$($package.version)" Publisher="$($package.publisher)" />
    <DisplayName>$($package.displayName)</DisplayName>
    <Description xml:space="preserve">$($package.description)</Description>
    <Properties>
      <Property Id="Microsoft.VisualStudio.Code.Engine" Value="$($package.engines.vscode)" />
      <Property Id="Microsoft.VisualStudio.Code.ExtensionDependencies" Value="tamasfe.even-better-toml" />
    </Properties>
  </Metadata>
  <Installation><InstallationTarget Id="Microsoft.VisualStudio.Code" /></Installation>
  <Dependencies />
  <Assets><Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true" /></Assets>
</PackageManifest>
"@
$contentTypes = @'
<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="json" ContentType="application/json" />
  <Default Extension="js" ContentType="application/javascript" />
  <Default Extension="md" ContentType="text/markdown" />
  <Default Extension="vsixmanifest" ContentType="text/xml" />
</Types>
'@
$archive = [IO.Compression.ZipFile]::Open($target, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($name in $files) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, (Join-Path $PSScriptRoot $name), "extension/$name") | Out-Null
    }
    foreach ($item in @(@('extension.vsixmanifest', $manifest), @('[Content_Types].xml', $contentTypes))) {
        $entry = $archive.CreateEntry($item[0])
        $writer = [IO.StreamWriter]::new($entry.Open(), [Text.UTF8Encoding]::new($false))
        try { $writer.Write($item[1]) } finally { $writer.Dispose() }
    }
} finally { $archive.Dispose() }
Get-Item -LiteralPath $target | Select-Object FullName, Length
