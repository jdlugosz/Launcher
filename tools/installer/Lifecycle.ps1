#requires -Version 5.1
# Process and scoped shortcut operations; file installation belongs to the worker.

. (Join-Path $PSScriptRoot 'Processes.ps1')

function Get-LauncherInstallationTargets {
    param($Discovery, [string]$PackageDirectory, [string]$DestinationDirectory, [string]$UserSid, [int]$SessionId)
    if ((Get-LauncherDiscoveryProperty $Discovery 'ProcessEnumerationSucceeded') -eq $false) {
        throw 'Running Launcher processes could not be inspected. Refresh discovery before installing.'
    }
    $directories = @((ConvertTo-LauncherDiscoveryPath $PackageDirectory), (ConvertTo-LauncherDiscoveryPath $DestinationDirectory))
    foreach ($process in @($Discovery.Processes)) {
        if (!$process.Path) { throw ('Cannot locate running process {0}; close it or refresh discovery.' -f $process.ProcessId) }
        $directory = ConvertTo-LauncherDiscoveryPath ([IO.Path]::GetDirectoryName($process.Path))
        if ($directory -notin $directories) { continue }
        $leaf = [IO.Path]::GetFileName($process.Path)
        if ($leaf -inotmatch '^Launcher(?:-R\d+\.\d+(?:\.\d+){0,2})?\.exe$' -and $leaf -notin @('LauncherDrop.exe', 'LauncherSvgExtractor.exe')) { continue }
        if ($process.Identity.State -ne 'Verified' -or $process.Component -notin @('Launcher', 'LauncherDrop', 'LauncherSvgExtractor')) {
            throw ('Cannot verify running Launcher process {0} at {1}; close it before installing.' -f $process.ProcessId, $process.Path)
        }
        if ($process.OwnerSid -ne $UserSid -or $process.SessionId -ne $SessionId -or !$process.CreationTimeUtc) {
            throw ('Launcher process {0} belongs to another user/session or cannot be fully inspected. Its user must close it before installing.' -f $process.ProcessId)
        }
        if ($process.Component -eq 'LauncherDrop') {
            throw 'LauncherDrop is running from the source or selected installation. Preserve any uncopied text and close LauncherDrop before installing.'
        }
        if ($process.Component -eq 'LauncherSvgExtractor') {
            throw 'SVG Extractor is running from the source or selected installation. Let its scan finish or stop it, then close SVG Extractor before installing.'
        }
        $process
    }
}

function Open-LauncherInstallationProcesses {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]$Discovery,
        [Parameter(Mandatory = $true)][string]$PackageDirectory,
        [Parameter(Mandatory = $true)][string]$DestinationDirectory,
        [Parameter(Mandatory = $true)][string]$UserSid,
        [Parameter(Mandatory = $true)][int]$SessionId,
        [scriptblock]$ProcessPin = { param($Id) Initialize-LauncherLifecycleNative; [LauncherSetup.RunningProcess]::Pin($Id) },
        [scriptblock]$MetadataReader = { param($Path) [Diagnostics.FileVersionInfo]::GetVersionInfo($Path) }
    )
    $targets = @(Get-LauncherInstallationTargets $Discovery $PackageDirectory $DestinationDirectory $UserSid $SessionId)
    $pinned = [Collections.Generic.List[object]]::new()
    try {
        # Validate every target before signaling any window. Handles keep PID reuse
        # from changing which process is signaled or waited for.
        foreach ($target in $targets) {
            $process = & $ProcessPin $target.ProcessId
            if ($null -eq $process) { continue }
            $pinned.Add($process)
            $created = ([datetime]$target.CreationTimeUtc).ToUniversalTime()
            $identity = Get-LauncherExecutableIdentity $process.Path $MetadataReader
            if ($process.ProcessId -ne $target.ProcessId -or $process.Path -ine $target.Path -or
                $process.OwnerSid -ne $UserSid -or $process.SessionId -ne $SessionId -or
                [Math]::Abs(($process.CreationTimeUtc.ToUniversalTime() - $created).Ticks) -gt 10 -or
                $identity.State -ne 'Verified' -or $identity.Component -ne 'Launcher') {
                throw ('Running process {0} changed or cannot be verified. Refresh discovery before installing.' -f $target.ProcessId)
            }
        }
    } catch {
        foreach ($process in $pinned) { $process.Dispose() }
        throw
    }
    return ,$pinned
}

function Read-LauncherRestartArguments {
    param($Process, [int]$TimeoutMs)
    if (-not ('Dlugosz.Win32.WindowTextQuery' -as [type])) {
        Add-Type -Path (Join-Path $PSScriptRoot 'WindowTextQuery.cs') -ReferencedAssemblies System.Windows.Forms
    }
    if (!$Process.IsRunning) { throw 'The process exited before its restart arguments could be read.' }
    $arguments = [Dlugosz.Win32.WindowTextQuery]::Read($Process.MainWindow, 'Dlugosz.RestartArguments.v1', $TimeoutMs, $Process.ProcessId)
    if (!$Process.IsRunning) { throw 'The process exited while its restart arguments were being read.' }
    return $arguments
}

