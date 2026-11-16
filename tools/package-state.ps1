# Durable reservations live in Git's common directory so cleaning build/ never
# reuses a distributed prerelease or Beta number, including across linked worktrees.
Set-StrictMode -Version 2.0

function Open-LauncherPackageState([string] $Directory) {
    $full = [IO.Path]::GetFullPath($Directory)
    $ancestor = $full
    while ($ancestor) {
        $entry = Get-Item -LiteralPath $ancestor -Force -ErrorAction SilentlyContinue
        if ($entry -and ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Package state cannot cross a reparse point: $ancestor" }
        $ancestor = [IO.Path]::GetDirectoryName($ancestor)
    }
    [void][IO.Directory]::CreateDirectory($full)
    try { return [IO.File]::Open((Join-Path $full 'packaging.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None) }
    catch { throw "Another package operation holds the state lock, or it is inaccessible: $full. $($_.Exception.Message)" }
}

function Write-LauncherPackageReceipt([string] $Path, [object] $Receipt, [switch] $Create) {
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Receipt | ConvertTo-Json -Depth 8) + "`n")
    $temporary = $Path + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    $stream = $null
    try {
        $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
        $stream.Dispose()
        $stream = $null
        if ($Create) { [IO.File]::Move($temporary, $Path) }
        else { [IO.File]::Replace($temporary, $Path, [NullString]::Value) }
    } finally {
        if ($stream) { $stream.Dispose() }
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
}

# Caller must hold Open-LauncherPackageState until the package finishes. Each
# reserved receipt is permanent even if configure, compilation or checking fails.
function New-LauncherPackageReservation([string] $StateDirectory, [string] $Version, [string] $Kind, [string] $Commit) {
    if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') { throw 'Package source version must contain three numeric components.' }
    if ($Kind -notin @('Prerelease', 'Beta', 'Special', 'Release')) { throw 'Invalid distribution kind.' }
    $receipts = Join-Path ([IO.Path]::GetFullPath($StateDirectory)) $Version
    $existing = Get-Item -LiteralPath $receipts -Force -ErrorAction SilentlyContinue
    if ($existing -and ($existing.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Package receipts cannot cross a reparse point: $receipts" }
    [void][IO.Directory]::CreateDirectory($receipts)
    $revision = 0
    $numbered = $Kind -in @('Prerelease', 'Beta')
    if ($numbered) {
        $counterPath = Join-Path $receipts 'counter.json'
        if (Test-Path -LiteralPath $counterPath) {
            $counter = Get-Content -LiteralPath $counterPath -Raw | ConvertFrom-Json
            if ("$($counter.LastRevision)" -notmatch '^(0|[1-9][0-9]*)$') { throw "Invalid durable package counter: $counterPath" }
            $revision = [int]$counter.LastRevision
        }
        foreach ($file in Get-ChildItem -LiteralPath $receipts -Filter 'prerelease-*.json' -File) {
            if ($file.Name -notmatch '^prerelease-([1-9][0-9]*)\.json$') { throw "Unrecognized package reservation: $($file.Name)" }
            $number = [int]$Matches[1]
            if ($number -gt $revision) { $revision = $number }
        }
        $revision++
        if ($revision -gt 65535) { throw 'The fourth version component is exhausted; advance the source version.' }
        # Retain the shared receipt names so older worktrees also see Beta reservations.
        $receiptPath = Join-Path $receipts "prerelease-$revision.json"
    } else {
        $receiptPath = Join-Path $receipts ($Kind.ToLowerInvariant() + '-' + [Guid]::NewGuid().ToString('N') + '.json')
    }
    $receipt = [ordered]@{
        Schema = 1; Version = "$Version.$revision"; SourceVersion = $Version; Revision = $revision
        Kind = $Kind; Commit = $Commit; ReservedUtc = [DateTime]::UtcNow.ToString('o'); Status = 'Reserved'
        BuildDirectory = ''; Archive = ''; Sha256 = ''; CompletedUtc = ''; Failure = ''
    }
    Write-LauncherPackageReceipt $receiptPath $receipt -Create
    if ($numbered) {
        # Either the receipt or this independent high-water mark survives an
        # interrupted write. Never lower the counter if a receipt is lost.
        Write-LauncherPackageReceipt $counterPath ([ordered]@{ Schema = 1; LastRevision = $revision }) -Create:(!(Test-Path -LiteralPath $counterPath))
    }
    return [pscustomobject]@{ Revision = $revision; Path = $receiptPath; Receipt = $receipt }
}
