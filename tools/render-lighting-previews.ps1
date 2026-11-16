[CmdletBinding()]
param(
    [string] $LauncherPath,
    [string] $OutputDirectory,
    [Alias('FixturePath')] [string] $ConfigurationPath,
    [ValidateSet('top', 'right', 'bottom', 'left')] [string[]] $Edges = @('top', 'right'),
    [int[]] $Pages = @(0)
)

$ErrorActionPreference = 'Stop'
$previewProject = Split-Path -Parent $PSScriptRoot
if (!$LauncherPath) { $LauncherPath = Join-Path $previewProject 'build\windows-x64\Release\Launcher.exe' }
if (!$ConfigurationPath) { $ConfigurationPath = Join-Path $previewProject 'examples\preview.toml' }
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $previewProject ('build\previews\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
}
$previewExecutable = [IO.Path]::GetFullPath($LauncherPath)
$previewConfiguration = [IO.Path]::GetFullPath($ConfigurationPath)
$previewOutput = [IO.Path]::GetFullPath($OutputDirectory)
foreach ($previewInput in @($previewExecutable, $previewConfiguration)) {
    if (!(Test-Path -LiteralPath $previewInput -PathType Leaf)) { throw "Preview input is missing: $previewInput" }
    if ($previewInput.Contains('"')) { throw 'Preview input paths cannot contain a double quote.' }
}
if ($previewOutput.Contains('"')) { throw 'OutputDirectory cannot contain a double quote.' }
if ($Pages | Where-Object { $_ -lt 0 }) { throw 'Preview page indexes must be nonnegative.' }
if (!$Edges.Count -or !$Pages.Count) { throw 'Choose at least one edge and page.' }
$null = New-Item -ItemType Directory -Path $previewOutput -Force
foreach ($previewEdge in $Edges) {
    foreach ($previewPage in $Pages) {
        $previewPng = Join-Path $previewOutput ("prism-$previewEdge-page$previewPage.png")
        if (Test-Path -LiteralPath $previewPng) { throw "Preview already exists: $previewPng" }
        # Pass the original configuration: relative icons and adjacent color
        # files retain their normal meaning. The native override only changes
        # the in-memory edge; no configuration or runtime file is rewritten.
        $previewArguments = @('--config', ('"' + $previewConfiguration + '"'),
            '--preview-edge', $previewEdge, '--preview-page', $previewPage,
            '--render-preview', ('"' + $previewPng + '"'))
        $previewProcess = Start-Process -FilePath $previewExecutable -ArgumentList $previewArguments -WindowStyle Hidden -Wait -PassThru
        if ($previewProcess.ExitCode -ne 0) { throw "Preview failed: $previewEdge/page$previewPage" }
        Write-Output $previewPng
    }
}