function Get-LauncherInstallationRestartPlan {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]$Discovery, [Parameter(Mandatory)][string]$PackageDirectory,
        [Parameter(Mandatory)][string]$DestinationDirectory, [Parameter(Mandatory)][string]$UserSid,
        [Parameter(Mandatory)][int]$SessionId, [ValidateRange(1, 30000)][int]$TimeoutMs = 5000,
        [scriptblock]$ProcessPin = { param($Id) Initialize-LauncherLifecycleNative; [LauncherSetup.RunningProcess]::Pin($Id) },
        [scriptblock]$MetadataReader = { param($Path) [Diagnostics.FileVersionInfo]::GetVersionInfo($Path) },
        [scriptblock]$ArgumentsReader = { param($Process, $Timeout) Read-LauncherRestartArguments $Process $Timeout }
    )
    $pinned = Open-LauncherInstallationProcesses $Discovery $PackageDirectory $DestinationDirectory $UserSid $SessionId $ProcessPin $MetadataReader
    $instances = [Collections.Generic.List[object]]::new()
    try {
        # Query every instance before any close. The response is already a complete
        # argument string; setup neither parses it nor reconstructs application options.
        foreach ($process in $pinned) {
            try { $arguments = & $ArgumentsReader $process $TimeoutMs }
            catch { throw ('Cannot save restart arguments for Launcher process {0}: {1} Close that instance manually and retry.' -f $process.ProcessId, $_.Exception.Message) }
            if ($null -eq $arguments -or $arguments -isnot [string] -or $arguments.Length -ge 32767 -or $arguments.Contains([string][char]0)) {
                throw ('Launcher process {0} returned invalid restart arguments. No instances have been closed.' -f $process.ProcessId)
            }
            $instances.Add([pscustomobject]@{
                ProcessId = $process.ProcessId; Path = $process.Path; OwnerSid = $process.OwnerSid
                SessionId = $process.SessionId; CreationTimeUtc = $process.CreationTimeUtc.ToUniversalTime().ToString('o')
                Arguments = $arguments; ExecutableHash = (Get-FileHash -LiteralPath $process.Path -Algorithm SHA256).Hash
            })
        }
    } finally { foreach ($process in $pinned) { $process.Dispose() } }
    return $instances.ToArray()
}

function Stop-LauncherInstallationProcesses {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]$Discovery, [Parameter(Mandatory)][string]$PackageDirectory,
        [Parameter(Mandatory)][string]$DestinationDirectory, [Parameter(Mandatory)][string]$UserSid,
        [Parameter(Mandatory)][int]$SessionId, [ValidateRange(1, 30000)][int]$TimeoutMs = 5000,
        [scriptblock]$ProcessPin = { param($Id) Initialize-LauncherLifecycleNative; [LauncherSetup.RunningProcess]::Pin($Id) },
        [scriptblock]$MetadataReader = { param($Path) [Diagnostics.FileVersionInfo]::GetVersionInfo($Path) },
        [object[]]$RestartInstances,
        [Collections.Generic.List[int]]$StoppedProcessIds = [Collections.Generic.List[int]]::new()
    )
    $pinned = Open-LauncherInstallationProcesses $Discovery $PackageDirectory $DestinationDirectory $UserSid $SessionId $ProcessPin $MetadataReader
    try {
        if ($PSBoundParameters.ContainsKey('RestartInstances')) {
            foreach ($process in $pinned) {
                $saved = @($RestartInstances | Where-Object { $_.ProcessId -eq $process.ProcessId })
                if ($saved.Count -ne 1 -or $saved[0].Path -ine $process.Path -or
                    $saved[0].OwnerSid -ne $process.OwnerSid -or $saved[0].SessionId -ne $process.SessionId -or
                    [Math]::Abs((([datetime]$saved[0].CreationTimeUtc).ToUniversalTime() - $process.CreationTimeUtc.ToUniversalTime()).Ticks) -gt 10) {
                    throw ('Launcher process {0} started or changed after restart arguments were collected. Refresh discovery before installing.' -f $process.ProcessId)
                }
            }
        }
        foreach ($process in $pinned) { $process.Close($TimeoutMs); $StoppedProcessIds.Add($process.ProcessId) }
    } finally { foreach ($process in $pinned) { $process.Dispose() } }
    return [pscustomobject]@{ StoppedProcessIds = $StoppedProcessIds.ToArray() }
}

function Start-LauncherInstallationInstance {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Executable, [AllowEmptyString()][string]$Arguments = '')
    $options = @{
        FilePath = $Executable; WorkingDirectory = [IO.Path]::GetDirectoryName($Executable)
        WindowStyle = 'Normal'; PassThru = $true
    }
    if ($Arguments.Length) { $options.ArgumentList = $Arguments }
    $process = Start-Process @options
    $process.Dispose()
}

