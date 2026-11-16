using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Windows.Forms;

namespace Dlugosz.Win32 {
    // This transport has no application-specific message names or text format.
    // The receiver HWND and cookie belong to this call only; the target replies
    // synchronously with WM_COPYDATA before returning from the registered query.
    public static class WindowTextQuery {
        const int CopyDataMessage = 0x004a;
        const uint AbortIfHung = 0x0002, ErrorOnExit = 0x0020;
        const uint MaximumTextBytes = 32767 * 2;

        [StructLayout(LayoutKind.Sequential)]
        struct CopyData {
            public IntPtr Cookie;
            public uint ByteCount;
            public IntPtr Data;
        }

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern uint RegisterWindowMessage(string name);
        [DllImport("user32.dll", SetLastError = true)]
        static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr word, IntPtr data, uint flags, uint milliseconds, out IntPtr result);
        [DllImport("user32.dll")]
        static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
        [DllImport("user32.dll")]
        static extern bool IsWindow(IntPtr window);
        [DllImport("kernel32.dll")]
        static extern void SetLastError(uint error);

        static bool HasOwner(IntPtr window, uint process) {
            uint actual;
            return IsWindow(window) && GetWindowThreadProcessId(window, out actual) != 0 && actual == process;
        }

        static IntPtr NewCookie() {
            byte[] bytes = new byte[IntPtr.Size];
            using (RandomNumberGenerator random = RandomNumberGenerator.Create()) {
                for (;;) {
                    random.GetBytes(bytes);
                    IntPtr cookie = IntPtr.Size == 8 ? new IntPtr(BitConverter.ToInt64(bytes, 0)) : new IntPtr(BitConverter.ToInt32(bytes, 0));
                    if (cookie != IntPtr.Zero) return cookie;
                }
            }
        }

        sealed class ReplyWindow : NativeWindow, IDisposable {
            readonly IntPtr target, cookie;
            readonly uint process;
            public bool Received { get; private set; }
            public string Text { get; private set; }
            public Exception Failure { get; private set; }

            public ReplyWindow(IntPtr target, uint process, IntPtr cookie) {
                this.target = target; this.process = process; this.cookie = cookie;
                CreateParams parameters = new CreateParams();
                parameters.Parent = new IntPtr(-3); // HWND_MESSAGE: temporary, never visible.
                CreateHandle(parameters);
            }

            protected override void WndProc(ref Message message) {
                if (message.Msg != CopyDataMessage) { base.WndProc(ref message); return; }
                message.Result = IntPtr.Zero;
                if (Received || Failure != null || message.WParam != target || !HasOwner(target, process)) return;
                try {
                    if (message.LParam == IntPtr.Zero) throw new InvalidDataException("The text response has no COPYDATASTRUCT.");
                    CopyData data = (CopyData)Marshal.PtrToStructure(message.LParam, typeof(CopyData));
                    if (data.Cookie != cookie) return;
                    if (data.ByteCount < 2 || data.ByteCount > MaximumTextBytes || (data.ByteCount & 1) != 0 || data.Data == IntPtr.Zero) {
                        throw new InvalidDataException("The text response has an invalid UTF-16 byte count or buffer.");
                    }
                    char[] characters = new char[data.ByteCount / 2];
                    Marshal.Copy(data.Data, characters, 0, characters.Length);
                    if (characters[characters.Length - 1] != '\0' || Array.IndexOf(characters, '\0') != characters.Length - 1) {
                        throw new InvalidDataException("The text response must have exactly one terminal NUL.");
                    }
                    // WM_COPYDATA owns its buffer only until this callback returns.
                    Text = new string(characters, 0, characters.Length - 1);
                    Received = true;
                    message.Result = new IntPtr(1);
                } catch (Exception error) {
                    // Never unwind a managed exception through the native window procedure.
                    Failure = error;
                    message.Result = IntPtr.Zero;
                }
            }

            public void Dispose() { DestroyHandle(); }
        }

        public static string Read(IntPtr target, string requestName, int timeoutMs) {
            return ReadCore(target, requestName, timeoutMs, null);
        }

        public static string Read(IntPtr target, string requestName, int timeoutMs, int expectedProcessId) {
            if (expectedProcessId <= 0) throw new ArgumentOutOfRangeException("expectedProcessId", "A positive expected process ID is required.");
            return ReadCore(target, requestName, timeoutMs, (uint)expectedProcessId);
        }

        static string ReadCore(IntPtr target, string requestName, int timeoutMs, uint? expectedProcessId) {
            if (target == IntPtr.Zero) throw new ArgumentException("A target window is required.", "target");
            if (String.IsNullOrEmpty(requestName) || requestName.IndexOf('\0') >= 0 || requestName.Length > 255) {
                throw new ArgumentException("A registered message name of 1 through 255 characters is required.", "requestName");
            }
            if (timeoutMs < 1 || timeoutMs > 30000) throw new ArgumentOutOfRangeException("timeoutMs", "The query timeout must be 1 through 30000 milliseconds.");
            uint process;
            if (!IsWindow(target) || GetWindowThreadProcessId(target, out process) == 0) {
                throw new InvalidOperationException("The target window no longer exists.");
            }
            if (expectedProcessId.HasValue && process != expectedProcessId.Value) {
                throw new InvalidOperationException("The target window belongs to a different process than expected.");
            }
            uint request = RegisterWindowMessage(requestName);
            if (request == 0) throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot register the window text query.");
            IntPtr cookie = NewCookie();
            using (ReplyWindow receiver = new ReplyWindow(target, process, cookie)) {
                IntPtr result;
                SetLastError(0);
                // Deliberately omit SMTO_BLOCK: the reverse sent message must be
                // dispatched on this thread while this outer SendMessage waits.
                IntPtr completed = SendMessageTimeout(target, request, receiver.Handle, cookie, AbortIfHung | ErrorOnExit, (uint)timeoutMs, out result);
                int error = Marshal.GetLastWin32Error();
                if (!HasOwner(target, process)) throw new InvalidOperationException("The target window was destroyed or replaced during the query.");
                if (completed == IntPtr.Zero) {
                    if (error == 0 || error == 1460) throw new TimeoutException("The target window did not finish the text query before the timeout.");
                    throw new Win32Exception(error, "Cannot query text from the target window.");
                }
                if (receiver.Failure != null) throw new InvalidDataException("The target window returned an invalid text response.", receiver.Failure);
                if (receiver.Received) return receiver.Text;
                if (result == IntPtr.Zero) throw new NotSupportedException("The target window does not support this text query.");
                throw new InvalidOperationException("The target window acknowledged the text query without returning a matching response.");
            }
        }
    }
}
