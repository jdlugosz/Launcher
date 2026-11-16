[CmdletBinding()]
param(
    [string] $BuildDirectory = (Join-Path $PSScriptRoot '..\build\windows-x64'),
    [string] $ResourceHackerPath = 'C:\Program Files (x86)\Resource Hacker\ResourceHacker.exe',
    [string] $OutputDirectory,
    [switch] $IncludeInstalledExamples
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$build = [IO.Path]::GetFullPath($BuildDirectory)
$probe = Join-Path $build 'Release\LauncherResourceProbe.exe'
$fixture = Join-Path $build 'Release\LauncherResourceFixture.dll'
foreach ($path in @($probe, $fixture, $ResourceHackerPath)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing test dependency: $path" }
}
if (!$OutputDirectory) { $OutputDirectory = Join-Path $build ('pe-resources-' + [Guid]::NewGuid().ToString('N')) }
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw "Use a new results directory: $output" }
New-Item -ItemType Directory -Path $output | Out-Null

$cases = [Collections.Generic.List[object]]::new()
$cases.Add(@{ Label = 'Fixture SVG'; File = $fixture; Type = 'SVG'; Name = 'ARTWORK'; Language = 0 })
$cases.Add(@{ Label = 'Fixture PNG numeric'; File = $fixture; Type = 301; Name = 7; Language = 0 })
$cases.Add(@{ Label = 'Fixture commented SVG'; File = $fixture; Type = 10; Name = 'COMMENTS'; Language = 0 })
$cases.Add(@{ Label = 'Fixture PNG under SVG'; File = $fixture; Type = 'SVG'; Name = 'PNG_IN_SVG'; Language = 0 })
$cases.Add(@{ Label = 'Fixture other type'; File = $fixture; Type = 'OTHER'; Name = 'ARTWORK'; Language = 0 })
$cases.Add(@{ Label = 'Fixture neutral'; File = $fixture; Type = 10; Name = 'LOCALIZED'; Language = 0 })
$cases.Add(@{ Label = 'Fixture English'; File = $fixture; Type = 10; Name = 'LOCALIZED'; Language = 1033 })
$cases.Add(@{ Label = 'Fixture Unicode'; File = $fixture; Type = '图片'; Name = '图标'; Language = 0 })
$skipped = @()
if ($IncludeInstalledExamples) {
    $installed = @(
        @{ Label = 'Windows Bluetooth device'; File = 'C:\Windows\SystemApps\MicrosoftWindows.Client.CBS_cw5n1h2txyewy\BatteryWidgetProvider.dll'; Type = 10; Name = 'BATTERY_EXTENDED_DEVICEBLUETOOTH_SVG'; Language = 1033 },
        @{ Label = 'Windows authentication'; File = 'C:\Windows\System32\en-US\webauthn.dll.mui'; Type = 23; Name = 96; Language = 1033 },
        @{ Label = 'Adobe magnifying glass'; File = 'C:\Program Files\Adobe\Adobe InDesign 2026\dvaui.dll'; Type = 'SVG'; Name = 'S_SEARCHMAGNIFY_WI_N_AT_2X'; Language = 0 },
        @{ Label = 'Zoom meeting'; File = 'I:\Users\John\AppData\Roaming\Zoom\bin\zWinRes.dll'; Type = 'ZPIMGRES'; Name = 'HOME_MEETING.SVG'; Language = 0 }
    )
    foreach ($case in $installed) {
        if (Test-Path -LiteralPath $case.File -PathType Leaf) { $cases.Add($case) }
        else { $skipped += $case; Write-Warning "Skipped unavailable source: $($case.File)" }
    }
}

function ConvertTo-TomlValue($Value) { ConvertTo-Json -InputObject $Value -Compress }
$configLines = @('schema_version = 1', '[grid]', 'columns = 4', 'icon_size_dip = 64',
    '[[tabs]]', 'name = "PE resource examples"', 'color = "#F3C982"', 'items = [')
