#requires -Version 5.1
# Profile inspection and transfer belong to the initiating user, never the
# elevated program-file worker. Only the documented, bounded scope is imported.

function Initialize-LauncherPortableProfileApi {
    if ('LauncherSetup.PortableProfileFiles' -as [type]) { return }
    Add-Type @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using Microsoft.Win32.SafeHandles;
namespace LauncherSetup {
    public static class PortableProfileFiles {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern uint GetFileAttributesW(string path);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)] static extern bool CreateDirectoryW(string path, IntPtr security);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)] static extern bool GetFileInformationByHandle(SafeFileHandle file, out FileInformation information);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)] static extern bool SetFileInformationByHandle(SafeFileHandle file, int informationClass, ref byte information, uint size);
        [StructLayout(LayoutKind.Sequential)]
        struct FileInformation {
            public uint Attributes;
            public System.Runtime.InteropServices.ComTypes.FILETIME CreationTime, AccessTime, WriteTime;
            public uint Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
        }
        static Exception Error(string operation, string path) {
            int error = Marshal.GetLastWin32Error();
            return new Win32Exception(error, operation + ": " + path + " (" + new Win32Exception(error).Message + ")");
        }
        public static int Attributes(string path) {
            uint attributes = GetFileAttributesW(path);
            if (attributes != UInt32.MaxValue) return checked((int)attributes);
            int error = Marshal.GetLastWin32Error();
            if (error == 2 || error == 3) return -1;
            throw new Win32Exception(error, "Cannot inspect portable profile path: " + path + " (" + new Win32Exception(error).Message + ")");
        }
        public static bool CreateDirectory(string path) {
            if (CreateDirectoryW(path, IntPtr.Zero)) return true;
            int error = Marshal.GetLastWin32Error();
            if (error == 183) return false;
            throw new Win32Exception(error, "Cannot create portable profile directory: " + path + " (" + new Win32Exception(error).Message + ")");
        }
        public static SafeFileHandle PinDirectory(string path) {
            // Sharing reads/writes permits ordinary directory use; denying delete
            // pins this entry against replacement by a junction during traversal.
            var handle = CreateFileW(path, 0, 3, IntPtr.Zero, 3, 0x02200000, IntPtr.Zero);
            if (handle.IsInvalid) { var error = Error("Cannot open portable profile directory", path); handle.Dispose(); throw error; }
            FileInformation info;
            if (!GetFileInformationByHandle(handle, out info)) { var error = Error("Cannot inspect portable profile directory", path); handle.Dispose(); throw error; }
            if ((info.Attributes & 0x410) != 0x10) { handle.Dispose(); throw new IOException("Portable profile paths must use real directories: " + path); }
            return handle;
        }
        public sealed class PinnedFile : IDisposable {
            readonly FileStream stream;
            readonly string path;
            bool delete;
            internal PinnedFile(SafeFileHandle handle, string path, bool created) {
                this.path = path; delete = created;
                stream = new FileStream(handle, created ? FileAccess.ReadWrite : FileAccess.Read);
            }
            public string Hash() {
                try {
                    stream.Position = 0;
                    using (var algorithm = SHA256.Create()) {
                        return BitConverter.ToString(algorithm.ComputeHash(stream)).Replace("-", "");
                    }
                } catch (Exception error) { throw new IOException("Cannot hash portable profile file '" + path + "': " + error.Message, error); }
            }
            public void CopyFrom(PinnedFile source) {
                try {
                    source.stream.Position = 0;
                    source.stream.CopyTo(stream);
                    stream.Flush(true);
                } catch (Exception error) {
                    throw new IOException("Cannot copy portable profile file '" + source.path + "' into '" + path + "': " + error.Message, error);
                }
            }
            public byte[] ReadBytes() {
                stream.Position = 0;
                using (var content = new MemoryStream()) { stream.CopyTo(content); return content.ToArray(); }
            }
            public void WriteBytes(byte[] content) {
                stream.Position = 0;
                stream.Write(content, 0, content.Length);
                stream.SetLength(content.Length);
                stream.Flush(true);
            }
            public void Commit() { delete = false; }
            public void Dispose() {
                try {
                    if (delete) {
                        byte remove = 1;
                        if (!SetFileInformationByHandle(stream.SafeFileHandle, 4, ref remove, 1))
                            throw Error("Cannot roll back portable profile file", path);
                    }
                } finally { stream.Dispose(); }
            }
        }
        public static PinnedFile OpenRead(string path) {
            // OPEN_REPARSE_POINT checks the actual entry, and sharing only reads
            // keeps its bytes and identity stable through hashing and transfer.
            var handle = CreateFileW(path, 0x80000000, 1, IntPtr.Zero, 3, 0x00200000, IntPtr.Zero);
            if (handle.IsInvalid) { var error = Error("Cannot read portable profile file", path); handle.Dispose(); throw error; }
            FileInformation info;
            if (!GetFileInformationByHandle(handle, out info)) { var error = Error("Cannot inspect portable profile file", path); handle.Dispose(); throw error; }
            if ((info.Attributes & 0x410) != 0) { handle.Dispose(); throw new IOException("Portable profile files must be regular files: " + path); }
            try { return new PinnedFile(handle, path, false); } catch { handle.Dispose(); throw; }
        }
        public static PinnedFile CreateFile(string path) {
            // CREATE_NEW never overwrites a racing file, link, or directory.
            // DELETE access allows rollback by the owned handle, avoiding a
            // check-then-delete race against a subsequently replaced filename.
            var handle = CreateFileW(path, 0xC0010000, 1, IntPtr.Zero, 1, 0x00200080, IntPtr.Zero);
            if (handle.IsInvalid) { var error = Error("Cannot create portable profile file", path); handle.Dispose(); throw error; }
            try { return new PinnedFile(handle, path, true); } catch { handle.Dispose(); throw; }
        }
    }
}
'@
}

