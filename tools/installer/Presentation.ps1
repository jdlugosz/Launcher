#requires -Version 5.1
# Loaded before WinForms creates any windows. The PowerShell host's process-wide
# manifest is outside this script's control, so set awareness on this UI thread.
if (-not ('LauncherSetup.NativePresentation' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace LauncherSetup {
    public static class NativePresentation {
        [DllImport("user32.dll", SetLastError=true)]
        public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr value);
        [DllImport("user32.dll")]
        public static extern IntPtr GetThreadDpiAwarenessContext();
        [DllImport("user32.dll")]
        public static extern IntPtr GetWindowDpiAwarenessContext(IntPtr window);
        [DllImport("user32.dll")]
        public static extern bool AreDpiAwarenessContextsEqual(IntPtr a, IntPtr b);
        [DllImport("user32.dll")]
        public static extern uint GetDpiForWindow(IntPtr window);
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        public static extern IntPtr LoadLibraryEx(string path, IntPtr reserved, uint flags);
        [DllImport("kernel32.dll")]
        public static extern bool FreeLibrary(IntPtr module);
        [DllImport("user32.dll", EntryPoint="LoadImageW", SetLastError=true)]
        public static extern IntPtr LoadImage(IntPtr module, IntPtr name, uint type, int cx, int cy, uint flags);
        [DllImport("user32.dll")]
        public static extern bool DestroyIcon(IntPtr icon);
    }
}
'@
}

function Initialize-LauncherPresentation {
    $previous = [LauncherSetup.NativePresentation]::SetThreadDpiAwarenessContext([IntPtr]::new(-4))
    if ($previous -eq [IntPtr]::Zero) {
        throw 'Windows could not enable per-monitor DPI awareness for the setup window.'
    }
    return $previous
}

function Restore-LauncherPresentation {
    param([IntPtr]$PreviousContext)
    if ($PreviousContext -ne [IntPtr]::Zero) {
        [void][LauncherSetup.NativePresentation]::SetThreadDpiAwarenessContext($PreviousContext)
    }
}

function Get-LauncherArtworkPath {
    param([string]$PackageDirectory)
    if (-not (Get-Command Get-LauncherPackageExecutable -ErrorAction SilentlyContinue)) {
        . (Join-Path $PSScriptRoot 'Discovery.ps1')
    }
    $adjacent = @(Get-ChildItem -LiteralPath $PackageDirectory -File -ErrorAction Stop | Where-Object {
        $_.Name -ieq 'Launcher.exe' -or $_.Name -imatch '^Launcher-R\d+\.\d+(?:\.\d+){0,2}\.exe$'
    })
    if ($adjacent.Count) { return (Get-LauncherPackageExecutable -PackageDirectory $PackageDirectory) }
    # Source-tree previews may use the regular Release build. A shipped package
    # always loads the adjacent executable; there is no loose icon dependency.
    $sourceRoot = Split-Path (Split-Path $PackageDirectory -Parent) -Parent
    if (Test-Path -LiteralPath (Join-Path $sourceRoot 'CMakeLists.txt') -PathType Leaf) {
        $development = Join-Path $sourceRoot 'build\windows-x64\Release\Launcher.exe'
        if (Test-Path -LiteralPath $development -PathType Leaf) { return $development }
    }
    throw 'The Launcher executable is missing beside Setup. Extract the complete ZIP before running Setup.'
}

function Get-LauncherResourceIcon {
    param([string]$ExecutablePath, [int]$Size = 256)
    # DATAFILE | IMAGE_RESOURCE: read resources without loading executable code.
    $module = [LauncherSetup.NativePresentation]::LoadLibraryEx($ExecutablePath, [IntPtr]::Zero, 0x22)
    if ($module -eq [IntPtr]::Zero) {
        throw "Could not read Launcher artwork: $ExecutablePath"
    }
    $nativeIcon = [IntPtr]::Zero
    try {
        # The icon group contains an exact 256px frame. Request that frame, then
        # draw it smaller; do not let a shell API hand us a 32px thumbnail.
        $nativeIcon = [LauncherSetup.NativePresentation]::LoadImage(
            $module, [IntPtr]::new(101), 1, $Size, $Size, 0)
        if ($nativeIcon -eq [IntPtr]::Zero) {
            throw 'Could not read the full-size embedded Launcher icon.'
        }
        # The managed clone owns independent pixels after the native handles close.
        $borrowed = [System.Drawing.Icon]::FromHandle($nativeIcon)
        try { return [System.Drawing.Icon]$borrowed.Clone() }
        finally { $borrowed.Dispose() }
    } finally {
        if ($nativeIcon -ne [IntPtr]::Zero) { [void][LauncherSetup.NativePresentation]::DestroyIcon($nativeIcon) }
        [void][LauncherSetup.NativePresentation]::FreeLibrary($module)
    }
}