foreach ($case in $cases) {
    $selector = if ($case.Name -is [string]) { 'name = ' } else { 'id = ' }
    $configLines += ('    {{ label = {0}, icon = {{ file = {1}, type = {2}, {3}{4}, language = {5} }} }},' -f
        (ConvertTo-TomlValue $case.Label), (ConvertTo-TomlValue $case.File), (ConvertTo-TomlValue $case.Type),
        $selector, (ConvertTo-TomlValue $case.Name), $case.Language)
}
$configLines += ']'
$config = Join-Path $output 'examples.toml'
[IO.File]::WriteAllLines($config, $configLines, [Text.UTF8Encoding]::new($false))
$payloads = Join-Path $output 'payloads'
$probeOutput = & $probe $config $payloads 2>&1
$probeExit = $LASTEXITCODE
$probeOutput | Set-Content -LiteralPath (Join-Path $output 'probe.txt') -Encoding utf8
$probeOutput | Write-Output
if ($probeExit -ne 0) { throw "Resource extraction/decoding probe failed: $probeExit" }

function Read-ResIdentifier([byte[]] $Bytes, [ref] $Cursor) {
    $start = $Cursor.Value
    $word = [BitConverter]::ToUInt16($Bytes, $Cursor.Value)
    $Cursor.Value += 2
    if ($word -eq 65535) {
        $id = [BitConverter]::ToUInt16($Bytes, $Cursor.Value)
        $Cursor.Value += 2
        return [int] $id
    }
    while ($word -ne 0) {
        $word = [BitConverter]::ToUInt16($Bytes, $Cursor.Value)
        $Cursor.Value += 2
    }
    return [Text.Encoding]::Unicode.GetString($Bytes, $start, $Cursor.Value - $start - 2)
}

function Read-ResPayload([byte[]] $Bytes, $Type, $Name, [int] $Language) {
    # Parse only the standard RES container written by the independent tool.
    # This is not PE lookup and shares no production resource-reading code.
    $offset = 0
    while ($offset + 8 -le $Bytes.Length) {
        $length = [BitConverter]::ToUInt32($Bytes, $offset)
        $header = [BitConverter]::ToUInt32($Bytes, $offset + 4)
        $cursor = $offset + 8
        $data = $offset + $header
        if ($header -lt 8 -or $data + $length -gt $Bytes.Length) { throw 'Malformed Resource Hacker RES container' }
        $entryType = Read-ResIdentifier $Bytes ([ref] $cursor)
        $entryName = Read-ResIdentifier $Bytes ([ref] $cursor)
        $cursor = ($cursor + 3) -band -4
        if ($cursor + 16 -gt $data) { throw 'Malformed RES entry header' }
        $entryLanguage = [BitConverter]::ToUInt16($Bytes, $cursor + 6)
        $typeMatches = (($Type -is [string]) -eq ($entryType -is [string])) -and ($Type -ceq $entryType)
        $nameMatches = (($Name -is [string]) -eq ($entryName -is [string])) -and ($Name -ceq $entryName)
        if ($length -gt 0 -and $typeMatches -and $nameMatches -and $Language -eq $entryLanguage) {
            $payload = [byte[]]::new($length)
            [Array]::Copy($Bytes, [long] $data, $payload, 0, [long] $length)
            return ,$payload
        }
        $offset = ($data + $length + 3) -band -4
    }
    throw "Resource Hacker RES export lacks $Type/$Name/$Language"
}