function Get-LauncherPortableProfilePaths([string]$PackageDirectory, [string]$UserDataDirectory) {
    $package = [IO.Path]::GetFullPath($PackageDirectory).TrimEnd('\')
    $userData = [IO.Path]::GetFullPath($UserDataDirectory).TrimEnd('\')
    $sourceTree = $package + '\'
    $destinationTree = $userData + '\'
    if ($package -eq [IO.Path]::GetPathRoot($package).TrimEnd('\') -or
        $userData -eq [IO.Path]::GetPathRoot($userData).TrimEnd('\') -or
        $sourceTree.StartsWith($destinationTree, [StringComparison]::OrdinalIgnoreCase) -or
        $destinationTree.StartsWith($sourceTree, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Portable package and user-data paths must be separate, non-overlapping dedicated directories.'
    }
    return [pscustomobject]@{ PackageDirectory = $package; UserDataDirectory = $userData }
}

function Lock-LauncherPortableProfileDirectory {
    param([string]$Path, $DirectoryPins, $CreatedDirectories, [switch]$AllowMissing, [switch]$Create)
    if ($DirectoryPins.Contains($Path)) { return $true }
    $parent = [IO.Path]::GetDirectoryName($Path)
    if ($parent -and $parent -ne $Path) {
        $exists = Lock-LauncherPortableProfileDirectory $parent $DirectoryPins $CreatedDirectories -AllowMissing:$AllowMissing -Create:$Create
        if (!$exists -and !$Create) { return $false }
    }
    if ([LauncherSetup.PortableProfileFiles]::Attributes($Path) -eq -1) {
        if ($Create) {
            if ([LauncherSetup.PortableProfileFiles]::CreateDirectory($Path)) { $CreatedDirectories.Add($Path) }
        } elseif ($AllowMissing) { return $false }
        else { throw "Portable profile directory is missing: $Path" }
    }
    $DirectoryPins.Add($Path, [LauncherSetup.PortableProfileFiles]::PinDirectory($Path))
    return $true
}

function Get-LauncherPortableProfileInventory {
    param([string]$Directory, $DirectoryPins, $FilePins, [switch]$AllowMissing, $OwnedFiles = @{})
    if (!(Lock-LauncherPortableProfileDirectory $Directory $DirectoryPins $null -AllowMissing:$AllowMissing)) { return }
    $inventory = [Collections.Generic.List[object]]::new()
    $names = @{}
    $visit = {
        param([string]$Current, [bool]$Root)
        try { $children = [IO.Directory]::GetFileSystemEntries($Current) }
        catch { throw "Cannot list portable profile directory '$Current': $($_.Exception.Message)" }
        foreach ($path in $children) {
            $name = [IO.Path]::GetFileName($path)
            $iconDirectory = $Root -and $name -in @('Icon', 'Icons')
            $toml = $Root -and [IO.Path]::GetExtension($name) -ieq '.toml' -and
                $name -notin @('Launcher.default.toml', 'Launcher.installed.default.toml')
            if ($Root -and !$iconDirectory -and !$toml) { continue }
            $attributes = [LauncherSetup.PortableProfileFiles]::Attributes($path)
            if ($attributes -eq -1) { throw "Portable profile entry disappeared while inspecting: $path" }
            if ($attributes -band [int][IO.FileAttributes]::ReparsePoint) { throw "Portable profile paths must not contain reparse points: $path" }
            if ($attributes -band [int][IO.FileAttributes]::Directory) {
                if ($toml) { throw "Portable profile files must be regular files: $path" }
                [void](Lock-LauncherPortableProfileDirectory $path $DirectoryPins $null)
                & $visit $path $false
            } else {
                if ($iconDirectory) { throw "Portable profile paths must use real directories: $path" }
                $relative = $path.Substring($Directory.Length + 1)
                if ($names.ContainsKey($relative)) { throw "Duplicate portable profile filename: $path" }
                $names[$relative] = $true
                if ($OwnedFiles.ContainsKey($relative)) { $handle = $OwnedFiles[$relative] }
                else {
                    $handle = [LauncherSetup.PortableProfileFiles]::OpenRead($path)
                    $FilePins.Add($handle)
                }
                $inventory.Add([pscustomobject]@{ RelativePath = $relative; Path = $path; Sha256 = $handle.Hash(); Handle = $handle })
            }
        }
    }
    & $visit $Directory $true
    return @($inventory | Sort-Object RelativePath)
}

function Close-LauncherPortableProfilePins($FilePins, $DirectoryPins) {
    foreach ($pin in $FilePins) { $pin.Dispose() }
    $paths = @($DirectoryPins.Keys | Sort-Object Length -Descending)
    foreach ($path in $paths) { $DirectoryPins[$path].Dispose() }
}

function Get-LauncherPortableProfilePlan {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$PackageDirectory,
        [Parameter(Mandatory)][string]$UserDataDirectory,
        [Parameter(Mandatory)][ValidateSet('Install', 'Upgrade')][string]$Action
    )
    Initialize-LauncherPortableProfileApi
    $paths = Get-LauncherPortableProfilePaths $PackageDirectory $UserDataDirectory
    $directories = [Collections.Specialized.OrderedDictionary]::new([StringComparer]::OrdinalIgnoreCase)
    $pins = [Collections.Generic.List[object]]::new()
    try {
        $source = @(Get-LauncherPortableProfileInventory $paths.PackageDirectory $directories $pins)
        $destination = @(Get-LauncherPortableProfileInventory $paths.UserDataDirectory $directories $pins -AllowMissing)
        $existing = @{}
        foreach ($file in $destination) { $existing[$file.RelativePath] = $file.Sha256 }
        $files = @($source | ForEach-Object {
            [pscustomobject]@{
                RelativePath = $_.RelativePath; SourcePath = $_.Path
                DestinationPath = Join-Path $paths.UserDataDirectory $_.RelativePath
                Sha256 = $_.Sha256; DestinationHash = $existing[$_.RelativePath]
            }
        })
        $different = @($files | Where-Object { $_.Sha256 -ne $_.DestinationHash })
        $sourceNames = @{}
        foreach ($file in $files) { $sourceNames[$file.RelativePath] = $file.Sha256 }
        $conflicts = @($destination | Where-Object { !$sourceNames.ContainsKey($_.RelativePath) -or $_.Sha256 -ne $sourceNames[$_.RelativePath] })
        $copy = @()
        $warning = ''
        if (!$files.Count) {
            $summary = 'Portable profile: no files to transfer.'
        } elseif ($Action -eq 'Upgrade') {
            $summary = 'Upgrade preserves the installed profile. Portable profile files are not copied.'
            if ($different.Count) {
                $warning = "WARNING: Edited or added portable profile files are NOT copied during upgrades. These files differ from or are absent in AppData ($($paths.UserDataDirectory)):`r`n" +
                    (($different | ForEach-Object { '  ' + $_.RelativePath }) -join "`r`n")
            }
        } elseif ($conflicts.Count) {
            $summary = 'The existing AppData profile is preserved; no portable profile files will be copied.'
            $warning = "WARNING: Portable profile files are NOT copied because AppData already contains a different profile ($($paths.UserDataDirectory)). No files are merged or overwritten. Existing files that differ from or are absent in the portable profile:`r`n" +
                (($conflicts | ForEach-Object { '  ' + $_.RelativePath }) -join "`r`n")
        } else {
            $copy = @($files | Where-Object { !$_.DestinationHash })
            if ($copy.Count) {
                $summary = "Portable profile files to copy into $($paths.UserDataDirectory):`r`n" +
                    (($copy | ForEach-Object { '  ' + $_.RelativePath }) -join "`r`n")
            } else { $summary = "The portable profile already matches AppData ($($paths.UserDataDirectory)); no files need copying." }
        }
        return [pscustomobject]@{
            PackageDirectory = $paths.PackageDirectory; UserDataDirectory = $paths.UserDataDirectory; Action = $Action
            Files = $files; CopyFiles = $copy; Warning = $warning; Summary = $summary
            DestinationFiles = @($destination | ForEach-Object { [pscustomobject]@{ RelativePath = $_.RelativePath; Sha256 = $_.Sha256 } })
        }
    } finally { Close-LauncherPortableProfilePins $pins $directories }
}

function Assert-LauncherPortableProfileSnapshot($Expected, $Actual, [string]$Directory) {
    $hashes = @{}
    foreach ($file in $Expected) { $hashes[$file.RelativePath] = $file.Sha256 }
    foreach ($file in $Actual) {
        if (!$hashes.ContainsKey($file.RelativePath) -or $hashes[$file.RelativePath] -ne $file.Sha256) {
            throw "Portable profile changed since it was inspected: $($file.Path)"
        }
        $hashes.Remove($file.RelativePath)
    }
    if ($hashes.Count) { throw "Portable profile changed since it was inspected: $(Join-Path $Directory @($hashes.Keys)[0])" }
}

function Publish-LauncherPortableProfileFile($Stage, [string]$DestinationPath) {
    $created = [LauncherSetup.PortableProfileFiles]::CreateFile($DestinationPath)
    try { $created.CopyFrom($Stage); return $created }
    catch { $created.Dispose(); throw }
}

function Invoke-LauncherPortableProfileTransfer {
    [CmdletBinding()]
    param([Parameter(Mandatory)]$Plan, [scriptblock]$BeforePublish)
    if ($Plan.Action -eq 'Upgrade' -or $Plan.Warning -or !$Plan.CopyFiles.Count) { return $Plan.Summary }
    Initialize-LauncherPortableProfileApi
    $paths = Get-LauncherPortableProfilePaths $Plan.PackageDirectory $Plan.UserDataDirectory
    $directories = [Collections.Specialized.OrderedDictionary]::new([StringComparer]::OrdinalIgnoreCase)
    $pins = [Collections.Generic.List[object]]::new()
    $createdDirectories = [Collections.Generic.List[string]]::new()
    $staged = [Collections.Generic.List[object]]::new()
    $published = [Collections.Generic.List[object]]::new()
    $transaction = $null
    $completed = $false
    try {
        $source = @(Get-LauncherPortableProfileInventory $paths.PackageDirectory $directories $pins)
        $existing = @(Get-LauncherPortableProfileInventory $paths.UserDataDirectory $directories $pins -AllowMissing)
        Assert-LauncherPortableProfileSnapshot $Plan.Files $source $paths.PackageDirectory
        Assert-LauncherPortableProfileSnapshot $Plan.DestinationFiles $existing $paths.UserDataDirectory
        [void](Lock-LauncherPortableProfileDirectory $paths.UserDataDirectory $directories $createdDirectories -Create)
        $transaction = Join-Path $paths.UserDataDirectory ('.launcher-profile-' + [Guid]::NewGuid().ToString('N'))
        if (![LauncherSetup.PortableProfileFiles]::CreateDirectory($transaction)) { throw "Portable profile staging directory already exists: $transaction" }
        [void](Lock-LauncherPortableProfileDirectory $transaction $directories $createdDirectories)
        $sourceFiles = @{}
        foreach ($file in $source) { $sourceFiles[$file.RelativePath] = $file }
        foreach ($file in $Plan.CopyFiles) {
            $stagePath = Join-Path $transaction ($staged.Count.ToString() + '.tmp')
            $stage = [LauncherSetup.PortableProfileFiles]::CreateFile($stagePath)
            $staged.Add($stage)
            $stage.CopyFrom($sourceFiles[$file.RelativePath].Handle)
            if ($stage.Hash() -ne $file.Sha256) { throw "Portable profile file failed staging verification: $($file.SourcePath)" }
            [void](Lock-LauncherPortableProfileDirectory ([IO.Path]::GetDirectoryName($file.DestinationPath)) $directories $createdDirectories -Create)
        }
        if ($BeforePublish) { & $BeforePublish | Out-Null }
        # Detect added files as well as changed bytes. The source and existing
        # destination handles remain pinned throughout staging and publication.
        $checkPins = [Collections.Generic.List[object]]::new()
        try {
            Assert-LauncherPortableProfileSnapshot $Plan.Files @(Get-LauncherPortableProfileInventory $paths.PackageDirectory $directories $checkPins) $paths.PackageDirectory
            Assert-LauncherPortableProfileSnapshot $Plan.DestinationFiles @(Get-LauncherPortableProfileInventory $paths.UserDataDirectory $directories $checkPins) $paths.UserDataDirectory
        } finally { foreach ($pin in $checkPins) { $pin.Dispose() } }
        $ownedFiles = @{}
        for ($index = 0; $index -lt $Plan.CopyFiles.Count; ++$index) {
            $file = $Plan.CopyFiles[$index]
            $created = Publish-LauncherPortableProfileFile $staged[$index] $file.DestinationPath
            $published.Add($created)
            $ownedFiles[$file.RelativePath] = $created
            if ($created.Hash() -ne $file.Sha256) { throw "Portable profile file failed destination verification: $($file.DestinationPath)" }
        }
        $checkPins = [Collections.Generic.List[object]]::new()
        try {
            Assert-LauncherPortableProfileSnapshot $Plan.Files @(Get-LauncherPortableProfileInventory $paths.PackageDirectory $directories $checkPins) $paths.PackageDirectory
            $expected = @($Plan.DestinationFiles) + @($Plan.CopyFiles)
            Assert-LauncherPortableProfileSnapshot $expected @(Get-LauncherPortableProfileInventory $paths.UserDataDirectory $directories $checkPins -OwnedFiles $ownedFiles) $paths.UserDataDirectory
        } finally { foreach ($pin in $checkPins) { $pin.Dispose() } }
        foreach ($file in $published) { $file.Commit() }
        $completed = $true
    } finally {
        $cleanupErrors = [Collections.Generic.List[string]]::new()
        foreach ($pin in @($published) + @($staged)) {
            try { $pin.Dispose() } catch { $cleanupErrors.Add($_.Exception.Message) }
        }
        try { Close-LauncherPortableProfilePins $pins $directories }
        catch { $cleanupErrors.Add($_.Exception.Message) }
        if ($transaction) {
            try { [IO.Directory]::Delete($transaction) }
            catch { $cleanupErrors.Add("Portable profile staging directory retained: $transaction ($($_.Exception.Message))") }
        }
        if (!$completed) {
            for ($index = $createdDirectories.Count - 1; $index -ge 0; --$index) {
                # Delete only empty directories this transaction created. A
                # concurrent file is preserved along with its parent directory.
                try { [IO.Directory]::Delete($createdDirectories[$index]) } catch { }
            }
        }
        if ($cleanupErrors.Count) { throw ('Portable profile cleanup requires attention: ' + ($cleanupErrors -join '; ')) }
    }
    return "Copied $($Plan.CopyFiles.Count) portable profile file(s) into $($paths.UserDataDirectory), preserving their relative paths."
}

function Initialize-LauncherInstalledConfiguration {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$TemplatePath,
        [Parameter(Mandatory)][string]$UserDataDirectory,
        [string]$FileName = 'Launcher.toml',
        [scriptblock]$BeforeCreate,
        [scriptblock]$BeforeCommit
    )

    # Called only by the initiating user's coordinator, after portable transfer.
    # Program deployment and elevated workers never select or write this profile.
    Initialize-LauncherPortableProfileApi
    $template = [IO.Path]::GetFullPath($TemplatePath)
    $data = [IO.Path]::GetFullPath($UserDataDirectory).TrimEnd('\')
    if ($data -ieq [IO.Path]::GetPathRoot($data).TrimEnd('\')) { throw 'User data requires a dedicated directory.' }
    # Exact-file seeding may copy a legacy demo from a child of this same
    # profile. The non-overlapping-tree rule applies to bulk portable transfer.
    $paths = [pscustomobject]@{ UserDataDirectory = $data }
    if ([IO.Path]::GetFileName($FileName) -cne $FileName -or $FileName -in @('', '.', '..')) { throw 'A configuration filename must be a single filename.' }
    $destination = Join-Path $paths.UserDataDirectory $FileName
    $directories = [Collections.Specialized.OrderedDictionary]::new([StringComparer]::OrdinalIgnoreCase)
    $createdDirectories = [Collections.Generic.List[string]]::new()
    $source = $null
    $created = $null
    $completed = $false
    $existing = {
        $attributes = [LauncherSetup.PortableProfileFiles]::Attributes($destination)
        if ($attributes -eq -1) { return $false }
        if ($attributes -band 0x410) { throw "Installed configuration must be a regular file: $destination" }
        return $true
    }
    try {
        [void](Lock-LauncherPortableProfileDirectory $paths.UserDataDirectory $directories $createdDirectories -AllowMissing)
        if (& $existing) { return "Existing configuration preserved: $destination" }
        [void](Lock-LauncherPortableProfileDirectory ([IO.Path]::GetDirectoryName($template)) $directories $createdDirectories)
        $source = [LauncherSetup.PortableProfileFiles]::OpenRead($template)
        $hash = $source.Hash()
        [void](Lock-LauncherPortableProfileDirectory $paths.UserDataDirectory $directories $createdDirectories -Create)
        if ($BeforeCreate) { & $BeforeCreate | Out-Null }
        try { $created = [LauncherSetup.PortableProfileFiles]::CreateFile($destination) }
        catch {
            # A concurrent first run or installer can win CREATE_NEW. Preserve
            # its regular file, regardless of whether its contents match ours.
            $failure = $_
            if (& $existing) { return "Existing configuration preserved: $destination" }
            throw $failure
        }
        $created.CopyFrom($source)
        if ($created.Hash() -ne $hash) { throw "Installed configuration failed SHA-256 verification: $destination" }
        if ($BeforeCommit) { & $BeforeCommit | Out-Null }
        $created.Commit()
        $completed = $true
    } finally {
        # Uncommitted files are removed through their owned handles. A file
        # created by another process is never deleted during cleanup.
        try { if ($created) { $created.Dispose() } }
        finally {
            try { if ($source) { $source.Dispose() } }
            finally {
                Close-LauncherPortableProfilePins @() $directories
                if (!$completed) {
                    for ($index = $createdDirectories.Count - 1; $index -ge 0; --$index) {
                        try { [IO.Directory]::Delete($createdDirectories[$index]) } catch { }
                    }
                }
            }
        }
    }
    return "Created starter configuration: $destination"
}

function Initialize-LauncherInstalledDemo {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$TemplatePath,
        [Parameter(Mandatory)][string]$UserDataDirectory,
        [scriptblock]$BeforeCreate,
        [scriptblock]$BeforeCommit
    )

    # This runs in the initiating account at setup completion, alongside the
    # default configuration seed. Reuse its pinned CREATE_NEW publication so
    # upgrades never refresh an edited demo or borrow its runtime settings.
    Initialize-LauncherPortableProfileApi
    $directory = Join-Path $UserDataDirectory 'Demos'
    $legacy = Join-Path $directory 'ManyColors\Launcher.toml'
    $destination = Join-Path $directory 'demo_many_colors.toml'
    $migrating = [LauncherSetup.PortableProfileFiles]::Attributes($destination) -eq -1 -and
        [LauncherSetup.PortableProfileFiles]::Attributes($legacy) -ne -1
    $source = if ($migrating) { $legacy } else { $TemplatePath }
    if ($migrating) {
        $runtime = Join-Path ([IO.Path]::GetDirectoryName($legacy)) 'Launcher.runtime.toml'
        if ([LauncherSetup.PortableProfileFiles]::Attributes($runtime) -ne -1) {
            $null = Initialize-LauncherInstalledConfiguration -TemplatePath $runtime -UserDataDirectory $directory -FileName 'demo_many_colors.runtime.toml'
        }
    }
    $summary = Initialize-LauncherInstalledConfiguration -TemplatePath $source -UserDataDirectory $directory -FileName 'demo_many_colors.toml' `
        -BeforeCreate $BeforeCreate -BeforeCommit $BeforeCommit
    return "Color demo: $summary"
}

function Initialize-LauncherInstalledDemoIcon {
    param([Parameter(Mandatory)][string]$PackageDirectory, [Parameter(Mandatory)][string]$UserDataDirectory)
    Initialize-LauncherInstalledConfiguration -TemplatePath (Join-Path $PackageDirectory 'resources\many-colors.svg') `
        -UserDataDirectory (Join-Path $UserDataDirectory 'Icons') -FileName 'many-colors.svg'
}

function Get-LauncherInstalledStarterReplacements {
    $quote = [string][char]0x27
    $demo = '"$USERDATA\Demos\demo_many_colors.toml"'
    $gear = '{ file = ' + $quote + '%SystemRoot%\System32\imageres.dll' + $quote + ', index = 62 }'
    $html = '{ file = ' + $quote + '%SystemRoot%\System32\imageres.dll' + $quote + ', index = 14 }'
    $replacements = [ordered]@{}
    foreach ($guide in @(
        @('Launcher guide', 'information', 'Launcher'),
        @('Setup and maintenance guide', 'document', 'Installer'),
        @('Launcher Drop guide', 'document', 'LauncherDrop'),
        @('SVG Extractor guide', 'document', 'SvgExtractor'))) {
        $prefix = '{ label = "' + $guide[0] + '", icon = { stock = "' + $guide[1] + '" }, target = '
        $old = $prefix + '"notepad.exe", arguments = ' + $quote + '"$EXE\docs\' + $guide[2] + '.md"' + $quote + ' },'
        $new = '{ label = "' + $guide[0] + '", icon = ' + $html + ', target = ' + $quote + '$EXE\docs\' + $guide[2] + '.html' + $quote + ' },'
        $replacements[$old] = $new
        $replacements[$prefix + $quote + '$EXE\docs\' + $guide[2] + '.html' + $quote + ' },'] = $new
    }
    $prefix = '{ label = "Try the many-colors demo", icon = { stock = "image" }, target = '
    $launch = '{ label = "Try the many-colors demo", icon = ' + $quote + '$USERDATA\Icons\many-colors.svg' + $quote + ', target = ' + $quote + '$SELF' + $quote + ', arguments = ' + $quote + $demo + $quote + ' },'
    $replacements[$prefix + $quote + '$EXE\examples\demo_many_colors.cmd' + $quote + ' },'] = $launch
    $replacements[$prefix + $quote + '$SELF' + $quote + ', arguments = ' + $quote + '"$USERDATA\Demos\ManyColors\Launcher.toml"' + $quote + ' },'] = $launch
    $portableLaunch = '{ label = "Try the many-colors demo", icon = ' + $quote + '$EXE\resources\many-colors.svg' + $quote + ', target = ' + $quote + '$SELF' + $quote + ', arguments = ' + $quote + '"$EXE\examples\demo_many_colors.toml"' + $quote + ' },'
    $replacements[$portableLaunch] = $launch
    $prefix = '{ label = "Read the color demo configuration", icon = { stock = "document" }, target = "notepad.exe", arguments = '
    $read = '{ label = "Read the color demo configuration", icon = ' + $gear + ', target = "notepad.exe", arguments = ' + $quote + $demo + $quote + ' },'
    foreach ($arguments in @('"$EXE\examples\demo_many_colors.toml"', '"$USERDATA\Demos\ManyColors\Launcher.toml"')) {
        $replacements[$prefix + $quote + $arguments + $quote + ' },'] = $read
    }
    $replacements['{ label = "Read the color demo configuration", icon = ' + $gear + ', target = "notepad.exe", arguments = ' + $quote + '"$EXE\examples\demo_many_colors.toml"' + $quote + ' },'] = $read
    $oldBasic = '{ label = "Basic color definitions", icon = { stock = "document" }, target = "notepad.exe", arguments = ' + $quote + '"$EXE\examples\basic_colors.toml"' + $quote + ' },'
    $replacements[$oldBasic] = $oldBasic.Replace('{ stock = "document" }', $gear)
    return $replacements
}

function Get-LauncherStarterQuoteState {
    param([string]$Line, [string]$State)
    # This conservative scanner only decides whether an exact stock item line
    # is ordinary TOML, never text inside a string. It does not rewrite TOML.
    for ($index = 0; $index -lt $Line.Length; $index++) {
        $character = $Line[$index]
        if ($State -eq 'Invalid') { return $State }
        if ($State -eq 'BasicMultiline') {
            if ($character -eq [char]92) { $index++; continue }
            if ($index + 2 -lt $Line.Length -and $Line.Substring($index, 3) -ceq '"""') {
                $State = 'Normal'; $index += 2
            }
            continue
        }
        if ($State -eq 'LiteralMultiline') {
            if ($index + 2 -lt $Line.Length -and $Line.Substring($index, 3) -ceq ([string][char]39 * 3)) {
                $State = 'Normal'; $index += 2
            }
            continue
        }
        if ($character -eq '#') { break }
        if ($character -ne '"' -and $character -ne [char]39) { continue }
        $quote = [string]$character
        if ($index + 2 -lt $Line.Length -and $Line.Substring($index, 3) -ceq ($quote * 3)) {
            $State = if ($character -eq '"') { 'BasicMultiline' } else { 'LiteralMultiline' }
            $index += 2
            continue
        }
        $closed = $false
        for ($index++; $index -lt $Line.Length; $index++) {
            if ($quote -eq '"' -and $Line[$index] -eq [char]92) { $index++; continue }
            if ([string]$Line[$index] -ceq $quote) { $closed = $true; break }
        }
        if (!$closed) { return 'Invalid' }
    }
    return $State
}

function ConvertTo-LauncherInstalledStarterText {
    [CmdletBinding()]
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
    $replacements = Get-LauncherInstalledStarterReplacements
    $parts = [regex]::Split($Text, '(\r\n|\n|\r)')
    $output = [Text.StringBuilder]::new()
    $state = 'Normal'
    $stopped = $false
    for ($index = 0; $index -lt $parts.Count; $index += 2) {
        $line = $parts[$index]
        $stopLine = if ($index -eq 0) { $line.TrimStart([char]0xFEFF) } else { $line }
        if ($stopLine -cmatch '^%STOP(?:$|[ \t])') { $stopped = $true }
        if (!$stopped) {
            $nextState = Get-LauncherStarterQuoteState $line $state
            if ($state -eq 'Normal' -and $nextState -eq 'Normal') {
                $content = $line.TrimStart([char[]]@(' ', "`t"))
                foreach ($old in $replacements.Keys) {
                    if (!$content.StartsWith($old, [StringComparison]::Ordinal)) { continue }
                    $suffix = $content.Substring($old.Length)
                    if ($suffix -notmatch '^[ \t]*(?:#.*)?$') { continue }
                    $line = $line.Substring(0, $line.Length - $content.Length) + $replacements[$old] + $suffix
                    break
                }
            }
            $state = $nextState
        }
        [void]$output.Append($line)
        if ($index + 1 -lt $parts.Count) { [void]$output.Append($parts[$index + 1]) }
    }
    return $output.ToString()
}

function Update-LauncherInstalledStarterLinks {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$UserDataDirectory, [scriptblock]$BeforeCommit)
    Initialize-LauncherPortableProfileApi
    $directory = [IO.Path]::GetFullPath($UserDataDirectory).TrimEnd('\')
    if ($directory -ieq [IO.Path]::GetPathRoot($directory).TrimEnd('\')) { throw 'Starter-link updates require a dedicated profile directory.' }
    $directories = [Collections.Specialized.OrderedDictionary]::new([StringComparer]::OrdinalIgnoreCase)
    $destination = Join-Path $directory 'Launcher.toml'
    $stage = $null
    $stageBytes = $null
    $source = $null
    $backup = $null
    try {
        if (!(Lock-LauncherPortableProfileDirectory $directory $directories $null -AllowMissing)) { return "No existing configuration to update: $destination" }
        if ([LauncherSetup.PortableProfileFiles]::Attributes($destination) -eq -1) { return "No existing configuration to update: $destination" }
        $source = [LauncherSetup.PortableProfileFiles]::OpenRead($destination)
        $originalBytes = $source.ReadBytes()
        $originalHash = $source.Hash()
        $encoding = [Text.UTF8Encoding]::new($false, $true)
        $original = $encoding.GetString($originalBytes)
        $updated = ConvertTo-LauncherInstalledStarterText -Text $original
        if ([string]::Equals($original, $updated, [StringComparison]::Ordinal)) { return "Existing configuration has no stock links to update: $destination" }
        $stageBytes = $encoding.GetBytes($updated)
        $stage = Join-Path $directory ('.launcher-starter-links-' + [guid]::NewGuid().ToString('N') + '.tmp')
        $writer = [LauncherSetup.PortableProfileFiles]::CreateFile($stage)
        try { $writer.WriteBytes($stageBytes); $writer.Commit() } finally { $writer.Dispose() }
        $source.Dispose(); $source = $null
        if ($BeforeCommit) { & $BeforeCommit $destination | Out-Null }
        $source = [LauncherSetup.PortableProfileFiles]::OpenRead($destination)
        if ($source.Hash() -cne $originalHash) { throw "Configuration changed while stock links were prepared; no update was applied: $destination" }
        $source.Dispose(); $source = $null
        $backupDirectory = Join-Path $directory 'Backups'
        $backupDirectories = [Collections.Generic.List[string]]::new()
        [void](Lock-LauncherPortableProfileDirectory $backupDirectory $directories $backupDirectories -Create)
        $backup = Join-Path $backupDirectory ('Launcher.before-setup-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N') + '.toml')
        [IO.File]::Replace($stage, $destination, $backup)
        $stage = $null
        $saved = [LauncherSetup.PortableProfileFiles]::OpenRead($backup)
        try { $savedHash = $saved.Hash() } finally { $saved.Dispose() }
        if ($savedHash -cne $originalHash) {
            # The atomic backup also detects an edit made in the narrow interval
            # after the precheck. Restore it only if our published bytes remain.
            $current = [LauncherSetup.PortableProfileFiles]::OpenRead($destination)
            try { $currentBytes = $current.ReadBytes() } finally { $current.Dispose() }
            if ([Convert]::ToBase64String($currentBytes) -ceq [Convert]::ToBase64String($stageBytes)) {
                $recovery = Join-Path $directory ('Launcher.stock-links-recovery-' + [guid]::NewGuid().ToString('N') + '.toml')
                [IO.File]::Copy($backup, $recovery, $false)
                $discarded = Join-Path $directory ('Launcher.stock-links-generated-' + [guid]::NewGuid().ToString('N') + '.toml')
                [IO.File]::Replace($backup, $destination, $discarded)
                throw "Configuration changed during publication; its newer content was restored. Recovery copy: $recovery"
            }
            throw "Configuration changed during publication; all recovery files were retained. Review: $backup"
        }
        return "Updated stock guide and demo links in $destination. Original configuration backup: $backup"
    } finally {
        if ($source) { $source.Dispose() }
        if ($stage -and [IO.File]::Exists($stage)) {
            $entry = Get-Item -LiteralPath $stage -Force -ErrorAction Stop
            if (!($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -and
                [Convert]::ToBase64String([IO.File]::ReadAllBytes($stage)) -ceq [Convert]::ToBase64String($stageBytes)) { [IO.File]::Delete($stage) }
        }
        Close-LauncherPortableProfilePins @() $directories
    }
}