function Add-LauncherDpiFormType {
    if ('LauncherSetup.DpiForm' -as [type]) { return }
    # Framework WinForms dynamic DPI support normally depends on powershell.exe's
    # app.config. Handle WM_DPICHANGED here instead of changing that shared host.
    # Pixel fonts make both layout and font scaling explicit and avoid cached
    # system-DPI font handles when a window moves between different monitors.
    Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;
using System.Windows.Forms;
namespace LauncherSetup {
    public sealed class TopTabsIllustration : Control {
        public TopTabsIllustration() {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            Size = new Size(420, 190);
            TabStop = false;
            AccessibleRole = AccessibleRole.Graphic;
            AccessibleName = "A screen with four colored Launcher tabs along its top edge and an arrow pointing up to them.";
        }
        protected override void OnPaint(PaintEventArgs e) {
            base.OnPaint(e);
            float scale = Math.Min(ClientSize.Width / 420f, ClientSize.Height / 190f);
            if (scale <= 0) return;
            Graphics g = e.Graphics;
            GraphicsState state = g.Save();
            try {
                g.ScaleTransform(scale, scale);
                g.SmoothingMode = SmoothingMode.AntiAlias;
                using (var frame = new SolidBrush(Color.FromArgb(77, 94, 111)))
                using (var desktop = new SolidBrush(Color.FromArgb(237, 245, 252)))
                using (var arrow = new SolidBrush(Color.FromArgb(40, 106, 166)))
                using (var outline = new Pen(Color.FromArgb(100, 100, 100), 1f)) {
                    g.FillRectangle(frame, 0, 0, 420, 164);
                    g.FillRectangle(desktop, 6, 6, 408, 152);
                    g.FillRectangle(frame, 196, 164, 28, 17);
                    g.FillRectangle(frame, 158, 181, 104, 5);
                    Color[] colors = { Color.Salmon, Color.Orange, Color.FromArgb(255, 225, 90), Color.LightGreen };
                    for (int i = 0; i < colors.Length; i++) {
                        int x = 66 + i * 72;
                        Point[] tab = { new Point(x, 6), new Point(x + 72, 6),
                            new Point(x + 64, 30), new Point(x + 8, 30) };
                        using (var fill = new SolidBrush(colors[i])) g.FillPolygon(fill, tab);
                        g.DrawLines(outline, new[] { tab[0], tab[3], tab[2], tab[1] });
                    }
                    g.FillPolygon(arrow, new[] { new Point(210, 46), new Point(194, 66),
                        new Point(205, 66), new Point(205, 117), new Point(215, 117),
                        new Point(215, 66), new Point(226, 66) });
                }
            } finally { g.Restore(state); }
        }
    }
    public sealed class RecoveryArrow : Control {
        public RecoveryArrow() {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            MinimumSize = new Size(48, 48);
            Size = MinimumSize;
            TabStop = false;
        }
        public override Size GetPreferredSize(Size proposedSize) { return MinimumSize; }
        protected override void OnPaint(PaintEventArgs e) {
            base.OnPaint(e);
            float scale = Math.Min(ClientSize.Width, ClientSize.Height) / 48f;
            if (scale <= 0) return;
            Graphics g = e.Graphics;
            GraphicsState state = g.Save();
            try {
                g.TranslateTransform(0, (ClientSize.Height - 48 * scale) / 2);
                g.ScaleTransform(scale, scale);
                g.SmoothingMode = SmoothingMode.AntiAlias;
                using (var stroke = new Pen(ForeColor, 3)) {
                    stroke.StartCap = LineCap.Round;
                    stroke.EndCap = LineCap.Round;
                    stroke.LineJoin = LineJoin.Round;
                    g.DrawBezier(stroke, 6, 9, 6, 31, 35, 17, 35, 39);
                    g.DrawLines(stroke, new[] { new Point(27, 31), new Point(35, 39), new Point(43, 31) });
                }
            } finally { g.Restore(state); }
        }
    }
    internal static class SetupTaskbar {
        [StructLayout(LayoutKind.Sequential)]
        private struct PropertyKey {
            public Guid Format;
            public uint Id;
            public PropertyKey(uint id) {
                Format = new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3");
                Id = id;
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct PropVariant {
            public ushort Type, Reserved1, Reserved2, Reserved3;
            public IntPtr Value, Padding;
        }

        [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IPropertyStore {
            [PreserveSig] int GetCount(out uint count);
            [PreserveSig] int GetAt(uint index, out PropertyKey key);
            [PreserveSig] int GetValue(ref PropertyKey key, out PropVariant value);
            [PreserveSig] int SetValue(ref PropertyKey key, ref PropVariant value);
        }

        [DllImport("shell32.dll", ExactSpelling = true)]
        private static extern int SHGetPropertyStoreForWindow(IntPtr window, ref Guid iid, out IPropertyStore store);
        [DllImport("ole32.dll", ExactSpelling = true)]
        private static extern int PropVariantClear(ref PropVariant value);

        private const string AppId = "Dlugosz.Launcher.Setup";
        private const uint RelaunchIconResource = 3;
        private const uint AppUserModelId = 5;
        private const ushort StringVariant = 31; // VT_LPWSTR

        private static IPropertyStore Open(IntPtr window) {
            Guid iid = typeof(IPropertyStore).GUID;
            IPropertyStore store;
            return SHGetPropertyStoreForWindow(window, ref iid, out store) >= 0 ? store : null;
        }

        private static bool Write(IPropertyStore store, uint id, string text) {
            var key = new PropertyKey(id);
            var value = new PropVariant(); // VT_EMPTY removes the property before destroying the window.
            if (text != null) {
                value.Type = StringVariant;
                value.Value = Marshal.StringToCoTaskMemUni(text);
            }
            try { return store.SetValue(ref key, ref value) >= 0; }
            finally { PropVariantClear(ref value); }
        }

        private static string Read(IPropertyStore store, uint id) {
            var key = new PropertyKey(id);
            PropVariant value;
            if (store.GetValue(ref key, out value) < 0) return null;
            try { return value.Type == StringVariant ? Marshal.PtrToStringUni(value.Value) : null; }
            finally { PropVariantClear(ref value); }
        }

        public static bool Apply(IntPtr window, string iconResource) {
            IPropertyStore store = Open(window);
            if (store == null) return false;
            try {
                // Assign the icon first. Setting the explicit ID refreshes the taskbar.
                if (!Write(store, RelaunchIconResource, iconResource)) return false;
                if (Write(store, AppUserModelId, AppId)) return true;
                Write(store, RelaunchIconResource, null);
                return false;
            } finally { Marshal.ReleaseComObject(store); }
        }

        public static bool Matches(IntPtr window, string iconResource) {
            IPropertyStore store = Open(window);
            if (store == null) return false;
            try { return Read(store, AppUserModelId) == AppId && Read(store, RelaunchIconResource) == iconResource; }
            finally { Marshal.ReleaseComObject(store); }
        }

        public static void Clear(IntPtr window) {
            IPropertyStore store = Open(window);
            if (store == null) return;
            try {
                Write(store, RelaunchIconResource, null);
                Write(store, AppUserModelId, null);
            } finally { Marshal.ReleaseComObject(store); }
        }
    }
    public class DpiForm : Form {
        [StructLayout(LayoutKind.Sequential)]
        private struct Rect { public int Left, Top, Right, Bottom; }
        [DllImport("user32.dll")]
        private static extern uint GetDpiForWindow(IntPtr window);
        private readonly Dictionary<Control, Font> designFonts = new Dictionary<Control, Font>();
        private readonly List<Font> ownedFonts = new List<Font>();
        private bool taskbarIdentityApplied;
        public int LayoutDpi { get; private set; }
        public int DpiTransitionCount { get; private set; }
        public bool RenderWithoutActivation { get; set; }
        public string TaskbarIconResource { get; set; }
        public event EventHandler LayoutDpiChanged;
        public DpiForm() { LayoutDpi = 96; }
        protected override bool ShowWithoutActivation {
            get { return RenderWithoutActivation || base.ShowWithoutActivation; }
        }
        protected override CreateParams CreateParams {
            get {
                CreateParams parameters = base.CreateParams;
                if (RenderWithoutActivation) parameters.ExStyle |= 0x08000000;
                return parameters;
            }
        }

        public void FitToWorkingArea() {
            Rectangle area = Screen.FromHandle(Handle).WorkingArea;
            MinimumSize = new Size(Math.Min(MinimumSize.Width, area.Width), Math.Min(MinimumSize.Height, area.Height));
            int width = Math.Min(Width, area.Width), height = Math.Min(Height, area.Height);
            int left = Math.Max(area.Left, Math.Min(Left, area.Right - width));
            int top = Math.Max(area.Top, Math.Min(Top, area.Bottom - height));
            Bounds = new Rectangle(left, top, width, height);
        }

        private void Visit(Control parent, Action<Control> action) {
            action(parent);
            foreach (Control child in parent.Controls) Visit(child, action);
        }
        private void SetLayoutDpi(int dpi) {
            if (dpi <= 0 || dpi == LayoutDpi) return;
            if (designFonts.Count == 0) Visit(this, c => designFonts[c] = c.Font);
            Visit(this, c => c.SuspendLayout());
            float ratio = (float)dpi / LayoutDpi;
            Scale(new SizeF(ratio, ratio));
            Font[] previousFonts = ownedFonts.ToArray();
            ownedFonts.Clear();
            foreach (var entry in designFonts) {
                Font source = entry.Value;
                float designPixels = source.Unit == GraphicsUnit.Pixel ? source.Size : source.SizeInPoints * 96f / 72f;
                var font = new Font(source.FontFamily, designPixels * dpi / 96f, source.Style, GraphicsUnit.Pixel);
                ownedFonts.Add(font);
                entry.Key.Font = font;
            }
            foreach (Font font in previousFonts) font.Dispose();
            LayoutDpi = dpi;
            Visit(this, c => c.ResumeLayout(true));
            PerformLayout();
            if (LayoutDpiChanged != null) LayoutDpiChanged(this, EventArgs.Empty);
        }
        protected override void OnHandleCreated(EventArgs e) {
            base.OnHandleCreated(e);
            if (!string.IsNullOrEmpty(TaskbarIconResource)) {
                taskbarIdentityApplied = SetupTaskbar.Apply(Handle, TaskbarIconResource);
            }
            SetLayoutDpi((int)GetDpiForWindow(Handle));
            FitToWorkingArea();
        }
        protected override void OnShown(EventArgs e) {
            FitToWorkingArea();
            base.OnShown(e);
        }
        protected override void WndProc(ref Message message) {
            if (message.Msg == 0x0002 && taskbarIdentityApplied) { // WM_DESTROY
                SetupTaskbar.Clear(message.HWnd);
                taskbarIdentityApplied = false;
            }
            if (message.Msg == 0x02E0) {
                int dpi = (int)(message.WParam.ToInt64() & 0xffff);
                Rect bounds = (Rect)Marshal.PtrToStructure(message.LParam, typeof(Rect));
                SetLayoutDpi(dpi);
                Bounds = Rectangle.FromLTRB(bounds.Left, bounds.Top, bounds.Right, bounds.Bottom);
                FitToWorkingArea();
                DpiTransitionCount++;
                message.Result = IntPtr.Zero;
                return;
            }
            base.WndProc(ref message);
        }
        // Rendering tests can exercise another layout DPI without moving a real window.
        public void SetRenderDpi(int dpi) { SetLayoutDpi(dpi); FitToWorkingArea(); }
        public bool HasSetupTaskbarIdentity() { return IsHandleCreated && SetupTaskbar.Matches(Handle, TaskbarIconResource); }
        protected override void Dispose(bool disposing) {
            base.Dispose(disposing);
            if (disposing) foreach (Font font in ownedFonts) font.Dispose();
        }
    }
}
'@
}