$results = @()
$moduleHashes = @{}
$moduleCopies = @{}
$referenceContainers = @{}
$moduleDirectory = Join-Path $output 'modules'
New-Item -ItemType Directory -Path $moduleDirectory | Out-Null
for ($index = 0; $index -lt $cases.Count; $index++) {
    $case = $cases[$index]
    if (!$moduleCopies.ContainsKey($case.File)) {
        # The installed Resource Hacker is 32-bit. Copies avoid WOW64's System32
        # redirection, and the hashes prove it reads exactly the same module.
        $moduleHashes[$case.File] = (Get-FileHash -LiteralPath $case.File -Algorithm SHA256).Hash
        $copy = Join-Path $moduleDirectory ("$($moduleCopies.Count)-" + [IO.Path]::GetFileName($case.File))
        Copy-Item -LiteralPath $case.File -Destination $copy
        if ((Get-FileHash -LiteralPath $copy -Algorithm SHA256).Hash -ne $moduleHashes[$case.File]) { throw "Module copy differs: $($case.File)" }
        $moduleCopies[$case.File] = $copy
        $container = Join-Path $output ("module-$($referenceContainers.Count).res")
        $log = $container + '.log'
        # Export all resources as RES, then select the exact tuple above. This
        # avoids Resource Hacker's CLI bugs with Unicode and dotted names.
        $arguments = @('-open', ('"' + $copy + '"'), '-save', ('"' + $container + '"'),
            '-action', 'extract', '-mask', ',,', '-log', ('"' + $log + '"'))
        $arguments | ConvertTo-Json | Set-Content -LiteralPath ($container + '.arguments.json') -Encoding utf8
        $process = Start-Process -FilePath $ResourceHackerPath -ArgumentList $arguments -WindowStyle Hidden -PassThru
        if (!$process.WaitForExit(15000)) {
            $process.Kill() # Only the process created for this extraction.
            throw "Resource Hacker timed out for $($case.File); see $log"
        }
        $process.Refresh()
        if ($process.ExitCode -ne 0 -or !(Test-Path -LiteralPath $container -PathType Leaf)) {
            throw "Resource Hacker export failed ($($process.ExitCode)): $($case.File); see $log"
        }
        $referenceContainers[$case.File] = [IO.File]::ReadAllBytes($container)
    }
    $reference = Join-Path $output "$index.reference.bin"
    $actual = Join-Path $payloads "$index.bin"
    $expectedBytes = Read-ResPayload $referenceContainers[$case.File] $case.Type $case.Name $case.Language
    [IO.File]::WriteAllBytes($reference, $expectedBytes)
    $actualBytes = [IO.File]::ReadAllBytes($actual)
    if ($expectedBytes.Length -ne $actualBytes.Length) { throw "Payload length differs: $($case.Label)" }
    for ($byte = 0; $byte -lt $expectedBytes.Length; $byte++) {
        if ($expectedBytes[$byte] -ne $actualBytes[$byte]) { throw "Payload differs at byte ${byte}: $($case.Label)" }
    }
    if ((Get-FileHash -LiteralPath $case.File -Algorithm SHA256).Hash -ne $moduleHashes[$case.File]) { throw "Source changed during comparison: $($case.File)" }
    $results += [pscustomobject]@{ Label = $case.Label; Status = 'PASS'; File = $case.File; Type = $case.Type;
        NameOrId = $case.Name; Language = $case.Language; Bytes = $actualBytes.Length;
        ModuleSha256 = $moduleHashes[$case.File]; PayloadSha256 = (Get-FileHash -LiteralPath $actual -Algorithm SHA256).Hash }
}
foreach ($case in $skipped) {
    $results += [pscustomobject]@{ Label = $case.Label; Status = 'SKIPPED'; File = $case.File; Type = $case.Type;
        NameOrId = $case.Name; Language = $case.Language; Bytes = 0; ModuleSha256 = ''; PayloadSha256 = '' }
}
$results | Export-Csv -LiteralPath (Join-Path $output 'comparison.csv') -NoTypeInformation -Encoding utf8
$results | Select-Object Label, Status, Bytes | Format-Table -AutoSize
Write-Output "PASS: $($cases.Count) exact Resource Hacker comparisons and resource decodes. Visual acceptance pending."
Write-Output "Sample configuration: $config"
Write-Output "Results: $output"
