[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $UserSid,
    [string] $StableSourceDirectory
)

$ErrorActionPreference = 'Stop'
if (!$StableSourceDirectory) {
    $StableSourceDirectory = Join-Path $PSScriptRoot '..\build\release-1.0.0\stage'
}
$installDirectory = Join-Path $env:ProgramFiles ('D' + [char]0x0142 + 'ugosz\Launcher')
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this one-time folder setup as administrator. Routine development copies need no elevation afterward.'
}
$accountSid = [Security.Principal.SecurityIdentifier]::new($UserSid)
$stableSource = Join-Path ([IO.Path]::GetFullPath($StableSourceDirectory)) 'Launcher.exe'
# This is the archived, verified 1.0.0 release, never the mutable build output.
$expectedStableHash = '4ABDB79F7FDD291373628D6AF46D40B1E5AEA64965CD7C27DF84C6F8ADD3370B'
if ((Get-FileHash -LiteralPath $stableSource -Algorithm SHA256).Hash -ne $expectedStableHash) {
    throw 'The preserved release executable does not match the verified 1.0.0 copy.'
}
if (Test-Path -LiteralPath $installDirectory) {
    $existing = Get-Item -LiteralPath $installDirectory
    if (!$existing.PSIsContainer -or ($existing.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'The dedicated Launcher installation path must be a real directory.'
    }
} else {
    New-Item -ItemType Directory -Path $installDirectory | Out-Null
}

# Add only this account's inherited Modify right on this app directory.
# Retain all existing owner, administrator, system and inherited permissions.
$installAcl = Get-Acl -LiteralPath $installDirectory
$rule = [Security.AccessControl.FileSystemAccessRule]::new($accountSid,
    [Security.AccessControl.FileSystemRights]::Modify,
    [Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit',
    [Security.AccessControl.PropagationFlags]::None,
    [Security.AccessControl.AccessControlType]::Allow)
$installAcl.AddAccessRule($rule)
Set-Acl -LiteralPath $installDirectory -AclObject $installAcl

& (Join-Path $PSScriptRoot 'install-release.ps1') -SourceDirectory $StableSourceDirectory `
    -InstallDirectory $installDirectory -KeepActive -NoStart
Write-Output "Ready: $installDirectory; Modify access granted to $($accountSid.Value). Existing files were preserved."
