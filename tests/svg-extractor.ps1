[CmdletBinding()]
param([string] $FixtureDirectory, [string] $OutputDirectory, [string] $ExecutablePath)
$ErrorActionPreference = 'Stop'
$svgTestProject = Split-Path -Parent $PSScriptRoot
if (!$OutputDirectory) { $OutputDirectory = Join-Path $svgTestProject ('build\svg-extractor-tests\' + [guid]::NewGuid().ToString('N')) }
$svgTestOutput = [IO.Path]::GetFullPath($OutputDirectory)
$null = New-Item -ItemType Directory -Path $svgTestOutput
if (!$FixtureDirectory) {
    & (Join-Path $PSScriptRoot 'advanced-icon-catalog.ps1') -OutputDirectory (Join-Path $svgTestOutput 'catalog')
    if ($LASTEXITCODE -ne 0) { throw 'Catalog fixture checks failed.' }
    $FixtureDirectory = Join-Path $svgTestOutput 'catalog\fixtures'
}
$svgTestCompiler = Join-Path ([Environment]::GetFolderPath('Windows')) 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$svgTestExe = Join-Path $svgTestOutput 'SvgExtractorProbe.exe'
$svgTestSource = Join-Path $svgTestProject 'tools\advanced-icon-catalog'
& $svgTestCompiler /nologo /optimize+ /warnaserror+ /target:exe /platform:anycpu /main:SvgExtractorProbe "/out:$svgTestExe" `
    "/win32manifest:$(Join-Path $svgTestSource 'svg-extractor.manifest')" "/win32icon:$(Join-Path $svgTestProject 'resources\launcher.ico')" `
    /reference:System.Core.dll /reference:System.Xml.dll /reference:System.Xml.Linq.dll /reference:System.IO.Compression.dll `
    /reference:System.IO.Compression.FileSystem.dll /reference:System.Web.Extensions.dll /reference:System.Windows.Forms.dll /reference:System.Drawing.dll `
    (Join-Path $svgTestSource 'AdvancedIconCatalog.cs') (Join-Path $svgTestSource 'SvgExtractor.cs') (Join-Path $svgTestSource 'SvgGalleryServer.cs') (Join-Path $PSScriptRoot 'svg_extractor_probe.cs')
if ($LASTEXITCODE -ne 0) { throw 'SVG extractor probe compilation failed.' }
$svgTestArguments = @([IO.Path]::GetFullPath($FixtureDirectory), (Join-Path $svgTestOutput 'results'))
if ($ExecutablePath) {
    $svgTestExecutable = Get-Item -LiteralPath $ExecutablePath -Force -ErrorAction Stop
    if ($svgTestExecutable.PSIsContainer -or ($svgTestExecutable.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'The exact SVG Extractor test binary must be a regular executable.' }
    $svgTestArguments += $svgTestExecutable.FullName
}
& $svgTestExe @svgTestArguments
if ($LASTEXITCODE -ne 0) { throw 'SVG extractor checks failed.' }
if (Get-Command node -ErrorAction SilentlyContinue) {
    & node (Join-Path $PSScriptRoot 'svg-gallery.cjs') (Join-Path $svgTestOutput 'results\results\gallery-with-keep.html')
    if ($LASTEXITCODE -ne 0) { throw 'SVG gallery checks failed.' }
}
$global:LASTEXITCODE = 0
