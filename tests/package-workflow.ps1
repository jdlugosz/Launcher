[CmdletBinding()]
param(
    [string] $OutputDirectory = (Join-Path ([IO.Path]::GetTempPath()) ('Launcher-package-tests-' + [Guid]::NewGuid().ToString('N'))),
    [Parameter(Mandatory)][string] $CMake
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$repository = Split-Path -Parent $PSScriptRoot
. (Join-Path $repository 'tools\package-state.ps1')
$script:checks = 0
function Check([bool] $Condition, [string] $Message) {
    if (!$Condition) { throw $Message }
    $script:checks++
}
function Invoke-FixtureGit([string[]] $Arguments) {
    & git.exe -C $fixture @Arguments
    if ($LASTEXITCODE -ne 0) { throw 'Fixture Git operation failed.' }
}

$root = [IO.Path]::GetFullPath($OutputDirectory)
[void][IO.Directory]::CreateDirectory($root)
$state = Join-Path $root 'state'
$held = Open-LauncherPackageState $state
try {
    $blocked = $false
    try { $other = Open-LauncherPackageState $state; $other.Dispose() } catch { $blocked = $_.Exception.Message -like '*state lock*' }
    Check $blocked 'Concurrent packaging must fail before reserving another number.'
    $one = New-LauncherPackageReservation $state '1.1.0' 'Prerelease' 'fixture'
    Check ($one.Revision -eq 1) 'First reservation must use revision one.'
    Check ((Get-Content -LiteralPath $one.Path -Raw | ConvertFrom-Json).Status -eq 'Reserved') 'Reservation must be durable before compilation.'
    $one.Receipt.Status = 'Failed'
    Write-LauncherPackageReceipt $one.Path $one.Receipt
    $special = New-LauncherPackageReservation $state '1.2.3' 'Special' 'fixture'
    Check ($special.Revision -eq 0 -and $special.Receipt.Version -eq '1.2.3.0') 'A special update uses the source patch number without a prerelease counter.'
    $two = New-LauncherPackageReservation $state '1.1.0' 'Beta' 'fixture'
    Check ($two.Revision -eq 2 -and $two.Receipt.Kind -ceq 'Beta') 'Beta must advance the same counter as evaluation builds.'
    Remove-Item -LiteralPath (Join-Path $state '1.1.0\counter.json')
    $three = New-LauncherPackageReservation $state '1.1.0' 'Prerelease' 'fixture'
    Check ($three.Revision -eq 3) 'A Beta reservation must prevent number reuse even without the high-water counter.'
    $otherSeries = New-LauncherPackageReservation $state '1.2.3' 'Prerelease' 'fixture'
    Check ($otherSeries.Revision -eq 1) 'A changed source version has its own counter.'
    $lostReceipt = New-LauncherPackageReservation $state '9.9.9' 'Prerelease' 'fixture'
    Remove-Item -LiteralPath $lostReceipt.Path
    $afterLoss = New-LauncherPackageReservation $state '9.9.9' 'Prerelease' 'fixture'
    Check ($afterLoss.Revision -eq 2) 'The durable high-water counter must prevent reuse even if the latest receipt is lost.'
} finally { $held.Dispose() }

$fixture = Join-Path $root 'source'
$callingBuild = Join-Path $fixture 'build\calling'
[void][IO.Directory]::CreateDirectory($callingBuild)
Set-Content -LiteralPath (Join-Path $fixture 'CMakeLists.txt') -Value 'project(Launcher VERSION 1.1.0 LANGUAGES NONE)'
Set-Content -LiteralPath (Join-Path $fixture 'vcpkg.json') -Value '{"version-semver":"1.1.0"}'
Set-Content -LiteralPath (Join-Path $callingBuild 'CMakeCache.txt') -Value @("CMAKE_HOME_DIRECTORY:INTERNAL=$fixture", 'LAUNCHER_VERSION_TEST_HOOKS:BOOL=OFF', 'LAUNCHER_BUILD_INSTRUMENTATION:STRING=', 'VCPKG_MANIFEST_FEATURES:STRING=extra')
Invoke-FixtureGit @('init', '--quiet')
Invoke-FixtureGit @('config', 'user.name', 'Launcher package test')
Invoke-FixtureGit @('config', 'user.email', 'test@example.invalid')
Invoke-FixtureGit @('config', 'commit.gpgsign', 'false')
Invoke-FixtureGit @('add', 'CMakeLists.txt', 'vcpkg.json')
Invoke-FixtureGit @('commit', '--quiet', '-m', 'fixture')
$mock = Join-Path $root 'mock-cmake.ps1'
@'
$ErrorActionPreference = 'Stop'
$arguments = @($args)
if ($arguments[0] -eq '-S') {
    $build = $arguments[3]
    if ($build -like '*\calling') { throw 'Packaging recursed into its calling Ninja tree.' }
    $revisionArgument = @($arguments | Where-Object { $_ -like '-DLAUNCHER_PACKAGE_REVISION=*' })[0]
    $revision = [int]($revisionArgument -replace '^.*=', '')
    $fixtureRoot = $arguments[1]
    $receipt = Join-Path $fixtureRoot ".git\launcher-packages\1.1.0\prerelease-$revision.json"
    if (!(Test-Path -LiteralPath $receipt -PathType Leaf)) { throw 'The revision was not reserved before configuration.' }
    [void][IO.Directory]::CreateDirectory($build)
    Set-Content -LiteralPath (Join-Path $build 'configured.txt') -Value $arguments
    $sourceVersion = [regex]::Match((Get-Content -LiteralPath (Join-Path $fixtureRoot 'CMakeLists.txt') -Raw), 'VERSION ([0-9.]+)').Groups[1].Value
    $parsed = [Version]$sourceVersion
    $display = "$($parsed.Major).$($parsed.Minor)"
    if ($revision -gt 0) { $display = "$sourceVersion.$revision" }
    elseif ($parsed.Build -ne 0) { $display += ".$($parsed.Build)" }
    Set-Content -LiteralPath (Join-Path $build 'display.txt') -Value $display
    if (Test-Path -LiteralPath (Join-Path $fixtureRoot 'fail-next')) { throw 'Intentional configure failure.' }
} elseif ($arguments[0] -eq '--build') {
    if ($arguments[-1] -ne 'LauncherShippingCheck') { throw 'Packaging must check the final archive and installer.' }
    $build = $arguments[1]
    if ((Get-Content -LiteralPath (Join-Path $build 'configured.txt')) -contains '-DLAUNCHER_DISTRIBUTION_KIND=Beta' -and $arguments[-2] -cne 'all') {
        throw 'Beta must build its full test suite together with the shipping archive.'
    }
    $display = (Get-Content -LiteralPath (Join-Path $build 'display.txt') -Raw).Trim()
    $output = Join-Path $build 'shipping\Release'
    [void][IO.Directory]::CreateDirectory($output)
    Set-Content -LiteralPath (Join-Path $output "Launcher-R$display-windows-x64.zip") -Value "unique package $display"
} else { throw 'Unexpected packaging command.' }
$global:LASTEXITCODE = 0
'@ | Set-Content -LiteralPath $mock
$driver = Join-Path $repository 'tools\package-distribution.ps1'
& $driver -Kind Prerelease -SourceDirectory $fixture -CallingBuildDirectory $callingBuild -CMake $mock
$durable = Join-Path $fixture '.git\launcher-packages\1.1.0'
$first = Get-Content -LiteralPath (Join-Path $durable 'prerelease-1.json') -Raw | ConvertFrom-Json
Check ($first.Status -eq 'Complete' -and $first.Version -eq '1.1.0.1') 'The driver must complete its reserved four-part version.'
Check ($first.Sha256 -ceq (Get-FileHash -LiteralPath $first.Archive -Algorithm SHA256).Hash) 'The durable receipt must identify the exact archive bytes.'
$firstHash = $first.Sha256
Set-Content -LiteralPath (Join-Path $fixture 'fail-next') -Value 'fail'
$failed = $false
try { & $driver -Kind Prerelease -SourceDirectory $fixture -CallingBuildDirectory $callingBuild -CMake $mock } catch { $failed = $_.Exception.Message -like '*Intentional configure failure*' }
Check $failed 'An underlying packaging error must propagate.'
$failure = Get-Content -LiteralPath (Join-Path $durable 'prerelease-2.json') -Raw | ConvertFrom-Json
Check ($failure.Status -eq 'Failed') 'Failed packaging must keep its receipt and consume its number.'
Remove-Item -LiteralPath (Join-Path $fixture 'fail-next')
& $driver -Kind Prerelease -SourceDirectory $fixture -CallingBuildDirectory $callingBuild -CMake $mock
$third = Get-Content -LiteralPath (Join-Path $durable 'prerelease-3.json') -Raw | ConvertFrom-Json
Check ($third.Status -eq 'Complete' -and $third.Version -eq '1.1.0.3') 'The next package must skip the failed reservation.'
Check ($firstHash -ceq (Get-FileHash -LiteralPath $first.Archive -Algorithm SHA256).Hash) 'A later package must not replace any prior archive.'
Check ($third.BuildDirectory -ne $first.BuildDirectory -and $third.BuildDirectory -ne $callingBuild) 'Each package needs its own compilation tree.'

$mockCTest = Join-Path $root 'mock-ctest.ps1'
@'
$ErrorActionPreference = 'Stop'
if ($args.Count -ne 6 -or $args[0] -cne '--test-dir' -or ($args[2..5] -join ' ') -cne '-C Release --output-on-failure --no-tests=error') {
    throw 'Beta must run unfiltered Release CTest and require discovered tests.'
}
$build = $args[1]
$configuration = Get-Content -LiteralPath (Join-Path $build 'configured.txt')
foreach ($option in @('-DLAUNCHER_DISTRIBUTION_KIND=Beta', '-DLAUNCHER_RELEASE_PACKAGE=OFF', '-DBUILD_TESTING=ON', '-DVCPKG_MANIFEST_FEATURES=extra;tests')) {
    if ($configuration -cnotcontains $option) { throw "Missing Beta configuration option: $option" }
}
$display = (Get-Content -LiteralPath (Join-Path $build 'display.txt') -Raw).Trim()
if (!(Test-Path -LiteralPath (Join-Path $build "shipping\Release\Launcher-R$display-windows-x64.zip"))) { throw 'Beta tested before building the candidate.' }
$global:LASTEXITCODE = if ($display -eq '1.1.0.4') { 7 } else { 0 }
'@ | Set-Content -LiteralPath $mockCTest
$failed = $false
try { & $driver -Kind Beta -SourceDirectory $fixture -CallingBuildDirectory $callingBuild -CMake $mock -CTest $mockCTest } catch { $failed = $_.Exception.Message -like '*Package command failed (7)*' }
Check $failed 'A failed Beta CTest run must fail the distribution.'
$failedBeta = Get-Content -LiteralPath (Join-Path $durable 'prerelease-4.json') -Raw | ConvertFrom-Json
Check ($failedBeta.Status -ceq 'Failed' -and $failedBeta.Kind -ceq 'Beta' -and !$failedBeta.Archive) 'An unverified Beta must leave a failed reservation without a publishable archive path.'
& $driver -Kind Beta -SourceDirectory $fixture -CallingBuildDirectory $callingBuild -CMake $mock -CTest $mockCTest
$beta = Get-Content -LiteralPath (Join-Path $durable 'prerelease-5.json') -Raw | ConvertFrom-Json
Check ($beta.Status -ceq 'Complete' -and $beta.Kind -ceq 'Beta' -and $beta.Version -ceq '1.1.0.5') 'Beta must complete only after full CTest passes.'

# The real release gate must reject this odd development version before any
# release reservation is created. This test never creates or signs a tag.
$rejected = $false
try { & $driver -Kind Release -SourceDirectory $fixture -CallingBuildDirectory $callingBuild -CMake $CMake } catch { $rejected = $_.Exception.Message -like '*Package command failed*' }
Check $rejected 'The final package target must enforce the existing release gate.'
Check (@(Get-ChildItem -LiteralPath $durable -Filter 'release-*.json').Count -eq 0) 'A rejected release must not reserve or build a final package.'

Set-Content -LiteralPath (Join-Path $fixture 'CMakeLists.txt') -Value 'project(Launcher VERSION 1.2.3 LANGUAGES NONE)'
Set-Content -LiteralPath (Join-Path $fixture 'vcpkg.json') -Value '{"version-semver":"1.2.3"}'
# Special updates retain a patch version and get unique output directories.
# The mock checks the appropriate kind-specific reservation before configure.
(Get-Content -LiteralPath $mock -Raw).Replace('".git\launcher-packages\1.1.0\prerelease-$revision.json"', '".git\launcher-packages\1.2.3"').Replace('if (!(Test-Path -LiteralPath $receipt -PathType Leaf))', 'if (@(Get-ChildItem -LiteralPath $receipt -Filter ''special-*.json'').Count -eq 0)') | Set-Content -LiteralPath $mock
& $driver -Kind Prerelease -DistributionKind Special -SourceDirectory $fixture -CallingBuildDirectory $callingBuild -CMake $mock
$specialReceipts = Join-Path $fixture '.git\launcher-packages\1.2.3'
$firstSpecial = Get-Content -LiteralPath @(Get-ChildItem -LiteralPath $specialReceipts -Filter 'special-*.json')[0].FullName -Raw | ConvertFrom-Json
Check ($firstSpecial.Version -eq '1.2.3.0' -and $firstSpecial.Kind -eq 'Special') 'Special driver output must keep the patch version and explicit kind.'
Check ((Split-Path -Leaf $firstSpecial.Archive) -eq 'Launcher-R1.2.3-windows-x64.zip') 'A special update filename must use three components.'
& $driver -Kind Prerelease -DistributionKind Special -SourceDirectory $fixture -CallingBuildDirectory $callingBuild -CMake $mock
Check (@(Get-ChildItem -LiteralPath $specialReceipts -Filter 'special-*.json').Count -eq 2) 'Repeated special packages need distinct permanent receipts.'
Check ($firstSpecial.Sha256 -ceq (Get-FileHash -LiteralPath $firstSpecial.Archive -Algorithm SHA256).Hash) 'Another special package must preserve the older output.'
Write-Output "Package workflow passed: $script:checks checks. Evidence: $root"