function Restore-LauncherInstallationInstances {
    [CmdletBinding()]
    param(
        [object[]]$Instances, [int[]]$StoppedProcessIds,
        [scriptblock]$ProcessStart = { param($Path, $Arguments) Start-LauncherInstallationInstance -Executable $Path -Arguments $Arguments }
    )
    foreach ($instance in $Instances) {
        if ($instance.ProcessId -notin $StoppedProcessIds) { continue }
        try {
            # A failed deployment may have rolled back, or failed during rollback.
            # Resume only the original verified bytes, never an uncertain replacement.
            if ((Get-FileHash -LiteralPath $instance.Path -Algorithm SHA256 -ErrorAction Stop).Hash -cne $instance.ExecutableHash) {
                throw 'The original executable has not been restored.'
            }
            $null = & $ProcessStart $instance.Path $instance.Arguments
        } catch {
            'Cannot restart process {0} from {1}: {2}' -f $instance.ProcessId, $instance.Path, $_.Exception.Message
        }
    }
}

function Assert-LauncherInstallationStopped {
    [CmdletBinding()]
    param($Discovery, [string]$PackageDirectory, [string]$DestinationDirectory, [string]$UserSid, [int]$SessionId)
    $remaining = @(Get-LauncherInstallationTargets $Discovery $PackageDirectory $DestinationDirectory $UserSid $SessionId)
    if ($remaining.Count) { throw 'Launcher started again while installation was prepared. Close it and retry the installation.' }
}

function Read-LauncherShortcut {
    param([string]$Path)
    $shell = $null; $shortcut = $null
    try {
        $shell = New-Object -ComObject WScript.Shell
        $shortcut = $shell.CreateShortcut($Path)
        return [pscustomobject]@{
            TargetPath = [string]$shortcut.TargetPath; WorkingDirectory = [string]$shortcut.WorkingDirectory
            Arguments = [string]$shortcut.Arguments; Description = [string]$shortcut.Description
            IconLocation = [string]$shortcut.IconLocation
        }
    } finally {
        if ($null -ne $shortcut) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut) }
        if ($null -ne $shell) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) }
    }
}

function Get-LauncherRecoveryShortcutPlan {
    param($Registration, [string]$ProgramsFolder, [string]$ProgramRoot)
    $machine = $Registration.Scope -ceq 'AllUsers'
    if (!$ProgramsFolder) {
        $ProgramsFolder = [Environment]::GetFolderPath($(if ($machine) { 'CommonPrograms' } else { 'Programs' }))
    }
    if (!$ProgramRoot) {
        $ProgramRoot = if (!$machine) { Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Programs' }
            elseif ($env:ProgramW6432) { $env:ProgramW6432 }
            else { [Environment]::GetFolderPath('ProgramFiles') }
    }
    if (![IO.Path]::IsPathRooted($ProgramsFolder) -or ![IO.Path]::IsPathRooted($ProgramRoot)) {
        throw 'Windows could not locate the scoped Programs folders.'
    }
    $publisher = 'D' + [char]0x0142 + 'ugosz'
    $parent = Join-Path $ProgramsFolder $publisher
    $target = Join-Path $ProgramRoot ($publisher + '\Updater\Updater.exe')
    # Use the same scope as Launcher's update handoff. The standard path also
    # remains usable if the shared client is installed after Launcher.
    [pscustomobject]@{
        Path = Join-Path $parent 'Launcher Update History and Recovery.lnk'
        ProgramsFolder = $ProgramsFolder; Parent = $parent; TargetPath = $target
        WorkingDirectory = [IO.Path]::GetDirectoryName($target); InstallDirectory = $Registration.Directory
        Arguments = 'launcher --recovery --scope ' + $(if ($machine) { 'machine' } else { 'user' })
        Description = 'Review the last ' + $publisher + ' Launcher update'
        MatchIconLocation = ''
    }
}

function Get-LauncherStartMenuLaunchPlan($Recovery) {
    $launch = $Recovery.PSObject.Copy()
    $launch.Path = Join-Path $launch.Parent 'Launcher.lnk'
    $launch.TargetPath = Join-Path $launch.InstallDirectory 'Launcher.exe'
    $launch.WorkingDirectory = $launch.InstallDirectory
    $launch.Arguments = ''
    $launch.Description = 'Start Launcher'
    $launch.MatchIconLocation = $launch.TargetPath + ',0'
    return $launch
}

function Get-LauncherLegacyRecoveryShortcutPlan($Recovery) {
    $legacy = $Recovery.PSObject.Copy()
    $legacy.Parent += ' Launcher'
    $legacy.Path = Join-Path $legacy.Parent ([IO.Path]::GetFileName($Recovery.Path))
    return $legacy
}

function Test-LauncherRecoveryIcon([string]$Path) {
    if (!('LauncherSetup.RecoveryIcon' -as [type])) {
        Add-Type @'
using System;
using System.Runtime.InteropServices;
namespace LauncherSetup {
    public static class RecoveryIcon {
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, ExactSpelling=true)]
        static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
        [DllImport("kernel32.dll", ExactSpelling=true)]
        static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);
        [DllImport("kernel32.dll")]
        static extern bool FreeLibrary(IntPtr module);
        public static bool Exists(string path) {
            IntPtr module = LoadLibraryExW(path, IntPtr.Zero, 0x22); // data and image resources only
            if (module == IntPtr.Zero) return false;
            try { return FindResourceW(module, new IntPtr(20000), new IntPtr(14)) != IntPtr.Zero; }
            finally { FreeLibrary(module); }
        }
    }
}
'@
    }
    return [LauncherSetup.RecoveryIcon]::Exists($Path)
}

