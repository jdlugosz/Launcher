#requires -Version 5.1
# Native read-only process discovery and verified graceful-close handles.

function Initialize-LauncherLifecycleNative {
    if ('LauncherSetup.RunningProcess' -as [type]) { return }
    Add-Type @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
namespace LauncherSetup {
    public sealed class ProcessCandidate {
        public int ProcessId { get; internal set; }
        public string Name { get; internal set; }
        public string ExecutablePath { get; internal set; }
        public int? SessionId { get; internal set; }
        public DateTime? CreationDate { get; internal set; }
        public string OwnerSid { get; internal set; }
        public string Owner { get; internal set; }
        public string[] Issues { get; internal set; }
    }
    public sealed class RunningProcess : IDisposable {
        IntPtr handle;
        public int ProcessId { get; private set; }
        public string Path { get; private set; }
        public string OwnerSid { get; private set; }
        public int SessionId { get; private set; }
        public DateTime CreationTimeUtc { get; private set; }
        [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
        struct ProcessEntry {
            public uint size, usage, processId;
            public UIntPtr defaultHeapId;
            public uint moduleId, threads, parentProcessId;
            public int priority;
            public uint flags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst=260)] public string name;
        }
        delegate bool WindowVisitor(IntPtr window, IntPtr argument);
        [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint process);
        [DllImport("kernel32.dll", EntryPoint="Process32FirstW", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool Process32First(IntPtr snapshot, ref ProcessEntry entry);
        [DllImport("kernel32.dll", EntryPoint="Process32NextW", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool Process32Next(IntPtr snapshot, ref ProcessEntry entry);
        [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, int id);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool QueryFullProcessImageName(IntPtr process, uint flags, StringBuilder path, ref int length);
        [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetProcessTimes(IntPtr process, out long created, out long exited, out long kernel, out long user);
        [DllImport("kernel32.dll", SetLastError=true)] static extern bool ProcessIdToSessionId(int process, out int session);
        [DllImport("kernel32.dll", SetLastError=true)] static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
        [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
        [DllImport("user32.dll", SetLastError=true)] static extern bool EnumWindows(WindowVisitor visitor, IntPtr argument);
        [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out int process);
        [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr window, StringBuilder name, int count);
        [DllImport("user32.dll", SetLastError=true)] static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr word, IntPtr data, uint flags, uint milliseconds, out IntPtr result);
        static string NativeError(string action, int error) {
            return action + ": " + new Win32Exception(error).Message;
        }
        static ProcessCandidate Inspect(ProcessEntry entry) {
            ProcessCandidate candidate = new ProcessCandidate();
            candidate.ProcessId = unchecked((int)entry.processId); candidate.Name = entry.name;
            List<string> issues = new List<string>();
            IntPtr process = OpenProcess(0x101000, false, candidate.ProcessId);
            if (process == IntPtr.Zero) {
                int error = Marshal.GetLastWin32Error();
                if (error == 87) return null; // The snapshot candidate has already exited.
                candidate.Issues = new string[] { NativeError("Cannot open the process for inspection", error) };
                return candidate;
            }
            try {
                if (WaitForSingleObject(process, 0) == 0) return null;
                try {
                    StringBuilder path = new StringBuilder(32768); int length = path.Capacity;
                    if (QueryFullProcessImageName(process, 0, path, ref length)) candidate.ExecutablePath = path.ToString();
                    else issues.Add(NativeError("Cannot read the executable path", Marshal.GetLastWin32Error()));
                    long created, exited, kernel, user;
                    if (GetProcessTimes(process, out created, out exited, out kernel, out user)) candidate.CreationDate = DateTime.FromFileTimeUtc(created);
                    else issues.Add(NativeError("Cannot read the process creation time", Marshal.GetLastWin32Error()));
                    int session;
                    if (ProcessIdToSessionId(candidate.ProcessId, out session)) candidate.SessionId = session;
                    else issues.Add(NativeError("Cannot read the process session", Marshal.GetLastWin32Error()));
                    IntPtr token;
                    if (OpenProcessToken(process, 8, out token)) {
                        try {
                            using (WindowsIdentity identity = new WindowsIdentity(token)) {
                                candidate.OwnerSid = identity.User.Value;
                                // Account-name lookup can be unavailable even when the token SID is readable.
                                try { candidate.Owner = identity.Name; }
                                catch { candidate.Owner = candidate.OwnerSid; }
                            }
                        } catch (Exception error) { issues.Add("Cannot read the process owner: " + error.Message); }
                        finally { CloseHandle(token); }
                    } else issues.Add(NativeError("Cannot read the process owner token", Marshal.GetLastWin32Error()));
                } catch (Exception error) { issues.Add("Cannot inspect the process: " + error.Message); }
                uint state = WaitForSingleObject(process, 0);
                if (state == 0) return null;
                if (state == 0xffffffff) issues.Add(NativeError("Cannot check whether the process exited", Marshal.GetLastWin32Error()));
            } finally { CloseHandle(process); }
            candidate.Issues = issues.ToArray();
            return candidate;
        }
        public static ProcessCandidate[] Candidates() {
            IntPtr snapshot = CreateToolhelp32Snapshot(2, 0); // TH32CS_SNAPPROCESS; no process or module access needed.
            if (snapshot == new IntPtr(-1)) {
                int error = Marshal.GetLastWin32Error();
                throw new Win32Exception(error, NativeError("Cannot list running processes", error));
            }
            List<ProcessCandidate> candidates = new List<ProcessCandidate>();
            try {
                ProcessEntry entry = new ProcessEntry(); entry.size = (uint)Marshal.SizeOf(typeof(ProcessEntry));
                bool available = Process32First(snapshot, ref entry);
                while (available) {
                    if (entry.name.StartsWith("Launcher", StringComparison.OrdinalIgnoreCase) &&
                        entry.name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) &&
                        entry.name.IndexOf("backup", StringComparison.OrdinalIgnoreCase) < 0) {
                        ProcessCandidate candidate = Inspect(entry);
                        if (candidate != null) candidates.Add(candidate);
                    }
                    available = Process32Next(snapshot, ref entry);
                }
                int error = Marshal.GetLastWin32Error();
                if (error != 18) throw new Win32Exception(error, NativeError("Cannot finish listing running processes", error));
            } finally { CloseHandle(snapshot); }
            return candidates.ToArray();
        }
        public static RunningProcess Pin(int id) {
            IntPtr process = OpenProcess(0x101000, false, id); // query limited information and synchronize
            if (process == IntPtr.Zero) {
                int error = Marshal.GetLastWin32Error();
                if (error == 87) return null; // Process has already exited.
                throw new Win32Exception(error, "Cannot inspect running Launcher process " + id + ".");
            }
            RunningProcess pinned = new RunningProcess();
            pinned.handle = process;
            try {
                if (WaitForSingleObject(process, 0) == 0) { pinned.Dispose(); return null; }
                StringBuilder path = new StringBuilder(32768); int length = path.Capacity;
                long created, exited, kernel, user; int session; IntPtr token;
                if (!QueryFullProcessImageName(process, 0, path, ref length) ||
                    !GetProcessTimes(process, out created, out exited, out kernel, out user) ||
                    !ProcessIdToSessionId(id, out session) || !OpenProcessToken(process, 8, out token)) {
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot verify running Launcher process " + id + ".");
                }
                try { using (WindowsIdentity identity = new WindowsIdentity(token)) { pinned.OwnerSid = identity.User.Value; } }
                finally { CloseHandle(token); }
                pinned.ProcessId = id; pinned.Path = path.ToString(); pinned.SessionId = session;
                pinned.CreationTimeUtc = DateTime.FromFileTimeUtc(created);
                return pinned;
            } catch { pinned.Dispose(); throw; }
        }
        List<IntPtr> MainWindows() {
            List<IntPtr> windows = new List<IntPtr>();
            if (!EnumWindows(delegate(IntPtr window, IntPtr argument) {
                int id; GetWindowThreadProcessId(window, out id);
                if (id == ProcessId) {
                    StringBuilder name = new StringBuilder(128); GetClassName(window, name, name.Capacity);
                    if (name.ToString() == "Launcher.DrawingPrototype.Window") windows.Add(window);
                }
                return true;
            }, IntPtr.Zero)) throw new Win32Exception(Marshal.GetLastWin32Error());
            if (windows.Count == 0 && WaitForSingleObject(handle, 0) != 0) {
                throw new InvalidOperationException("Launcher has no accessible main window; close it and try again.");
            }
            return windows;
        }
        public IntPtr MainWindow {
            get {
                if (WaitForSingleObject(handle, 0) == 0) throw new InvalidOperationException("The process has already exited.");
                List<IntPtr> windows = MainWindows();
                if (windows.Count != 1) throw new InvalidOperationException("The process does not have exactly one accessible main window.");
                return windows[0];
            }
        }
        public bool IsRunning { get { return WaitForSingleObject(handle, 0) == 0x102; } }
        public void Close(int timeout) {
            if (WaitForSingleObject(handle, 0) == 0) return;
            List<IntPtr> windows = MainWindows();
            Stopwatch elapsed = Stopwatch.StartNew();
            foreach (IntPtr window in windows) {
                int id; GetWindowThreadProcessId(window, out id);
                if (id != ProcessId) continue;
                IntPtr ignored; uint remaining = (uint)Math.Max(1, timeout - elapsed.ElapsedMilliseconds);
                if (SendMessageTimeout(window, 0x10, IntPtr.Zero, IntPtr.Zero, 3, remaining, out ignored) == IntPtr.Zero &&
                    WaitForSingleObject(handle, 0) != 0) {
                    throw new InvalidOperationException("Launcher did not accept a graceful close; close it and try again.");
                }
            }
            if (WaitForSingleObject(handle, (uint)Math.Max(0, timeout - elapsed.ElapsedMilliseconds)) != 0) {
                throw new InvalidOperationException("Launcher did not exit in time; close it and try again.");
            }
        }
        public void Dispose() { if (handle != IntPtr.Zero) { CloseHandle(handle); handle = IntPtr.Zero; } }
    }
}
'@
}

function Get-LauncherProcessCandidates {
    Initialize-LauncherLifecycleNative
    [LauncherSetup.RunningProcess]::Candidates()
}
