#requires -Version 5.1
# The caller holds the scoped installation lock throughout writing, replay,
# and retirement. No installed entry changes before the marker is published.

function Get-LauncherArchiveJournalFileHash([string]$Path) {
    [void](Assert-LauncherSetupDirectory ([IO.Path]::GetDirectoryName($Path)))
    $entry = Get-LauncherSetupEntry $Path
    if (!$entry) { return $null }
    if ($entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Archive revert recovery requires a regular file: $Path"
    }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash
}

function Get-LauncherArchiveJournalPlan([string]$Directory, $Record) {
    $install = Assert-LauncherSetupDirectory $Directory
    $transactionName = Get-LauncherDiscoveryProperty $Record 'TransactionDirectory'
    $transactionId = Get-LauncherDiscoveryProperty $Record 'TransactionId'
    $aliasKind = Get-LauncherDiscoveryProperty $Record 'AliasKind'
    $manifestHash = Get-LauncherDiscoveryProperty $Record 'ManifestHash'
    $oldVersion = Get-LauncherDiscoveryProperty $Record 'OldVersion'
    $newVersion = Get-LauncherDiscoveryProperty $Record 'NewVersion'
    $rows = @(Get-LauncherDiscoveryProperty $Record 'Entries')
    $digest = '\A[0-9A-F]{64}\z'
    if ((Get-LauncherDiscoveryProperty $Record 'SchemaVersion') -ne 1 -or
        $transactionName -isnot [string] -or $transactionId -isnot [string] -or
        $manifestHash -isnot [string] -or $aliasKind -isnot [string] -or
        $oldVersion -isnot [string] -or $newVersion -isnot [string] -or
        $transactionName -cnotmatch '\A\.launcher-setup-[0-9a-f]{32}\z' -or
        $transactionId -cnotmatch '\A[0-9a-f]{32}\z' -or $transactionId -ceq ('0' * 32) -or
        $manifestHash -cnotmatch $digest -or $aliasKind -cnotin @('HardLink', 'Copy') -or
        !(Test-LauncherQuickRollbackBuildIdentity ([pscustomobject]@{ Version = $oldVersion; Sha256 = $manifestHash })) -or
        !(Test-LauncherQuickRollbackBuildIdentity ([pscustomobject]@{ Version = $newVersion; Sha256 = $manifestHash })) -or
        $oldVersion -ceq $newVersion -or $rows.Count -lt 2 -or $rows.Count -gt 256) {
        throw 'The archive revert journal has an invalid transaction.'
    }
    $transaction = Assert-LauncherSetupDirectory (Join-Path $install $transactionName)
    if (!(Get-LauncherSetupEntry $transaction)) { throw 'The archive revert recovery directory is missing.' }
    $destinations = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $files = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $entries = [Collections.Generic.List[object]]::new()
    $hasActive = $false
    $hasManifest = $false
    foreach ($row in $rows) {
        $relative = Get-LauncherDiscoveryProperty $row 'RelativePath'
        $stage = Get-LauncherDiscoveryProperty $row 'Stage'
        $backup = Get-LauncherDiscoveryProperty $row 'Backup'
        $before = Get-LauncherDiscoveryProperty $row 'OriginalHash'
        $after = Get-LauncherDiscoveryProperty $row 'Sha256'
        $isManifest = $relative -ieq 'Launcher.install-manifest.json'
        $isActive = $relative -ieq 'Launcher.exe'
        if ($relative -isnot [string] -or (!$isManifest -and !(Test-LauncherManagedRelativePath $relative)) -or
            !$destinations.Add($relative) -or ($null -ne $stage -and $stage -isnot [string]) -or
            ($null -ne $backup -and $backup -isnot [string]) -or
            ($null -ne $before -and ($before -isnot [string] -or $before -cnotmatch $digest)) -or
            ($null -ne $after -and ($after -isnot [string] -or $after -cnotmatch $digest)) -or (!$before -and !$after)) {
            throw 'The archive revert journal has an invalid or repeated managed file.'
        }
        if ($isManifest) {
            if ($before -cne $manifestHash -or !$after -or $before -ceq $after -or
                $stage -cne 'manifest.json' -or $backup -cne 'manifest.backup') {
                throw 'The archive revert journal has an invalid ownership manifest entry.'
            }
            $hasManifest = $true
        } elseif (($after -and $stage -cnotmatch '\Astage-[0-9]{1,3}\z') -or (!$after -and $stage) -or
            ($before -and $after -and $backup -cnotmatch '\Abackup-[0-9]{1,3}\z') -or
            ($before -and !$after -and $backup -cnotmatch '\Aremoved-[0-9]{1,3}\z') -or (!$before -and $backup) -or
            ($isActive -and (!$before -or !$after -or $before -ceq $after))) {
            throw 'The archive revert journal has an invalid stage or backup.'
        }
        foreach ($name in @($stage, $backup)) {
            if ($name -and !$files.Add($name)) { throw 'The archive revert journal repeats a stage or backup.' }
        }
        if ($isActive) { $hasActive = $true }
        $replay = 'replay-' + $entries.Count
        [void]$files.Add($replay)
        $entries.Add([pscustomobject]@{
            Destination = Join-Path $install $relative
            Stage = $(if ($stage) { Join-Path $transaction $stage } else { $null })
            Backup = $(if ($backup) { Join-Path $transaction $backup } else { $null })
            Replay = Join-Path $transaction $replay
            OriginalHash = $before; Sha256 = $after; IsActive = $isActive
        })
    }
    if (!$hasActive -or !$hasManifest) { throw 'The archive revert journal omits the active executable or manifest.' }
    return [pscustomobject]@{
        Marker = Join-Path $install '.launcher-archive-restore.json'; Directory = $transaction
        OldVersion = $oldVersion; NewVersion = $newVersion; AliasKind = $aliasKind
        Entries = $entries.ToArray(); TemporaryFiles = $files
    }
}

