#requires -Version 5.1
param([Parameter(Mandatory)][string]$Path)

$ErrorActionPreference = 'Stop'
$file = Get-Item -LiteralPath $Path
if ($file.PSIsContainer -or $file.Length -le 0 -or $file.Length -gt 16KB) {
    throw 'Use a DER public signing certificate up to 16 KiB.'
}
$bytes = [IO.File]::ReadAllBytes($file.FullName)
if ($bytes.Length -le 0 -or $bytes.Length -gt 16KB -or
    [Security.Cryptography.X509Certificates.X509Certificate2]::GetCertContentType($bytes) -ne 'Cert') {
    throw 'Use a DER file containing only a public signing certificate.'
}
$certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($bytes)
try {
    if ($certificate.HasPrivateKey -or [Convert]::ToBase64String($certificate.RawData) -cne [Convert]::ToBase64String($bytes)) {
        throw 'Use the public signing certificate in DER format.'
    }
    $key = [Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPublicKey($certificate)
    if (!$key) { throw 'The public signing certificate must have an RSA key.' }
    $key.Dispose()
    [BitConverter]::ToString($bytes).Replace('-', '')
} finally { $certificate.Dispose() }