function Get-LauncherRecoveryIconLocation($Deployment) {
    $active = $Deployment.VersionedExecutable
    $install = Assert-LauncherSetupDirectory ([IO.Path]::GetDirectoryName($active))
    $entry = Get-LauncherSetupEntry $active
    if (!$entry -or $entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        (Get-FileHash -LiteralPath $active -Algorithm SHA256).Hash -cne $Deployment.Sha256) {
        throw 'The active Launcher executable changed before the recovery shortcut was created.'
    }
    $receipt = [pscustomobject](Get-LauncherDiscoveryProperty $Deployment 'LastUpdate')
    $prior = [pscustomobject](Get-LauncherDiscoveryProperty $receipt 'Previous')
    $current = [pscustomobject](Get-LauncherDiscoveryProperty $receipt 'Current')
    $priorVersion = Get-LauncherDiscoveryProperty $prior 'Version'
    $priorHash = Get-LauncherDiscoveryProperty $prior 'Sha256'
    # A Release can retire the previous executable after Keep. Keep the
    # shortcut artwork attached to a file that survives that cleanup.
    if ((Get-LauncherDiscoveryProperty $Deployment 'DistributionKind') -cne 'Release' -and
        $priorVersion -cmatch '^\d{1,5}\.\d{1,5}(?:\.\d{1,5}){0,2}$' -and $priorHash -cmatch '^[0-9A-F]{64}$' -and
        (Get-LauncherDiscoveryProperty $current 'Sha256') -ceq $Deployment.Sha256) {
        $paths = @($Deployment.PreviousExecutable, (Join-Path $install "Launcher-R$priorVersion.exe"))
        $paths += @(Get-ChildItem -LiteralPath $install -Filter 'Launcher*.backup.exe' -File -Force | ForEach-Object FullName)
        $candidates = foreach ($path in ($paths | Select-Object -Unique)) {
            if (!$path -or $path -ieq $active -or [IO.Path]::GetDirectoryName($path) -ine $install) { continue }
            $file = Get-LauncherSetupEntry $path
            if (!$file -or $file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) { continue }
            try {
                if ((Get-FileHash -LiteralPath $path -Algorithm SHA256 -ErrorAction Stop).Hash -cne $priorHash) { continue }
                $metadata = [Diagnostics.FileVersionInfo]::GetVersionInfo($path)
                if ((Get-LauncherVersionLabel $metadata) -cne $priorVersion -or !(Test-LauncherRecoveryIcon $path)) { continue }
                $version = Get-LauncherExactVersion $metadata -RequireFixed
            } catch { continue }
            [pscustomobject]@{
                Path = $path; Sha256 = $priorHash
                Version = $version
            }
        }
        $previous = Get-LauncherReleaseReviewFallback $receipt @($candidates) $Deployment.PreviousExecutable
        if ($previous) { return "$previous,-20000" }
    }
    if (Test-LauncherRecoveryIcon $active) { return "$active,-20000" }
    return "$active,0"
}

