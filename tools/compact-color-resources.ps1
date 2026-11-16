[CmdletBinding()]
param(
    [Parameter(Mandatory)][string[]] $InputFiles,
    [Parameter(Mandatory)][string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'
[void][IO.Directory]::CreateDirectory($OutputDirectory)
foreach ($inputFile in $InputFiles) {
    $destination = Join-Path $OutputDirectory ([IO.Path]::GetFileName($inputFile))
    if ([IO.Path]::GetFullPath($inputFile) -eq [IO.Path]::GetFullPath($destination)) {
        throw 'Resource compaction must write to an intermediate directory.'
    }
    $lines = [Collections.Generic.List[string]]::new()
    $quote = [char]0
    $multiline = $false
    $blankPending = $false
    foreach ($line in [IO.File]::ReadAllLines($inputFile)) {
        # Match config_text_builder's physical-line directive before TOML scanning.
        if ($line -cmatch '^%STOP(?:$|[ \t])') { break }
        $preserveWhitespace = $multiline
        $text = [Text.StringBuilder]::new()
        for ($index = 0; $index -lt $line.Length;) {
            $character = $line[$index]
            if ($quote -eq [char]0) {
                if ($character -eq '#') { break }
                if ($character -eq '"' -or $character -eq "'") {
                    $quote = $character
                    $run = 1
                    while ($index + $run -lt $line.Length -and $line[$index + $run] -eq $quote) { ++$run }
                    $multiline = $run -ge 3
                    $count = if ($multiline) { 3 } else { 1 }
                    [void]$text.Append($line.Substring($index, $count))
                    $index += $count
                    continue
                }
            } elseif ($quote -eq '"' -and $character -eq '\') {
                # Escaped quotes and hashes belong to the basic string.
                $count = [Math]::Min(2, $line.Length - $index)
                [void]$text.Append($line.Substring($index, $count))
                $index += $count
                continue
            } elseif ($character -eq $quote) {
                $run = 1
                while ($index + $run -lt $line.Length -and $line[$index + $run] -eq $quote) { ++$run }
                if (!$multiline -or $run -ge 3) {
                    $count = if ($multiline) { $run } else { 1 }
                    [void]$text.Append($line.Substring($index, $count))
                    $index += $count
                    $quote = [char]0
                    $multiline = $false
                    continue
                }
            }
            [void]$text.Append($character)
            ++$index
        }
        $retained = $text.ToString()
        if (!$preserveWhitespace -and !$multiline) {
            $retained = $retained.TrimEnd([char[]]" `t")
            if ([string]::IsNullOrWhiteSpace($retained)) {
                $blankPending = $lines.Count -gt 0
                continue
            }
        }
        if ($blankPending) { $lines.Add(''); $blankPending = $false }
        $lines.Add($retained)
    }
    $content = if ($lines.Count) { ($lines -join "`n") + "`n" } else { '' }
    if (!(Test-Path -LiteralPath $destination) -or [IO.File]::ReadAllText($destination) -cne $content) {
        [IO.File]::WriteAllText($destination, $content, [Text.UTF8Encoding]::new($false))
    }
}
