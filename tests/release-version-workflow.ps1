#requires -Version 5.1
[CmdletBinding()]
param([string]$OutputDirectory = [IO.Path]::GetTempPath())
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$scriptPath = Join-Path $PSScriptRoot '..\tools\set-release-version.ps1'
$fixture = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('launcher-version-workflow-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($fixture)
$cmakePath = Join-Path $fixture 'CMakeLists.txt'
$manifestPath = Join-Path $fixture 'vcpkg.json'
[IO.File]::WriteAllText($cmakePath, "project(Launcher VERSION 1.1.0 LANGUAGES CXX RC)`n# retained text`n")
[IO.File]::WriteAllText($manifestPath, "{`n  `"name`": `"launcher`",`n  `"version-semver`": `"1.1.0`"`n}`n")
$checks = 0
function Check([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message }; $script:checks++ }
function Reject([scriptblock]$Run) { $failed = $false; try { & $Run | Out-Null } catch { $failed = $true }; Check $failed 'An invalid version transition was accepted.' }
$release = & $scriptPath -RepositoryDirectory $fixture -Action PrepareRelease
Check ($release.PreviousVersion -ceq '1.1.0' -and $release.Version -ceq '1.2.0') 'Release promotion did not select the next even minor.'
Check ([IO.File]::ReadAllText($cmakePath).Contains('# retained text')) 'Version preparation changed unrelated source text.'
Check ((Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json).'version-semver' -ceq '1.2.0') 'Manifest version did not follow CMake.'
Reject { & $scriptPath -RepositoryDirectory $fixture -Action PrepareRelease }
$development = & $scriptPath -RepositoryDirectory $fixture -Action NextDevelopment
Check ($development.Version -ceq '1.3.0') 'Development did not resume on the next odd minor.'
Reject { & $scriptPath -RepositoryDirectory $fixture -Action NextDevelopment }
Reject { & $scriptPath -RepositoryDirectory $fixture -Action Set -Version '1.3.0.8' }
Reject { & $scriptPath -RepositoryDirectory $fixture -Action PrepareRelease -Version '1.5.0' }
Reject { & $scriptPath -RepositoryDirectory $fixture -Action Set -Version '1.2.3' }
$patch = & $scriptPath -RepositoryDirectory $fixture -Action PrepareRelease -Version '1.4.3'
Check ($patch.Version -ceq '1.4.3') 'Explicit release patch version was not accepted.'
Reject { & $scriptPath -RepositoryDirectory $fixture -Action Set -Version '1.65536.0' }
Check (@(Get-ChildItem -LiteralPath $fixture -Filter '.launcher-version-*').Count -eq 0) 'Version preparation left temporary source files.'
$before = [IO.File]::ReadAllText($cmakePath)
[IO.File]::WriteAllText($manifestPath, '{"name":"launcher","version-semver":"1.0.0"}')
Reject { & $scriptPath -RepositoryDirectory $fixture -Action NextDevelopment }
Check ([IO.File]::ReadAllText($cmakePath) -ceq $before) 'A mismatched manifest changed the CMake source.'
[IO.File]::WriteAllText($manifestPath, '{"name":"launcher","version-semver":"1.4.3"}')
$manifestBefore = [IO.File]::ReadAllText($manifestPath)
$lock = [IO.File]::Open($manifestPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try { Reject { & $scriptPath -RepositoryDirectory $fixture -Action NextDevelopment } }
finally { $lock.Dispose() }
Check ([IO.File]::ReadAllText($cmakePath) -ceq $before) 'A failed manifest update did not roll back the CMake version.'
Check ([IO.File]::ReadAllText($manifestPath) -ceq $manifestBefore) 'A failed update changed the locked manifest.'
Check (@(Get-ChildItem -LiteralPath $fixture -Filter '.launcher-version-*').Count -eq 0) 'A failed version update left recovery artifacts after successful rollback.'
Write-Output "Release version workflow passed: $checks checks; fixture: $fixture"
