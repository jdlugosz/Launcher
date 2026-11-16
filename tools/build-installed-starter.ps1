#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SourcePath,
    [Parameter(Mandatory)][string]$OutputPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'installer\PortableProfile.ps1')
$source = [IO.Path]::GetFullPath($SourcePath)
$output = [IO.Path]::GetFullPath($OutputPath)
$text = [IO.File]::ReadAllText($source, [Text.UTF8Encoding]::new($false, $true))
$installed = ConvertTo-LauncherInstalledStarterText -Text $text
$entry = Get-Item -LiteralPath $output -Force -ErrorAction SilentlyContinue
if ($entry -and ($entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint))) {
    throw "Installed starter output must be a regular file: $output"
}
[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($output))
[IO.File]::WriteAllText($output, $installed, [Text.UTF8Encoding]::new($false))
Write-Output "Installed starter: $output"
