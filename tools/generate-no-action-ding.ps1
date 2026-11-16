# Original short bell tone, embedded in Launcher; no Windows sound-scheme dependency.
[CmdletBinding()]
param([string] $OutputPath = (Join-Path $PSScriptRoot '..\resources\no-action-ding.wav'))

$ErrorActionPreference = 'Stop'
$sampleRate = 22050
$samples = [int]($sampleRate * 0.4)
$stream = [IO.File]::Create([IO.Path]::GetFullPath($OutputPath))
$writer = [IO.BinaryWriter]::new($stream)
try {
    $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
    $writer.Write([int](36 + $samples * 2))
    $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
    $writer.Write([int]16)
    $writer.Write([int16]1) # PCM
    $writer.Write([int16]1) # Mono
    $writer.Write([int]$sampleRate)
    $writer.Write([int]($sampleRate * 2))
    $writer.Write([int16]2)
    $writer.Write([int16]16)
    $writer.Write([Text.Encoding]::ASCII.GetBytes('data'))
    $writer.Write([int]($samples * 2))
    for ($sample = 0; $sample -lt $samples; ++$sample) {
        $time = $sample / [double]$sampleRate
        $phase = 2 * [Math]::PI * 1046.5 * $time
        $tone = [Math]::Sin($phase) * [Math]::Exp(-$time / 0.10) +
            0.24 * [Math]::Sin(2.4 * $phase) * [Math]::Exp(-$time / 0.045)
        $fade = [Math]::Min(1.0, $time / 0.002) * [Math]::Min(1.0, (0.4 - $time) / 0.01)
        $writer.Write([int16][Math]::Round(32767 * 0.18 * $fade * $tone))
    }
} finally {
    $writer.Dispose()
}
