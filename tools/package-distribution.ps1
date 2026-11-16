[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Prerelease', 'Beta', 'Release')][string] $Kind,
    [string] $SourceDirectory = (Split-Path -Parent $PSScriptRoot),
    [Parameter(Mandatory)][string] $CallingBuildDirectory,
    [string] $CMake = 'cmake',
    [string] $CTest,
    [string] $Git = 'git',
    [ValidateSet('Prerelease', 'Special')][string] $DistributionKind = 'Prerelease',
    [string] $StateDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'package-state.ps1')
$CMake = (Get-Command $CMake -ErrorAction Stop).Source
$Git = (Get-Command $Git -ErrorAction Stop).Source
$actualKind = $DistributionKind
if ($Kind -eq 'Beta') { $actualKind = 'Beta' }

function Invoke-PackageTool([string] $Program, [string[]] $Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Package command failed ($LASTEXITCODE): $Program $($Arguments -join ' ')" }
}

$source = [IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\', '/')
$callingBuild = [IO.Path]::GetFullPath($CallingBuildDirectory).TrimEnd('\', '/')
$cacheFile = Join-Path $callingBuild 'CMakeCache.txt'
if (!(Test-Path -LiteralPath $cacheFile -PathType Leaf)) { throw 'CallingBuildDirectory must be a configured CMake build tree.' }
$cache = @{}
foreach ($line in Get-Content -LiteralPath $cacheFile) {
    if ($line -match '^([^#/:][^:]*):[^=]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
}
if (!$cache.ContainsKey('CMAKE_HOME_DIRECTORY') -or [IO.Path]::GetFullPath($cache.CMAKE_HOME_DIRECTORY).TrimEnd('\', '/') -ne $source) {
    throw 'The calling build tree belongs to another source directory.'
}
if ($cache.LAUNCHER_VERSION_TEST_HOOKS -eq 'ON' -or ($cache.ContainsKey('LAUNCHER_BUILD_INSTRUMENTATION') -and $cache.LAUNCHER_BUILD_INSTRUMENTATION)) {
    throw 'Instrumented build trees cannot produce distributions.'
}
$cmakeText = Get-Content -LiteralPath (Join-Path $source 'CMakeLists.txt') -Raw
if ($cmakeText -notmatch '(?m)^project\(Launcher VERSION ([0-9]+\.[0-9]+\.[0-9]+)\s') { throw 'Cannot read the three-part source version.' }
$version = $Matches[1]
$manifest = Get-Content -LiteralPath (Join-Path $source 'vcpkg.json') -Raw | ConvertFrom-Json
if ($manifest.'version-semver' -cne $version) { throw 'CMake and vcpkg source versions disagree.' }
$commit = (& $Git -C $source rev-parse --verify HEAD | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[0-9a-f]{40,64}$') { throw 'Packaging requires an identifiable Git commit.' }
if ($Kind -eq 'Release') {
    Invoke-PackageTool $CMake @("-DSOURCE_DIR=$source", "-DRELEASE_VERSION=$version", "-DGIT_EXECUTABLE=$Git", '-P', (Join-Path $PSScriptRoot 'verify-release.cmake'))
    $actualKind = 'Release'
}
$buildTesting = 'OFF'
if ($actualKind -eq 'Beta') {
    if (!$CTest) { $CTest = Join-Path (Split-Path -Parent $CMake) 'ctest.exe' }
    $CTest = (Get-Command $CTest -ErrorAction Stop).Source
    $features = @($cache['VCPKG_MANIFEST_FEATURES'] -split ';' | Where-Object { $_ })
    if ($features -notcontains 'tests') { $features += 'tests' }
    $cache.VCPKG_MANIFEST_FEATURES = $features -join ';'
    $buildTesting = 'ON'
}
if (!$StateDirectory) {
    $common = (& $Git -C $source rev-parse --path-format=absolute --git-common-dir | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or !$common) { throw 'Cannot locate the durable Git package state directory.' }
    $StateDirectory = Join-Path $common 'launcher-packages'
}
$stateLock = Open-LauncherPackageState $StateDirectory
$reservation = $null
try {
    $reservation = New-LauncherPackageReservation $StateDirectory $version $actualKind $commit
    $receipt = $reservation.Receipt
    $display = "$(([Version]$version).Major).$(([Version]$version).Minor)"
    if ($reservation.Revision -gt 0) { $display = "$version.$($reservation.Revision)" }
    elseif (([Version]$version).Build -ne 0) { $display += ".$(([Version]$version).Build)" }
    $buildName = "$actualKind-$display"
    if ($reservation.Revision -eq 0) { $buildName += '-' + [Guid]::NewGuid().ToString('N') }
    $build = Join-Path $source ('build\packages\' + $buildName)
    if (Test-Path -LiteralPath $build) { throw "Reserved package build directory already exists; no output will be replaced: $build" }
    $receipt.BuildDirectory = $build
    Write-LauncherPackageReceipt $reservation.Path $receipt
    Write-Output "Reserved $actualKind $($receipt.Version); receipt: $($reservation.Path)"
    $arguments = @('-S', $source, '-B', $build, '-G', 'Ninja Multi-Config', '-DCMAKE_CONFIGURATION_TYPES=Release', '-DCMAKE_DEFAULT_BUILD_TYPE=Release',
        "-DBUILD_TESTING=$buildTesting", "-DLAUNCHER_PACKAGE_REVISION=$($reservation.Revision)", '-DLAUNCHER_BUILD_INSTRUMENTATION=', '-DLAUNCHER_VERSION_TEST_HOOKS=OFF')
    foreach ($name in @('CMAKE_CXX_COMPILER', 'CMAKE_MAKE_PROGRAM', 'CMAKE_TOOLCHAIN_FILE', 'VCPKG_TARGET_TRIPLET', 'VCPKG_HOST_TRIPLET',
        'VCPKG_INSTALLED_DIR', 'VCPKG_EXECUTABLE', 'VCPKG_MANIFEST_FEATURES', 'LAUNCHER_USE_STD_MODULE',
        'LAUNCHER_UPDATER_INFO_URL', 'LAUNCHER_UPDATER_SETUP_URL', 'LAUNCHER_UPDATER_SIGNER_SHA256',
        'LAUNCHER_ARCHIVE_SIGNING_CERTIFICATE', 'LAUNCHER_ARCHIVE_ADDITIONAL_SIGNING_CERTIFICATE')) {
        if ($cache.ContainsKey($name) -and $cache[$name]) { $arguments += "-D$name=$($cache[$name])" }
    }
    if ($Kind -eq 'Release') { $arguments += '-DLAUNCHER_RELEASE_PACKAGE=ON' }
    else { $arguments += @('-DLAUNCHER_RELEASE_PACKAGE=OFF', "-DLAUNCHER_DISTRIBUTION_KIND=$actualKind") }
    Invoke-PackageTool $CMake $arguments
    $buildArguments = @('--build', $build, '--config', 'Release', '--target')
    # Build once so CTest exercises the same stamped binaries that go in the ZIP.
    if ($actualKind -eq 'Beta') { $buildArguments += 'all' }
    Invoke-PackageTool $CMake ($buildArguments + 'LauncherShippingCheck')
    if ($actualKind -eq 'Beta') {
        Invoke-PackageTool $CTest @('--test-dir', $build, '-C', 'Release', '--output-on-failure', '--no-tests=error')
    }
    $archive = Join-Path $build "shipping\Release\Launcher-R$display-windows-x64.zip"
    if (!(Test-Path -LiteralPath $archive -PathType Leaf)) { throw "Checked package archive is missing: $archive" }
    $receipt.Status = 'Complete'
    $receipt.Archive = $archive
    $receipt.Sha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    $receipt.CompletedUtc = [DateTime]::UtcNow.ToString('o')
    Write-LauncherPackageReceipt $reservation.Path $receipt
    Write-Output "Checked $actualKind ZIP: $archive"
    Write-Output "SHA-256: $($receipt.Sha256)"
} catch {
    if ($reservation) {
        $reservation.Receipt.Status = 'Failed'
        $reservation.Receipt.Failure = $_.Exception.Message
        try { Write-LauncherPackageReceipt $reservation.Path $reservation.Receipt } catch { Write-Warning "Could not update the permanent reservation receipt: $_" }
    }
    throw
} finally { $stateLock.Dispose() }