function Initialize-LauncherShellLink {
    if ('LauncherSetup.ShellLink' -as [type]) { return }

    Add-Type -ErrorAction Stop @'
using System;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Text;

namespace LauncherSetup {
    public static class ShellLink {
        private static readonly Guid ClassId = new Guid("00021401-0000-0000-C000-000000000046");
        private const int BufferLength = 32768;
        private const uint RawPath = 4; // SLGP_RAWPATH

        public sealed class Fields {
            internal Fields(string target, string working, string arguments, string description, string icon) {
                TargetPath = target;
                WorkingDirectory = working;
                Arguments = arguments;
                Description = description;
                IconLocation = icon;
            }

            public string TargetPath { get; private set; }
            public string WorkingDirectory { get; private set; }
            public string Arguments { get; private set; }
            public string Description { get; private set; }
            public string IconLocation { get; private set; }
        }

        public static Fields Read(string path) {
            IShellLinkW link = Create();
            try {
                ((IPersistFile)link).Load(path, 0); // STGM_READ
                var target = new StringBuilder(BufferLength);
                var working = new StringBuilder(BufferLength);
                var arguments = new StringBuilder(BufferLength);
                var description = new StringBuilder(BufferLength);
                var icon = new StringBuilder(BufferLength);
                int index;

                // Read the stored target without asking Explorer to resolve it.
                link.GetPath(target, target.Capacity, IntPtr.Zero, RawPath);
                link.GetWorkingDirectory(working, working.Capacity);
                link.GetArguments(arguments, arguments.Capacity);
                link.GetDescription(description, description.Capacity);
                link.GetIconLocation(icon, icon.Capacity, out index);
                return new Fields(target.ToString(), working.ToString(), arguments.ToString(), description.ToString(),
                    icon.ToString() + "," + index.ToString(CultureInfo.InvariantCulture));
            } finally {
                Marshal.FinalReleaseComObject(link);
            }
        }

        public static void Write(string path, string target, string working, string arguments, string description, string icon) {
            IShellLinkW link = Create();
            try {
                SetTarget(link, target);
                link.SetWorkingDirectory(working);
                link.SetArguments(arguments);
                link.SetDescription(description);
                if (!String.IsNullOrEmpty(icon)) {
                    // The numeric suffix follows the last comma. Earlier commas belong to the path.
                    int separator = icon.LastIndexOf(',');
                    int index;
                    if (separator >= 0 && Int32.TryParse(icon.Substring(separator + 1), NumberStyles.Integer,
                            CultureInfo.InvariantCulture, out index)) {
                        link.SetIconLocation(icon.Substring(0, separator), index);
                    } else {
                        link.SetIconLocation(icon, 0);
                    }
                }
                ((IPersistFile)link).Save(path, true);
            } finally {
                Marshal.FinalReleaseComObject(link);
            }
        }

        private static void SetTarget(IShellLinkW link, string path) {
            // SetPath can expand a literal %NAME% in an existing filename.
            if (path.IndexOf('%') >= 0) {
                IntPtr item = ILCreateFromPathW(path);
                if (item != IntPtr.Zero) {
                    try { link.SetIDList(item); }
                    finally { ILFree(item); }
                    return;
                }
            }
            // A recovery link can name the standard Updater path before it exists.
            link.SetPath(path);
        }

        [DllImport("shell32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern IntPtr ILCreateFromPathW(string path);

        [DllImport("shell32.dll", ExactSpelling = true)]
        private static extern void ILFree(IntPtr item);

        private static IShellLinkW Create() {
            return (IShellLinkW)Activator.CreateInstance(Type.GetTypeFromCLSID(ClassId, true));
        }

        [ComImport, Guid("000214F9-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IShellLinkW {
            // Keep native slot order, including methods this helper does not call.
            void GetPath([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder path, int characters, IntPtr findData, uint flags);
            void GetIDList(out IntPtr idList);
            void SetIDList(IntPtr idList);
            void GetDescription([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder description, int characters);
            void SetDescription([MarshalAs(UnmanagedType.LPWStr)] string description);
            void GetWorkingDirectory([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder directory, int characters);
            void SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] string directory);
            void GetArguments([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder arguments, int characters);
            void SetArguments([MarshalAs(UnmanagedType.LPWStr)] string arguments);
            void GetHotkey(out ushort hotkey);
            void SetHotkey(ushort hotkey);
            void GetShowCmd(out int show);
            void SetShowCmd(int show);
            void GetIconLocation([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder path, int characters, out int index);
            void SetIconLocation([MarshalAs(UnmanagedType.LPWStr)] string path, int index);
            void SetRelativePath([MarshalAs(UnmanagedType.LPWStr)] string path, uint reserved);
            void Resolve(IntPtr window, uint flags);
            void SetPath([MarshalAs(UnmanagedType.LPWStr)] string path);
        }
    }
}
'@
}

function Read-LauncherStartMenuShortcut([string]$Path) {
    Initialize-LauncherShellLink
    [LauncherSetup.ShellLink]::Read($Path)
}

function Write-LauncherStartMenuShortcut([string]$Path, $Plan, [string]$IconLocation) {
    Initialize-LauncherShellLink
    [LauncherSetup.ShellLink]::Write($Path, $Plan.TargetPath, $Plan.WorkingDirectory, $Plan.Arguments, $Plan.Description, $IconLocation)
}

function Read-LauncherOwnedStartMenuShortcut($Plan, [string]$ExpectedIcon) {
    $entry = Get-LauncherSetupEntry $Plan.Path
    if (!$entry) { return }
    if ($entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw ('The shortcut path is not a regular file: ' + $Plan.Path)
    }
    $hash = (Get-FileHash -LiteralPath $Plan.Path -Algorithm SHA256).Hash
    $link = Read-LauncherStartMenuShortcut $Plan.Path
    if ($link.TargetPath -ine $Plan.TargetPath -or $link.Arguments -cne $Plan.Arguments -or
        $link.WorkingDirectory -ine $Plan.WorkingDirectory -or $link.Description -cne $Plan.Description -or
        $link.IconLocation -notmatch '^(.+),(?:-20000|0)$' -or [IO.Path]::GetDirectoryName($Matches[1]) -ine $Plan.InstallDirectory -or
        ($Plan.MatchIconLocation -and $link.IconLocation -ine $Plan.MatchIconLocation) -or
        ($ExpectedIcon -and $link.IconLocation -ine $ExpectedIcon) -or
        (Get-FileHash -LiteralPath $Plan.Path -Algorithm SHA256).Hash -cne $hash) {
        throw ('The shortcut belongs to another installation or was changed. It was left in place: ' + $Plan.Path)
    }
    return $hash
}

function Set-LauncherStartMenuShortcut($Plan, [string]$IconLocation) {
    [void](Assert-LauncherSetupDirectory $Plan.Parent)
    [void](Assert-LauncherSetupDirectory ([IO.Path]::GetDirectoryName($Plan.TargetPath)))
    $target = Get-LauncherSetupEntry $Plan.TargetPath
    if ($target -and ($target.PSIsContainer -or ($target.Attributes -band [IO.FileAttributes]::ReparsePoint))) {
        throw ('The shortcut target is not a regular file: ' + $Plan.TargetPath)
    }
    if (![IO.Directory]::Exists($Plan.ProgramsFolder)) { throw 'The scoped Start menu Programs folder is unavailable.' }
    $before = Read-LauncherOwnedStartMenuShortcut $Plan
    [void][IO.Directory]::CreateDirectory($Plan.Parent)
    [void](Assert-LauncherSetupDirectory $Plan.Parent)
    $stage = Join-Path $Plan.Parent ('.launcher-' + [Guid]::NewGuid().ToString('N') + '.lnk')
    $backup = "$stage.backup"
    try {
        Write-LauncherStartMenuShortcut $stage $Plan $IconLocation
        $staged = $Plan.PSObject.Copy()
        $staged.Path = $stage
        $newHash = Read-LauncherOwnedStartMenuShortcut $staged $IconLocation
        if (!$newHash) { throw ('The staged shortcut failed verification: ' + $Plan.Path) }
        [void](Assert-LauncherSetupDirectory $Plan.Parent)
        if ((Read-LauncherOwnedStartMenuShortcut $Plan) -cne $before) { throw ('The shortcut changed during setup: ' + $Plan.Path) }
        if (!$before) { [IO.File]::Move($stage, $Plan.Path) }
        else {
            [IO.File]::Replace($stage, $Plan.Path, $backup)
            if ((Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash -cne $before) {
                if ((Get-FileHash -LiteralPath $Plan.Path -Algorithm SHA256).Hash -ceq $newHash) {
                    [IO.File]::Replace($backup, $Plan.Path, [NullString]::Value)
                }
                throw ('The shortcut changed during replacement. Its previous bytes were preserved: ' + $Plan.Path)
            }
            [IO.File]::Delete($backup)
        }
    } finally {
        if ([IO.File]::Exists($stage)) { [IO.File]::Delete($stage) }
    }
}

function Remove-LauncherStartMenuShortcut($Plan) {
    [void](Assert-LauncherSetupDirectory $Plan.Parent)
    $before = Read-LauncherOwnedStartMenuShortcut $Plan
    if (!$before) { return }
    $backup = Join-Path $Plan.Parent ('.launcher-' + [Guid]::NewGuid().ToString('N') + '.backup')
    [IO.File]::Move($Plan.Path, $backup)
    if ((Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash -cne $before) {
        if (!(Get-LauncherSetupEntry $Plan.Path)) { [IO.File]::Move($backup, $Plan.Path) }
        throw ('The shortcut changed during removal. Its bytes were preserved: ' + $Plan.Path)
    }
    [IO.File]::Delete($backup)
}

function Remove-LauncherLegacyRecoveryShortcut($Recovery) {
    $legacy = Get-LauncherLegacyRecoveryShortcutPlan $Recovery
    Remove-LauncherStartMenuShortcut $legacy
    [void](Assert-LauncherSetupDirectory $legacy.Parent)
    if ([IO.Directory]::Exists($legacy.Parent) -and ![IO.Directory]::GetFileSystemEntries($legacy.Parent).Length) {
        [IO.Directory]::Delete($legacy.Parent, $false)
    }
}

function Set-LauncherStartMenuShortcuts($Recovery, [string]$RecoveryIcon) {
    $launch = Get-LauncherStartMenuLaunchPlan $Recovery
    $warnings = @(
        try { Set-LauncherStartMenuShortcut $launch $launch.MatchIconLocation } catch { $_.Exception.Message }
        try {
            Set-LauncherStartMenuShortcut $Recovery $RecoveryIcon
            Remove-LauncherLegacyRecoveryShortcut $Recovery
        } catch { $_.Exception.Message }
    )
    if ($warnings.Count) { throw ($warnings -join ' ') }
}

function Remove-LauncherStartMenuShortcuts($Recovery) {
    $warnings = @(
        foreach ($plan in (Get-LauncherStartMenuLaunchPlan $Recovery), $Recovery) {
            try { Remove-LauncherStartMenuShortcut $plan } catch { $_.Exception.Message }
        }
        try { Remove-LauncherLegacyRecoveryShortcut $Recovery } catch { $_.Exception.Message }
    )
    if ($warnings.Count) { throw ($warnings -join ' ') }
}

function Assert-LauncherReviewedShortcut {
    param($Startup)
    $entry = Get-Item -LiteralPath $Startup.Path -Force -ErrorAction SilentlyContinue
    if ($Startup.State -eq 'Missing') {
        if ($null -ne $entry) { throw 'The Startup shortcut appeared after review. Refresh discovery before changing it.' }
        return
    }
    if ($null -eq $entry -or $entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        !$Startup.Sha256 -or (Get-FileHash -LiteralPath $Startup.Path -Algorithm SHA256).Hash -ne $Startup.Sha256) {
        throw 'The Startup shortcut changed after review. Refresh discovery before changing it.'
    }
    $current = Read-LauncherShortcut $Startup.Path
    if ((ConvertTo-LauncherDiscoveryPath $current.TargetPath) -ine $Startup.TargetPath -or
        $current.Arguments -cne $Startup.Arguments -or $current.WorkingDirectory -cne $Startup.WorkingDirectory) {
        throw 'The Startup shortcut no longer matches the reviewed target and arguments.'
    }
}

function Set-LauncherStartupOption {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]$Discovery,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][bool]$StartOnSignIn
    )
    if ((Get-LauncherDiscoveryProperty $Discovery 'UserSid') -ne [Security.Principal.WindowsIdentity]::GetCurrent().User.Value) {
        throw 'Startup must be applied by the original user who reviewed the installation.'
    }
    $plan = Get-LauncherStartupPlan $Discovery $Destination -StartOnSignIn $StartOnSignIn
    $startup = $Discovery.Startup
    $result = [pscustomobject]@{ Action = $plan.Action; Path = $startup.Path; TargetPath = [IO.Path]::Combine($Destination, 'Launcher.exe') }
    if ($plan.Action -in @('None', 'Keep')) { return $result }
    if ($plan.Action -eq 'Review') { throw $plan.Explanation }
    Assert-LauncherReviewedShortcut $startup
    $parent = [IO.Path]::GetDirectoryName($startup.Path)
    $directory = Get-Item -LiteralPath $parent -Force -ErrorAction Stop
    if (!$directory.PSIsContainer -or ($directory.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'The current-user Startup folder must be a real directory.'
    }
    $unique = [Guid]::NewGuid().ToString('N')
    $stage = Join-Path $parent ('.Launcher-' + $unique + '.lnk')
    $backup = Join-Path $parent ('.Launcher-' + $unique + '.backup')
    try {
        if ($plan.Action -eq 'Remove') {
            [IO.File]::Move($startup.Path, $backup)
            if ((Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash -ne $startup.Sha256) {
                if (!(Test-Path -LiteralPath $startup.Path)) { [IO.File]::Move($backup, $startup.Path) }
                throw ('The Startup shortcut changed during removal; its file is preserved at ' + $backup + '.')
            }
        } else {
            if (!(Test-Path -LiteralPath $result.TargetPath -PathType Leaf)) { throw 'The installed Launcher.exe is missing; Startup was not changed.' }
            $shell = $null; $shortcut = $null
            try {
                $shell = New-Object -ComObject WScript.Shell
                $shortcut = $shell.CreateShortcut($stage)
                $shortcut.TargetPath = $result.TargetPath
                $shortcut.WorkingDirectory = $Destination
                $shortcut.Arguments = if ($plan.Action -eq 'Retarget') { $startup.Arguments } else { '' }
                $shortcut.Description = 'Launcher'
                $shortcut.Save()
            } finally {
                if ($null -ne $shortcut) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut) }
                if ($null -ne $shell) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) }
            }
            $written = Read-LauncherShortcut $stage
            if ($written.TargetPath -ine $result.TargetPath -or $written.WorkingDirectory -ine $Destination -or
                $written.Arguments -cne $(if ($plan.Action -eq 'Retarget') { $startup.Arguments } else { '' })) {
                throw 'The staged Startup shortcut failed verification.'
            }
            Assert-LauncherReviewedShortcut $startup
            if ($plan.Action -eq 'Create') { [IO.File]::Move($stage, $startup.Path) }
            else {
                $newHash = (Get-FileHash -LiteralPath $stage -Algorithm SHA256).Hash
                [IO.File]::Replace($stage, $startup.Path, $backup)
                if ((Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash -ne $startup.Sha256) {
                    if ((Get-FileHash -LiteralPath $startup.Path -Algorithm SHA256).Hash -eq $newHash) {
                        [IO.File]::Replace($backup, $startup.Path, $null)
                    }
                    throw 'The Startup shortcut changed during replacement; its prior file was preserved.'
                }
            }
        }
        if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Force }
        return $result
    } finally { if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Force } }
}

function Backup-LauncherFirstDevelopmentConfiguration {
    [CmdletBinding()]
    param(
        [string]$PackageDirectory, [string]$DestinationDirectory, [string]$UserDataDirectory,
        [scriptblock]$MetadataReader = { param($Path) [Diagnostics.FileVersionInfo]::GetVersionInfo($Path) }
    )
    $active = Join-Path $DestinationDirectory 'Launcher.exe'
    $configuration = Join-Path $UserDataDirectory 'Launcher.toml'
    if (!(Test-Path -LiteralPath $active -PathType Leaf) -or !(Test-Path -LiteralPath $configuration -PathType Leaf)) { return $null }
    $incomingPath = Get-LauncherPackageExecutable -PackageDirectory $PackageDirectory -MetadataReader $MetadataReader
    $incoming = & $MetadataReader $incomingPath
    $previous = & $MetadataReader $active
    if (!$incoming.IsPreRelease -or $previous.IsPreRelease -or $previous.IsSpecialBuild -or $previous.IsPrivateBuild) { return $null }
    $incomingIdentity = Get-LauncherExecutableIdentity $incomingPath $MetadataReader
    $previousIdentity = Get-LauncherExecutableIdentity $active $MetadataReader
    if ($incomingIdentity.State -ne 'Verified' -or $incomingIdentity.Component -ne 'Launcher' -or
        $previousIdentity.State -notin @('Verified', 'Legacy') -or $previousIdentity.Component -ne 'Launcher') {
        throw 'The Launcher release identity changed; configuration was not snapshotted. Refresh discovery before installing.'
    }
    $label = Get-LauncherVersionLabel $previous
    $snapshot = Join-Path $UserDataDirectory ('Launcher-R' + $label + '.toml')
    $existing = Get-Item -LiteralPath $snapshot -Force -ErrorAction SilentlyContinue
    if ($null -ne $existing) {
        if ($existing.PSIsContainer -or ($existing.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'The existing release configuration snapshot must be a regular file.' }
        return $snapshot
    }
    $copies = [Collections.Generic.List[object]]::new()
    $copies.Add([pscustomobject]@{ Source = $configuration; Destination = $snapshot; Stage = $null; Hash = $null; Published = $false })
    $runtime = Join-Path $UserDataDirectory 'Launcher.runtime.toml'
    if (Get-Item -LiteralPath $runtime -Force -ErrorAction SilentlyContinue) {
        $runtimeSnapshot = Join-Path $UserDataDirectory ('Launcher-R' + $label + '.runtime.toml')
        $runtimeExisting = Get-Item -LiteralPath $runtimeSnapshot -Force -ErrorAction SilentlyContinue
        if ($runtimeExisting -and ($runtimeExisting.PSIsContainer -or ($runtimeExisting.Attributes -band [IO.FileAttributes]::ReparsePoint))) {
            throw 'The existing release runtime snapshot must be a regular file.'
        }
        if (!$runtimeExisting) {
            $copies.Add([pscustomobject]@{ Source = $runtime; Destination = $runtimeSnapshot; Stage = $null; Hash = $null; Published = $false })
        }
    }
    try {
        # Prepare and verify the complete available pair before publishing
        # either file. The main snapshot also records that this transition has
        # already happened when there was no runtime file to preserve.
        foreach ($copy in $copies) {
            $entry = Get-Item -LiteralPath $copy.Source -Force -ErrorAction Stop
            if ($entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw 'The personal configuration and runtime settings must be regular files before making release snapshots.'
            }
            $copy.Stage = Join-Path $UserDataDirectory ('.Launcher-config-' + [Guid]::NewGuid().ToString('N') + '.tmp')
            [IO.File]::Copy($copy.Source, $copy.Stage, $false)
            $copy.Hash = (Get-FileHash -LiteralPath $copy.Stage -Algorithm SHA256).Hash
            if ($copy.Hash -ne (Get-FileHash -LiteralPath $copy.Source -Algorithm SHA256).Hash) {
                throw 'The personal settings changed while making their release snapshots.'
            }
        }
        foreach ($copy in $copies) {
            [IO.File]::Move($copy.Stage, $copy.Destination)
            $copy.Published = $true
        }
        return $snapshot
    } catch {
        foreach ($copy in $copies) {
            if ($copy.Published -and (Get-FileHash -LiteralPath $copy.Destination -Algorithm SHA256).Hash -eq $copy.Hash) {
                [IO.File]::Delete($copy.Destination)
            }
        }
        throw
    } finally {
        foreach ($copy in $copies) {
            if ($copy.Stage -and (Test-Path -LiteralPath $copy.Stage)) { [IO.File]::Delete($copy.Stage) }
        }
    }
}