function Read-LauncherArchiveRestoreJournal([string]$Directory) {
    $install = Assert-LauncherSetupDirectory $Directory
    $marker = Join-Path $install '.launcher-archive-restore.json'
    $entry = Get-LauncherSetupEntry $marker
    if (!$entry) { return $null }
    if ($entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'The archive revert journal must be a regular file.'
    }
    $stream = [IO.File]::Open($marker, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        if ($stream.Length -le 0 -or $stream.Length -gt 256KB) { throw 'The archive revert journal size is invalid.' }
        $reader = [IO.StreamReader]::new($stream, [Text.UTF8Encoding]::new($false, $true))
        try { $record = $reader.ReadToEnd() | ConvertFrom-Json -ErrorAction Stop }
        finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
    Get-LauncherArchiveJournalPlan $install $record
}

function Sync-LauncherArchiveJournalFile([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try { $stream.Flush($true) } finally { $stream.Dispose() }
}

function Write-LauncherArchiveRestoreJournal {
    param([Parameter(Mandatory)][string]$Directory, [Parameter(Mandatory)][string]$TransactionDirectory,
        [Parameter(Mandatory)]$Entries, [Parameter(Mandatory)]$Plan)
    $install = Assert-LauncherSetupDirectory $Directory
    $transaction = Assert-LauncherSetupDirectory $TransactionDirectory
    if ([IO.Path]::GetDirectoryName($transaction) -ine $install) { throw 'The archive revert stage must be inside the installation.' }
    $rows = foreach ($entry in $Entries) {
        if ($entry.Skip) { continue }
        if (!$entry.Destination.StartsWith($install + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'An archive revert destination is outside the installation.'
        }
        foreach ($path in @($entry.Stage, $entry.Backup)) {
            if ($path -and [IO.Path]::GetDirectoryName($path) -ine $transaction) {
                throw 'An archive revert backup or stage is outside the transaction.'
            }
        }
        [pscustomobject]@{
            RelativePath = $entry.Destination.Substring($install.Length + 1)
            Stage = $(if ($entry.Stage) { [IO.Path]::GetFileName($entry.Stage) } else { $null })
            Backup = $(if ($entry.Backup) { [IO.Path]::GetFileName($entry.Backup) } else { $null })
            OriginalHash = $(if ($entry.OriginalHash) { $entry.OriginalHash } else { $null })
            Sha256 = $(if ($entry.Sha256) { $entry.Sha256 } else { $null })
        }
    }
    $record = [pscustomobject]@{
        SchemaVersion = 1; TransactionDirectory = [IO.Path]::GetFileName($transaction)
        TransactionId = $Plan.TransactionId; ManifestHash = $Plan.ManifestHash; AliasKind = $Plan.AliasKind
        OldVersion = $Plan.Current.Version; NewVersion = $Plan.Previous.Version; Entries = @($rows)
    }
    $validated = Get-LauncherArchiveJournalPlan $install $record
    if (Get-LauncherSetupEntry $validated.Marker) { throw 'An earlier archive revert needs recovery.' }
    foreach ($entry in $validated.Entries) {
        if ((Get-LauncherArchiveJournalFileHash $entry.Destination) -cne $entry.OriginalHash -or
            ($entry.Stage -and (Get-LauncherArchiveJournalFileHash $entry.Stage) -cne $entry.Sha256) -or
            ($entry.Backup -and (Get-LauncherArchiveJournalFileHash $entry.Backup) -cne $(if ($entry.Sha256) { $entry.OriginalHash }))) {
            throw 'The archive revert files changed before journaling.'
        }
        if ($entry.Stage) { Sync-LauncherArchiveJournalFile $entry.Stage }
        if ($entry.Backup -and $entry.Sha256) { Sync-LauncherArchiveJournalFile $entry.Backup }
    }
    $temporary = Join-Path $transaction 'journal.pending'
    $data = [Text.UTF8Encoding]::new($false).GetBytes(($record | ConvertTo-Json -Depth 4 -Compress))
    $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.Write($data, 0, $data.Length); $stream.Flush($true) }
    finally { $stream.Dispose() }
    Move-LauncherSetupFile $temporary $validated.Marker $false
}

function Get-LauncherArchiveJournalReplay($Entry) {
    $current = Get-LauncherArchiveJournalFileHash $Entry.Destination
    if ($current -ceq $Entry.OriginalHash) { return 'None' }
    if ($current -cne $Entry.Sha256) { throw "An installed file changed during archive revert: $($Entry.Destination)" }
    if (!$Entry.OriginalHash) { return 'Remove' }
    if ((Get-LauncherArchiveJournalFileHash $Entry.Backup) -cne $Entry.OriginalHash) {
        throw "The archive revert backup could not be verified: $($Entry.Destination)"
    }
    return 'Restore'
}

function Repair-LauncherArchiveRestoreJournal {
    param([Parameter(Mandatory)][string]$Directory, [Parameter(Mandatory)][scriptblock]$RestoreRegistration)
    $journal = Read-LauncherArchiveRestoreJournal $Directory
    if (!$journal) { return }
    Initialize-LauncherSetupFileApi
    # Validate the entire plan before changing files. Recheck each operation
    # before applying it, since user files can change between these passes.
    & $RestoreRegistration $journal.OldVersion $journal.NewVersion $false | Out-Null
    foreach ($entry in $journal.Entries) { $null = Get-LauncherArchiveJournalReplay $entry }
    foreach ($entry in @($journal.Entries | Where-Object { !$_.IsActive }) + @($journal.Entries | Where-Object IsActive)) {
        $action = Get-LauncherArchiveJournalReplay $entry
        if ($action -eq 'None') { continue }
        if (Get-LauncherSetupEntry $entry.Replay) {
            $null = Get-LauncherArchiveJournalFileHash $entry.Replay
            [IO.File]::Delete($entry.Replay)
        }
        if ($action -eq 'Remove') {
            Move-LauncherSetupFile $entry.Destination $entry.Replay $false
            if ((Get-LauncherArchiveJournalFileHash $entry.Replay) -cne $entry.Sha256) {
                Move-LauncherSetupFile $entry.Replay $entry.Destination $false
                throw "A newly installed file changed during archive revert: $($entry.Destination)"
            }
            continue
        }
        if ($entry.IsActive -and $journal.AliasKind -eq 'HardLink') {
            if ((New-LauncherSetupAlias -Path $entry.Replay -Target $entry.Backup) -cne 'HardLink') {
                throw 'Archive revert recovery could not restore the original executable hard link.'
            }
        } else { [IO.File]::Copy($entry.Backup, $entry.Replay, $false) }
        if ((Get-LauncherArchiveJournalFileHash $entry.Replay) -cne $entry.OriginalHash -or
            (Get-LauncherArchiveJournalFileHash $entry.Destination) -cne $entry.Sha256) {
            throw "Archive revert recovery files changed: $($entry.Destination)"
        }
        Sync-LauncherArchiveJournalFile $entry.Replay
        Move-LauncherSetupFile $entry.Replay $entry.Destination ([bool]$entry.Sha256)
    }
    foreach ($entry in $journal.Entries) {
        if ((Get-LauncherArchiveJournalFileHash $entry.Destination) -cne $entry.OriginalHash) {
            throw "An installed file could not be restored after archive revert: $($entry.Destination)"
        }
    }
    & $RestoreRegistration $journal.OldVersion $journal.NewVersion $true | Out-Null
    Remove-LauncherArchiveRestoreJournal -Directory $Directory
}

function Remove-LauncherArchiveRestoreJournal {
    param([Parameter(Mandatory)][string]$Directory)
    $journal = Read-LauncherArchiveRestoreJournal $Directory
    if (!$journal) { return }
    Initialize-LauncherSetupFileApi
    $retired = Join-Path $journal.Directory 'journal.retired'
    # Retire the marker before removing anything replay would need. Cleanup
    # can fail or be interrupted without leaving an incomplete transaction.
    Move-LauncherSetupFile $journal.Marker $retired $false
    foreach ($name in @($journal.TemporaryFiles) + 'journal.retired') {
        $path = Join-Path $journal.Directory $name
        $entry = Get-LauncherSetupEntry $path
        if ($entry -and !$entry.PSIsContainer -and !($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            try { [IO.File]::Delete($path) } catch { Write-Warning "Archive revert staging file retained: $path" }
        }
    }
    try { [IO.Directory]::Delete($journal.Directory) }
    catch { Write-Warning "Archive revert staging directory retained: $($journal.Directory)" }
}
