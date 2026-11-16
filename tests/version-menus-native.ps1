[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $Launcher,
    [Parameter(Mandatory)][string] $OutputDirectory,
    [string] $Probe,
    [ValidateSet('All', 'Basic', 'Paged', 'Admin', 'Default')][string] $Mode = 'All',
    [switch] $CaptureImages
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3
$menuLauncher = [IO.Path]::GetFullPath($Launcher)
$menuVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($menuLauncher)
if (!$menuVersion.IsSpecialBuild -or !$menuVersion.SpecialBuild.Contains('version-scenarios')) {
    throw 'Native menu acceptance requires the separately instrumented version-scenarios executable.'
}
if ($Probe) {
    $Probe = [IO.Path]::GetFullPath($Probe)
    if (!(Test-Path -LiteralPath $Probe -PathType Leaf)) { throw 'The optional owned GUI probe executable is missing.' }
}
if ($Mode -eq 'Admin' -and !$Probe) { throw 'Admin coverage requires -Probe with the harmless LauncherLaunchGuiProbe executable.' }
if ($CaptureImages) { Add-Type -AssemblyName System.Drawing }
if (!('LauncherVersionMenuNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class LauncherVersionMenuNative {
    public delegate bool Callback(IntPtr window, IntPtr parameter);
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X,Y; }
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct GuiInfo {
        public uint Size,Flags; public IntPtr Active,Focus,Capture,MenuOwner,MoveSize,Caret; public Rect CaretRect;
    }
    [StructLayout(LayoutKind.Sequential)] struct MouseInput { public int X,Y; public uint Data,Flags,Time; public UIntPtr Extra; }
    [StructLayout(LayoutKind.Sequential)] struct KeyInput { public ushort Key,Scan; public uint Flags,Time; public UIntPtr Extra; }
    [StructLayout(LayoutKind.Explicit)] struct InputData { [FieldOffset(0)] public MouseInput Mouse; [FieldOffset(0)] public KeyInput Key; }
    [StructLayout(LayoutKind.Sequential)] struct Input { public uint Type; public InputData Data; }
    public sealed class Popup { public IntPtr Window,Menu; }
    public sealed class Row { public int Position; public uint Id,State; public IntPtr Submenu; public string Text; public Rect Bounds; }
    [DllImport("user32.dll")] static extern bool EnumWindows(Callback callback, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr window,StringBuilder text,int size);
    [DllImport("user32.dll",SetLastError=true)] public static extern bool PostMessage(IntPtr window,uint message,UIntPtr word,IntPtr parameter);
    [DllImport("user32.dll",SetLastError=true)] static extern IntPtr SendMessageTimeout(IntPtr window,uint message,UIntPtr word,IntPtr parameter,uint flags,uint timeout,out UIntPtr result);
    [DllImport("user32.dll")] static extern int GetMenuItemCount(IntPtr menu);
    [DllImport("user32.dll")] static extern uint GetMenuItemID(IntPtr menu,int position);
    [DllImport("user32.dll")] static extern uint GetMenuState(IntPtr menu,uint position,uint flags);
    [DllImport("user32.dll")] static extern IntPtr GetSubMenu(IntPtr menu,int position);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetMenuString(IntPtr menu,uint position,StringBuilder text,int size,uint flags);
    [DllImport("user32.dll")] static extern bool GetMenuItemRect(IntPtr owner,IntPtr menu,uint position,out Rect rectangle);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window,out Rect rectangle);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window,IntPtr dc,uint flags);
    [DllImport("user32.dll")] static extern bool GetGUIThreadInfo(uint thread,ref GuiInfo info);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr SetActiveWindow(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr SetFocus(IntPtr window);
    [DllImport("user32.dll")] static extern bool AttachThreadInput(uint thread,uint other,bool attach);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint access,bool inherit,uint process);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool QueryFullProcessImageName(IntPtr process,uint flags,StringBuilder image,ref int size);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(Point point);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")] static extern int GetSystemMetrics(int metric);
    [DllImport("user32.dll",SetLastError=true)] static extern uint SendInput(uint count,Input[] inputs,int size);
    static string Class(IntPtr window) { var value=new StringBuilder(128); GetClassName(window,value,value.Capacity); return value.ToString(); }
    public static void DpiAware() { SetThreadDpiAwarenessContext(new IntPtr(-4)); }
    public static long WindowProcessId(IntPtr window) { uint process; GetWindowThreadProcessId(window,out process); return process; }
    public static string ProcessImage(uint process) {
        // PROCESS_QUERY_LIMITED_INFORMATION works across x86/x64. Managed
        // Process.MainModule can be null for a 64-bit fixture in a 32-bit host.
        var handle=OpenProcess(0x1000,false,process);
        if(handle==IntPtr.Zero) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),"Cannot query the owned fixture process.");
        try {
            var image=new StringBuilder(32768); int size=image.Capacity;
            if(!QueryFullProcessImageName(handle,0,image,ref size)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),"Cannot verify the owned fixture executable.");
            return image.ToString();
        } finally { CloseHandle(handle); }
    }
    public static IntPtr Window(uint process) {
        IntPtr found=IntPtr.Zero;
        EnumWindows((window,unused)=>{ if(WindowProcessId(window)==process && Class(window)=="Launcher.DrawingPrototype.Window") { found=window; return false; } return true; },IntPtr.Zero);
        return found;
    }
    static void Validate(IntPtr window,uint process) {
        if(window==IntPtr.Zero || !IsWindow(window) || WindowProcessId(window)!=process) throw new InvalidOperationException("Native input target is not owned by the fixture process.");
    }
    public static Popup[] Popups(uint process) {
        var result=new List<Popup>();
        EnumWindows((window,unused)=>{
            if(WindowProcessId(window)==process && IsWindowVisible(window) && Class(window)=="#32768") {
                UIntPtr value;
                if(SendMessageTimeout(window,0x1e1,UIntPtr.Zero,IntPtr.Zero,3,500,out value)!=IntPtr.Zero && value!=UIntPtr.Zero) {
                    result.Add(new Popup { Window=window,Menu=new IntPtr(unchecked((long)value.ToUInt64())) });
                }
            }
            return true;
        },IntPtr.Zero);
        return result.ToArray();
    }
    public static Row[] Rows(IntPtr menu,uint process) {
        DpiAware();
        var owner=Window(process); Validate(owner,process);
        var rows=new List<Row>(); int count=GetMenuItemCount(menu);
        if(count<0 || count>16384) throw new InvalidOperationException("Invalid native fixture menu.");
        for(int position=0;position<count;++position) {
            var text=new StringBuilder(32768); GetMenuString(menu,(uint)position,text,text.Capacity,0x400);
            Rect bounds; GetMenuItemRect(owner,menu,(uint)position,out bounds);
            rows.Add(new Row { Position=position,Id=GetMenuItemID(menu,position),State=GetMenuState(menu,(uint)position,0x400),Submenu=GetSubMenu(menu,position),Text=text.ToString(),Bounds=bounds });
        }
        return rows.ToArray();
    }
    public static GuiInfo Gui(uint process) {
        var owner=Window(process); Validate(owner,process); uint ignored;
        var thread=GetWindowThreadProcessId(owner,out ignored); var info=new GuiInfo { Size=(uint)Marshal.SizeOf(typeof(GuiInfo)) };
        if(!GetGUIThreadInfo(thread,ref info)) throw new InvalidOperationException("Cannot inspect fixture menu/capture state.");
        return info;
    }
    static void Inject(params Input[] input) {
        int size=Marshal.SizeOf(typeof(Input));
        uint accepted=SendInput((uint)input.Length,input,size);
        if(accepted!=(uint)input.Length) throw new InvalidOperationException("Native menu input was not accepted: count="+accepted+"; expected="+input.Length+"; structure="+size+"; hostBits="+(IntPtr.Size*8)+"; error="+Marshal.GetLastWin32Error()+".");
    }
    public static void Move(int x,int y) {
        DpiAware();
        int left=GetSystemMetrics(76),top=GetSystemMetrics(77),width=GetSystemMetrics(78),height=GetSystemMetrics(79);
        if(width<=1 || height<=1 || x<left || y<top || x>=left+width || y>=top+height) throw new InvalidOperationException("Native menu point is outside the virtual desktop.");
        Inject(new Input { Type=0,Data=new InputData { Mouse=new MouseInput { X=(int)(((long)x-left)*65535/(width-1)),Y=(int)(((long)y-top)*65535/(height-1)),Flags=0xc001 } } });
    }
    public static void Mouse(uint process,IntPtr menu,int position,bool right) {
        var rows=Rows(menu,process); if(position<0 || position>=rows.Length) throw new InvalidOperationException("Native menu position is invalid.");
        var rectangle=rows[position].Bounds;
        if(rectangle.Right<=rectangle.Left || rectangle.Bottom<=rectangle.Top) throw new InvalidOperationException("Native menu row has no visible rectangle.");
        Move((rectangle.Left+rectangle.Right)/2,(rectangle.Top+rectangle.Bottom)/2);
        var hit=WindowFromPoint(new Point { X=(rectangle.Left+rectangle.Right)/2,Y=(rectangle.Top+rectangle.Bottom)/2 });
        if(WindowProcessId(hit)!=process || Class(hit)!="#32768") throw new InvalidOperationException("The native menu row is not exposed at its owned screen point.");
        Inject(new Input { Type=0,Data=new InputData { Mouse=new MouseInput { Flags=right ? 8u : 2u } } },
            new Input { Type=0,Data=new InputData { Mouse=new MouseInput { Flags=right ? 16u : 4u } } });
    }
    public static bool Hover(uint process,IntPtr menu,int position) {
        var rows=Rows(menu,process); if(position<0 || position>=rows.Length) return false;
        var rectangle=rows[position].Bounds;
        if(rectangle.Right<=rectangle.Left || rectangle.Bottom<=rectangle.Top) return false;
        var point=new Point { X=(rectangle.Left+rectangle.Right)/2,Y=(rectangle.Top+rectangle.Bottom)/2 };
        var hit=WindowFromPoint(point);
        if(WindowProcessId(hit)!=process || Class(hit)!="#32768") return false;
        // Send a real movement even when the previous observation left the
        // pointer at the row center. Windows otherwise need not select it.
        Move(point.X+1,point.Y);
        Move(point.X,point.Y);
        return true;
    }
    static Input KeyboardEvent(ushort key,bool up) {
        uint extended=(key>=0x21 && key<=0x28) || key==0x5d ? 1u : 0u;
        return new Input { Type=1,Data=new InputData { Key=new KeyInput { Key=key,Flags=extended | (up ? 2u : 0u) } } };
    }
    static void KeyboardOwner(uint process) {
        var window=Window(process); Validate(window,process);
        if(WindowProcessId(GetForegroundWindow())!=process) FocusWindow(window);
        if(WindowProcessId(GetForegroundWindow())!=process) {
            var foregroundInfo=new GuiInfo { Size=(uint)Marshal.SizeOf(typeof(GuiInfo)) };
            GetGUIThreadInfo(0,ref foregroundInfo);
            if(WindowProcessId(foregroundInfo.MenuOwner)==process && (foregroundInfo.Flags&0x14)!=0) return;
            var owned=Gui(process);
            throw new InvalidOperationException("The owned menu is not the foreground keyboard target: foregroundPID="+WindowProcessId(GetForegroundWindow())+
                "; globalFlags="+foregroundInfo.Flags+"; globalMenuPID="+WindowProcessId(foregroundInfo.MenuOwner)+"; globalFocusPID="+WindowProcessId(foregroundInfo.Focus)+
                "; ownFlags="+owned.Flags+"; ownMenuPID="+WindowProcessId(owned.MenuOwner)+"; ownFocusPID="+WindowProcessId(owned.Focus));
        }
    }
    static void FocusWindow(IntPtr window) {
        uint ignored; uint current=GetCurrentThreadId();
        uint foreground=GetWindowThreadProcessId(GetForegroundWindow(),out ignored);
        uint target=GetWindowThreadProcessId(window,out ignored);
        bool attachedForeground=foreground!=0 && foreground!=current && AttachThreadInput(current,foreground,true);
        bool attachedTarget=target!=0 && target!=current && target!=foreground && AttachThreadInput(current,target,true);
        try { SetForegroundWindow(window); SetActiveWindow(window); SetFocus(window); }
        finally {
            if(attachedTarget) AttachThreadInput(current,target,false);
            if(attachedForeground) AttachThreadInput(current,foreground,false);
        }
    }
    public static void FocusFixture(uint process) { var window=Window(process); Validate(window,process); FocusWindow(window); }
    public static void Key(uint process,ushort key) { KeyboardOwner(process); Inject(KeyboardEvent(key,false),KeyboardEvent(key,true)); }
    public static void Context(uint process,bool shiftF10) {
        KeyboardOwner(process);
        if(shiftF10) Inject(KeyboardEvent(0x10,false),KeyboardEvent(0x79,false),KeyboardEvent(0x79,true),KeyboardEvent(0x10,true));
        else Inject(KeyboardEvent(0x5d,false),KeyboardEvent(0x5d,true));
    }
    public static bool Barrier(uint process) { UIntPtr ignored; return SendMessageTimeout(Window(process),0,UIntPtr.Zero,IntPtr.Zero,3,2000,out ignored)!=IntPtr.Zero; }
    public static void Cancel(uint process) {
        var owner=Window(process);
        if(owner!=IntPtr.Zero) {
            PostMessage(owner,0x1f,UIntPtr.Zero,IntPtr.Zero);
            PostMessage(owner,0x10,UIntPtr.Zero,IntPtr.Zero);
        }
    }
    public static void Restore(IntPtr focus,uint process,Point cursor) {
        Move(cursor.X,cursor.Y);
        if(focus!=IntPtr.Zero && IsWindow(focus) && WindowProcessId(focus)==process) FocusWindow(focus);
    }
}
'@
}
function TomlString([string] $Value) { return '"' + $Value.Replace('\', '\\').Replace('"', '\"').Replace("`r", '\r').Replace("`n", '\n') + '"' }
function Check([bool] $Condition, [string] $Message) { if (!$Condition) { throw $Message }; $script:menuChecks++ }
function WaitCondition([scriptblock] $Condition, [string] $Description, [int] $Milliseconds = 10000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do { if (& $Condition) { return }; Start-Sleep -Milliseconds 25 } while ($watch.ElapsedMilliseconds -lt $Milliseconds)
    throw "Timed out: $Description"
}
function ReadTrace([string] $Path = $script:menuTrace) {
    if (![IO.File]::Exists($Path)) { return @() }
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8)
    try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
    return @($text.Split("`n") | Where-Object { $_.EndsWith('}') } | ForEach-Object { $_ | ConvertFrom-Json })
}
function StageCount([string] $Stage) { return @(ReadTrace | Where-Object { $_.stage -eq $Stage }).Count }
function CaptureCount { return @(ReadTrace | Where-Object { $_.stage -eq 'action-captured' }).Count }
function StageTotal([string] $Stage) {
    $count = 0
    foreach ($path in $script:menuTracePaths) { $count += @(ReadTrace $path | Where-Object { $_.stage -eq $Stage }).Count }
    return $count
}
function Control([int] $Command, [int] $Index = 0) {
    $window = [LauncherVersionMenuNative]::Window($script:menuPid)
    if ($window -eq [IntPtr]::Zero) { throw 'The owned scenario window is unavailable.' }
    $word = [UIntPtr]::new([uint64](($Index -shl 16) -bor $Command))
    if (![LauncherVersionMenuNative]::PostMessage($window, [uint32]$script:menuManifest.control_message, $word, [IntPtr]::new([long]$script:menuPid))) { throw 'The owned control message failed.' }
}
function Popups { return @([LauncherVersionMenuNative]::Popups($script:menuPid)) }
function Rows([IntPtr] $Menu) { return @([LauncherVersionMenuNative]::Rows($Menu, $script:menuPid)) }
function FindPopup([uint32] $Command) {
    foreach ($popup in @(Popups)) { if (@(Rows $popup.Menu | Where-Object { $_.Id -eq $Command }).Count) { return $popup } }
    return $null
}
function PopupFor([IntPtr] $Menu) { return @(Popups | Where-Object { $_.Menu -eq $Menu }) | Select-Object -First 1 }
function Row([IntPtr] $Menu, [uint32] $Command) {
    $found = @(Rows $Menu | Where-Object { $_.Id -eq $Command })
    if ($found.Count -ne 1) { throw "Native menu did not contain exactly one command 0x$($Command.ToString('x'))." }
    return $found[0]
}
function Snapshot([string] $Stage, [IntPtr] $Menu) {
    $snapshot = [pscustomobject]@{ Stage = $Stage; Menu = $Menu.ToInt64(); Rows = @(Rows $Menu | ForEach-Object {
        [pscustomobject]@{ Position = $_.Position; Id = $_.Id; State = $_.State; Text = $_.Text; Submenu = $_.Submenu.ToInt64();
            Left = $_.Bounds.Left; Top = $_.Bounds.Top; Right = $_.Bounds.Right; Bottom = $_.Bounds.Bottom }
    }) }
    $script:menuSnapshots.Add($snapshot)
    if ($CaptureImages) {
        $popup = PopupFor $Menu
        $bounds = [LauncherVersionMenuNative+Rect]::new()
        if ($popup -and [LauncherVersionMenuNative]::GetWindowRect($popup.Window, [ref] $bounds)) {
            $image = [Drawing.Bitmap]::new($bounds.Right - $bounds.Left, $bounds.Bottom - $bounds.Top)
            $graphics = [Drawing.Graphics]::FromImage($image)
            try {
                $dc = $graphics.GetHdc()
                try { $printed = [LauncherVersionMenuNative]::PrintWindow($popup.Window, $dc, 0) }
                finally { $graphics.ReleaseHdc($dc) }
                if (!$printed) { throw 'Could not capture the owned popup menu.' }
                $image.Save((Join-Path $menuRun ('{0:D3}-{1}.png' -f $script:menuSnapshots.Count, $Stage)), [Drawing.Imaging.ImageFormat]::Png)
            } finally { $graphics.Dispose(); $image.Dispose() }
        }
    }
}
function NoMenuCapture {
    WaitCondition {
        if (@(Popups).Count) { return $false }
        $info = [LauncherVersionMenuNative]::Gui($script:menuPid)
        return $info.Capture -eq [IntPtr]::Zero -and ($info.Flags -band 0x14) -eq 0
    } 'native menu chain and mouse capture released'
}
function SelectRow([IntPtr] $Menu, [int] $Position) {
    $rows = @(Rows $Menu)
    # GetMenuState overlays a popup's child count in its high byte; test
    # separator flags only for ordinary rows, not submenu parents.
    $selectable = @($rows | Where-Object { ($_.State -band 3) -eq 0 -and ($_.Submenu -ne [IntPtr]::Zero -or ($_.State -band 0x800) -eq 0) })
    $index = -1
    for ($rowIndex = 0; $rowIndex -lt $selectable.Count; ++$rowIndex) { if ($selectable[$rowIndex].Position -eq $Position) { $index = $rowIndex; break } }
    if ($index -lt 0) { throw 'The requested native menu row is disabled or structural.' }
    try {
        if (![LauncherVersionMenuNative]::Hover($script:menuPid, $Menu, $Position)) {
            $fromEnd = ($selectable.Count - 1 - $index) -lt $index
            $current = if ($fromEnd) { $selectable.Count - 1 } else { 0 }
            $edgeKey = if ($fromEnd) { 0x23 } else { 0x24 }
            $stepKey = if ($fromEnd) { 0x26 } else { 0x28 }
            [LauncherVersionMenuNative]::Key($script:menuPid, $edgeKey)
            WaitCondition { (@(Rows $Menu | Where-Object { $_.Position -eq $selectable[$current].Position })[0].State -band 0x80) -ne 0 } 'native menu edge highlighted'
            while ($current -ne $index) {
                $current += $(if ($fromEnd) { -1 } else { 1 })
                [LauncherVersionMenuNative]::Key($script:menuPid, $stepKey)
                WaitCondition { (@(Rows $Menu | Where-Object { $_.Position -eq $selectable[$current].Position })[0].State -band 0x80) -ne 0 } 'native menu keyboard step highlighted'
            }
        }
        WaitCondition { (@(Rows $Menu | Where-Object { $_.Position -eq $Position })[0].State -band 0x80) -ne 0 } "native menu row $Position highlighted"
    }
    catch { Snapshot ("failed-highlight-" + $Position) $Menu; throw }
}
function OpenRoot {
    $deadline = [DateTime]::UtcNow.AddSeconds(12)
    do {
        [LauncherVersionMenuNative]::FocusFixture($script:menuPid)
        Control 7 0
        WaitCondition { $null -ne (FindPopup 10) } 'owned real root menu'
        $root = FindPopup 10
        if (@(Rows $root.Menu | Where-Object { $_.Id -eq 0x800 }).Count) {
            $open = Row $root.Menu 10
            Check ((($open.State -band 0x1000) -ne 0) -eq (($open.State -band 3) -eq 0)) 'The default Open marker does not match its availability.'
            Snapshot 'root' $root.Menu
            return $root
        }
        [LauncherVersionMenuNative]::Key($script:menuPid, 0x1b)
        NoMenuCapture
        $checkpoint = StageCount 'ui-checkpoint'
        Control 5
        WaitCondition { (StageCount 'ui-checkpoint') -gt $checkpoint } 'presentation adoption before reopening the menu'
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'The real root menu never adopted its complete version catalog.'
}
function OpenChild([IntPtr] $Parent, [uint32] $Command, [string] $InputKind = 'Mouse') {
    $row = Row $Parent $Command
    SelectRow $Parent $row.Position
    $opened = StageCount 'child-menu-opened'
    if ($InputKind -eq 'Mouse') { [LauncherVersionMenuNative]::Mouse($script:menuPid, $Parent, $row.Position, $true) }
    else { [LauncherVersionMenuNative]::Context($script:menuPid, ($InputKind -eq 'ShiftF10')) }
    WaitCondition { $null -ne (FindPopup 0x9000) } "version child opened through $InputKind"
    WaitCondition { (StageCount 'child-menu-opened') -gt $opened } 'actual child popup trace'
    $child = FindPopup 0x9000
    Check (((Row $child.Menu 0x9000).State -band 0x1000) -ne 0) 'Open is not the default version action.'
    Snapshot ("child-" + $InputKind) $child.Menu
    return $child
}
function CancelChild([IntPtr] $Parent) {
    $canceled = StageCount 'child-menu-canceled'
    [LauncherVersionMenuNative]::Key($script:menuPid, 0x1b)
    WaitCondition { $null -eq (FindPopup 0x9000) -and $null -ne (PopupFor $Parent) } 'child cancel retains parent menu'
    WaitCondition { (StageCount 'child-menu-canceled') -gt $canceled } 'canceled child trace'
}
function OpenShell([IntPtr] $Parent, [string] $Stage) {
    $row = @(Rows $Parent | Where-Object { $_.Text -eq 'Windows commands' -and $_.Submenu -ne [IntPtr]::Zero })
    Check ($row.Count -eq 1) 'Windows commands submenu is missing or ambiguous.'
    $before = StageCount $Stage
    SelectRow $Parent $row[0].Position
    [LauncherVersionMenuNative]::Key($script:menuPid, 0x27)
    WaitCondition { $null -ne (PopupFor $row[0].Submenu) } 'real Windows commands popup'
    WaitCondition { (StageCount $Stage) -gt $before } 'Windows commands populated in the correct menu frame'
    Snapshot $Stage $row[0].Submenu
    if ($CaptureImages) { Snapshot ($Stage + '-parent-selected') $Parent }
    [LauncherVersionMenuNative]::Key($script:menuPid, 0x1b)
    WaitCondition { $null -eq (PopupFor $row[0].Submenu) -and $null -ne (PopupFor $Parent) } 'Windows submenu cancel returns to its parent'
    if ($CaptureImages) {
        Snapshot ($Stage + '-parent-closed') $Parent
        SelectRow $Parent 0
        Snapshot ($Stage + '-parent-unselected') $Parent
    }
}
function CheckCapture([int] $Before, [string] $Verb, [bool] $Reveal, [string] $Expected, [int] $ClosedBefore, [bool] $Properties = $false) {
    WaitCondition { (CaptureCount) -eq ($Before + 1) } 'one deferred exact-file dispatch receipt'
    NoMenuCapture
    $trace = @(ReadTrace)
    $captures = @($trace | Where-Object { $_.stage -eq 'action-captured' })
    Check ($captures.Count -eq ($Before + 1)) 'A native menu choice dispatched more than once.'
    $capture = $captures[$Before]
    Check ($capture.target -eq $Expected -and $capture.verb -eq $Verb -and [bool]$capture.reveal -eq $Reveal) 'The native menu dispatched the wrong file or operation.'
    if ($Properties) {
        Check ($capture.arguments -eq '' -and $capture.working_directory -eq '' -and $capture.show -eq 0) 'Properties received configured program arguments, directory, or show mode.'
    } else {
        Check ($capture.arguments -eq $script:menuArguments -and [IO.Path]::GetFullPath($capture.working_directory) -eq $script:menuWorking -and $capture.show -eq 2) 'The older version lost configured launch options.'
    }
    $closed = 0; $seen = 0; $ordered = $false
    foreach ($entry in $trace) {
        if ($entry.stage -eq 'menu-chain-closed') { $closed++ }
        if ($entry.stage -eq 'action-captured') { if ($seen -eq $Before) { $ordered = $closed -gt $ClosedBefore; break }; $seen++ }
    }
    Check $ordered 'The configured action was captured before the entire native menu chain closed.'
}
function ChooseChild($Child, [uint32] $Command, [string] $Verb, [bool] $Reveal, [string] $Expected, [bool] $Properties = $false) {
    $before = CaptureCount
    $closed = StageCount 'menu-chain-closed'
    $row = Row $Child.Menu $Command
    Check (($row.State -band 3) -eq 0) 'The requested child operation is disabled.'
    [LauncherVersionMenuNative]::Mouse($script:menuPid, $Child.Menu, $row.Position, $false)
    CheckCapture $before $Verb $Reveal $Expected $closed $Properties
}
function UiCheckpoint {
    $before = StageCount 'ui-checkpoint'
    Control 5
    WaitCondition { (StageCount 'ui-checkpoint') -gt $before } 'responsive preference UI checkpoint'
}
function CheckNoDispatch([int] $Before) {
    UiCheckpoint
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        Check ((CaptureCount) -eq $Before) 'Changing a version preference or activating an unavailable pin dispatched an action.'
        Start-Sleep -Milliseconds 25
    } while ($watch.ElapsedMilliseconds -lt 300)
    Check ([LauncherVersionMenuNative]::Barrier($script:menuPid)) 'The preference operation left the fixture unresponsive.'
}
function ChoosePreference([IntPtr] $Menu, [uint32] $Command) {
    $before = CaptureCount
    $closed = StageCount 'menu-chain-closed'
    $row = Row $Menu $Command
    Check (($row.State -band 3) -eq 0) 'The requested version preference command is disabled.'
    [LauncherVersionMenuNative]::Mouse($script:menuPid, $Menu, $row.Position, $false)
    NoMenuCapture
    WaitCondition { (StageCount 'menu-chain-closed') -gt $closed } 'preference choice closed the entire native menu chain'
    CheckNoDispatch $before
}
function CloseRoot {
    [LauncherVersionMenuNative]::Key($script:menuPid, 0x1b)
    NoMenuCapture
}
function OpenPreferenceRoot([uint32] $Selected, [bool] $Automatic, [bool] $Unavailable = $false) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $root = OpenRoot
        $rows = @(Rows $root.Menu)
        $checked = @($rows | Where-Object { $_.Id -ge 0x800 -and $_.Id -lt 0x1000 -and ($_.State -band 8) -ne 0 })
        $latest = Row $root.Menu 13
        $matches = (($latest.State -band 8) -ne 0) -eq $Automatic
        if ($Selected) { $matches = $matches -and $checked.Count -eq 1 -and $checked[0].Id -eq $Selected }
        else { $matches = $matches -and $checked.Count -eq 0 }
        if ($Unavailable) {
            $description = @($rows | Where-Object { $_.Text.StartsWith('Active version unavailable') -and ($_.State -band 3) -ne 0 })
            $matches = $matches -and $description.Count -eq 1 -and ((Row $root.Menu 10).State -band 3) -ne 0
        }
        if ($matches) { Snapshot 'preference-root' $root.Menu; return $root }
        CloseRoot
        UiCheckpoint
    } while ($watch.ElapsedMilliseconds -lt 12000)
    throw 'The native menu never adopted the expected active-version preference.'
}
function ActivateExpected([string] $Expected) {
    $before = CaptureCount
    Control 6 0
    WaitCondition { (CaptureCount) -eq ($before + 1) } 'one captured ordinary icon activation'
    UiCheckpoint
    $captures = @(ReadTrace | Where-Object { $_.stage -eq 'action-captured' })
    Check ($captures.Count -eq ($before + 1)) 'Ordinary icon activation dispatched more than once.'
    $capture = $captures[$before]
    Check ($capture.target -eq $Expected -and $capture.verb -eq '' -and ![bool]$capture.reveal) 'Ordinary icon activation did not use the effective default version.'
    Check ($capture.arguments -eq $script:menuArguments -and [IO.Path]::GetFullPath($capture.working_directory) -eq $script:menuWorking -and $capture.show -eq 2) 'Ordinary icon activation lost configured launch options.'
}
function RestartFixture([Diagnostics.Process] $Previous, [string] $Run) {
    $previousPid = $script:menuPid
    Control 3
    Check ($Previous.WaitForExit(15000)) 'The preference fixture failed to restart normally.'
    $Previous.Refresh()
    Check ($Previous.ExitCode -eq 0) 'The preference fixture failed while restarting.'
    Check ((StageCount 'owners-cleaned') -eq 1) 'The preference restart omitted normal owner cleanup.'
    $findReplacement = {
        return @(Get-ChildItem -LiteralPath (Join-Path $Run 'evidence') -Filter manifest.json -Recurse -File | Where-Object {
            $candidate = [IO.File]::ReadAllText($_.FullName) | ConvertFrom-Json
            @($script:menuProcesses | Where-Object { $_.Id -eq [int]$candidate.pid }).Count -eq 0
        })
    }
    WaitCondition { @(& $findReplacement).Count -eq 1 } 'one replacement preference fixture manifest'
    $manifestFile = @(& $findReplacement)[0]
    $manifest = [IO.File]::ReadAllText($manifestFile.FullName) | ConvertFrom-Json
    $replacement = [Diagnostics.Process]::GetProcessById([int]$manifest.pid)
    [void]$replacement.Handle
    if ([IO.Path]::GetFullPath([LauncherVersionMenuNative]::ProcessImage([uint32]$replacement.Id)) -ne $menuLauncher) {
        $replacement.Dispose()
        throw 'The replacement preference PID does not run the instrumented fixture executable.'
    }
    $script:menuProcesses.Add($replacement)
    $script:menuPid = [uint32]$replacement.Id
    Check ($script:menuPid -ne $previousPid) 'Restart reused the preference fixture process.'
    $script:menuManifest = $manifest
    $script:menuTrace = Join-Path $manifestFile.DirectoryName 'trace.jsonl'
    $script:menuTracePaths.Add($script:menuTrace)
    WaitCondition { @(ReadTrace | Where-Object { $_.stage -eq 'finished' -and $_.search -eq 'versions' }).Count -eq 1 } 'replacement version discovery'
    return $replacement
}

$menuRoot = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
[void][IO.Directory]::CreateDirectory($menuRoot)
[LauncherVersionMenuNative]::DpiAware()
$savedFocus = [IntPtr]([LauncherVersionMenuNative]::GetForegroundWindow())
$savedFocusPid = [uint32]([LauncherVersionMenuNative]::WindowProcessId($savedFocus))
$savedCursor = [LauncherVersionMenuNative+Point]::new()
if (![LauncherVersionMenuNative]::GetCursorPos([ref]$savedCursor)) { throw 'Cannot preserve the pointer position.' }
$menuCases = if ($Mode -eq 'All') { @('Basic', 'Paged', 'Default') + $(if ($Probe) { @('Admin') } else { @() }) } else { @($Mode) }
$menuResults = [Collections.Generic.List[object]]::new()
try {
    foreach ($menuCase in $menuCases) {
        $script:menuChecks = 0
        $script:menuSnapshots = [Collections.Generic.List[object]]::new()
        $menuRun = Join-Path $menuRoot ($menuCase.ToLowerInvariant() + '-' + [Guid]::NewGuid().ToString('N'))
        if (!$menuRun.StartsWith($menuRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Menu fixture escaped OutputDirectory.' }
        [void][IO.Directory]::CreateDirectory($menuRun)
        [IO.File]::WriteAllText((Join-Path $menuRun 'LauncherVersionScenario.fixture'), "Launcher version scenario fixture v1`n")
        $unicode = [string][char]0x8D44 + [char]0x6599
        $script:menuWorking = Join-Path $menuRun ('Original working directory ' + $unicode)
        [void][IO.Directory]::CreateDirectory($script:menuWorking)
        $script:menuArguments = '--owned-menu-capture "' + $unicode + ' & literal" %UNCHANGED%'
        $count = if ($menuCase -eq 'Paged') { 65 } else { 2 }
        $filename = if ($menuCase -eq 'Admin') { 'Selected.exe' } else { 'Selected.txt' }
        for ($version = 1; $version -le $count; ++$version) {
            $directory = Join-Path $menuRun ('Version {0:d3}' -f $version)
            [void][IO.Directory]::CreateDirectory($directory)
            $target = Join-Path $directory $filename
            if ($menuCase -eq 'Admin') { [IO.File]::Copy($Probe, $target) }
            else { [IO.File]::WriteAllText($target, 'Owned harmless native menu version fixture. Never execute configured targets.') }
        }
        $pattern = Join-Path $menuRun ("Version *\" + $filename)
        $config = Join-Path $menuRun 'fixture.toml'
        [IO.File]::WriteAllText($config, @"
schema_version = 1
[dock]
edge = 'top'
display = 'primary'
auto_hide = false
suppress_fullscreen = false
[[tabs]]
name = 'Owned native version menus'
items = [ { target = $(TomlString $pattern), label = 'Owned menu icon', arguments = $(TomlString $script:menuArguments), working_directory = $(TomlString $script:menuWorking), show = 'minimized', icon = { stock = 'document' }, context_verbs = [ { label = 'Configured inspect', verb = 'inspect-owned-fixture' } ] } ]
"@, [Text.UTF8Encoding]::new($false))
        $scenario = Join-Path $menuRun 'scenario.toml'
        [IO.File]::WriteAllText($scenario, @"
schema_version = 1
instance = '$([Guid]::NewGuid().ToString('N'))'
configuration = $(TomlString $config)
output_directory = $(TomlString (Join-Path $menuRun 'evidence'))
dispatch = 'capture-only'
[[searches]]
id = 'versions'
path = $(TomlString $pattern)
attempts = [ {}, {}, {} ]
"@, [Text.UTF8Encoding]::new($false))
        $process = $null
        $script:menuProcesses = [Collections.Generic.List[Diagnostics.Process]]::new()
        $script:menuTracePaths = [Collections.Generic.List[string]]::new()
        $script:menuManifest = $null
        $script:menuTrace = Join-Path $menuRun 'not-started.trace'
        $result = [ordered]@{ Passed = $false; Case = $menuCase; Checks = 0; Evidence = $menuRun; CaptureOnly = $true;
            NativePopupInput = $true; RequestedElevation = $false; ConfiguredProgramsExecuted = $false; Captures = 0; ParentPid = 0;
            InvalidHeadingForwarded = $null; FrozenPathRace = $false; DefaultSelection = $false; RestartPersistence = $false;
            MissingPinRetained = $false; AutomaticLatestRestored = $false; ProcessIds = @(); Error = $null; FailurePosition = $null }
        try {
            $process = Start-Process -FilePath $menuLauncher -ArgumentList @('--version-scenario', ('"' + $scenario + '"'), '--config', ('"' + $config + '"')) -WorkingDirectory $menuRun -WindowStyle Hidden -PassThru
            $script:menuProcesses.Add($process)
            $script:menuPid = [uint32]$process.Id
            $result.ParentPid = $script:menuPid
            $manifestPath = Join-Path $menuRun ('evidence\process-' + $script:menuPid + '\manifest.json')
            $script:menuTrace = Join-Path (Split-Path -Parent $manifestPath) 'trace.jsonl'
            $script:menuTracePaths.Add($script:menuTrace)
            WaitCondition { [IO.File]::Exists($manifestPath) -or $process.HasExited } 'instrumented menu manifest'
            if ($process.HasExited) { throw "The menu fixture exited before startup ($($process.ExitCode))." }
            Check ([IO.Path]::GetFullPath([LauncherVersionMenuNative]::ProcessImage($script:menuPid)) -eq $menuLauncher) 'The fixture PID does not run the specified instrumented executable.'
            $script:menuManifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
            Check ([uint32]$script:menuManifest.pid -eq $script:menuPid) 'The native menu manifest belongs to another process.'
            WaitCondition { @(ReadTrace | Where-Object { $_.stage -eq 'finished' -and $_.search -eq 'versions' }).Count -eq 1 } 'complete owned version discovery'
            $root = OpenRoot
            $older = Join-Path $menuRun ('Version 001\' + $filename)
            if ($menuCase -eq 'Paged') {
                $more = @(Rows $root.Menu | Where-Object { $_.Text -eq 'More versions' })
                Check ($more.Count -eq 1 -and $more[0].Submenu -ne [IntPtr]::Zero) 'A 65-entry snapshot lost its More versions submenu.'
                SelectRow $root.Menu $more[0].Position
                [LauncherVersionMenuNative]::Key($script:menuPid, 0x27)
                WaitCondition { $null -ne (PopupFor $more[0].Submenu) } 'More versions hierarchy'
                Snapshot 'more-versions' $more[0].Submenu
                $page = @(Rows $more[0].Submenu | Where-Object { $_.Submenu -ne [IntPtr]::Zero })
                Check ($page.Count -eq 1) 'The 51-65 version page is missing or duplicated.'
                SelectRow $more[0].Submenu $page[0].Position
                [LauncherVersionMenuNative]::Key($script:menuPid, 0x27)
                WaitCondition { $null -ne (PopupFor $page[0].Submenu) -and @(Rows $page[0].Submenu | Where-Object { $_.Id -eq 0x840 }).Count -eq 1 } 'last accepted snapshot entry on a populated version page'
                Snapshot 'versions-51-65' $page[0].Submenu
                $child = OpenChild $page[0].Submenu 0x840 'Apps'
                ChooseChild $child 0x9003 'properties' $false $older $true
            } elseif ($menuCase -eq 'Admin') {
                $child = OpenChild $root.Menu 0x801 'ShiftF10'
                ChooseChild $child 0x9001 'runas' $false $older
            } elseif ($menuCase -eq 'Default') {
                $runtime = Join-Path $menuRun 'fixture.runtime.toml'
                $newer = Join-Path $menuRun ('Version 002\' + $filename)
                Check (((Row $root.Menu 13).State -band 8) -ne 0) 'Always use latest is not initially checked.'
                Check (((Row $root.Menu 0x800).State -band 8) -ne 0) 'The newest version is not initially active.'
                Check ((Row $root.Menu 0x800).Text.Contains('Latest')) 'The newest version lacks its independent Latest label.'
                $child = OpenChild $root.Menu 0x801 'Apps'
                Check (((Row $child.Menu 0x9004).State -band 8) -eq 0) 'An unpinned older version claims to be the saved default.'
                ChoosePreference $child.Menu 0x9004
                Check ([IO.File]::Exists($runtime) -and [IO.File]::ReadAllText($runtime).Contains('Version 001')) 'Set as default did not persist the exact older installation.'
                $root = OpenPreferenceRoot 0x801 $false
                Check ((Row $root.Menu 0x800).Text.Contains('Latest')) 'Choosing an older default moved the Latest label away from the newest version.'
                $child = OpenChild $root.Menu 0x801 'ShiftF10'
                Check (((Row $child.Menu 0x9004).State -band 8) -ne 0) 'Set as default is not checked for the explicitly pinned version.'
                CancelChild $root.Menu
                CloseRoot
                ActivateExpected $older
                $result.DefaultSelection = $true

                # Exercise the application's real staged Restart command, with
                # the same isolated configuration and capture-only scenario.
                $process = RestartFixture $process $menuRun
                $root = OpenPreferenceRoot 0x801 $false
                CloseRoot
                ActivateExpected $older
                $result.RestartPersistence = $true

                # A new process must discover the removed pin as unavailable.
                # Its remaining installed version must not become the default.
                [IO.File]::Delete($older)
                $process = RestartFixture $process $menuRun
                $root = OpenPreferenceRoot 0 $false $true
                Check ([IO.File]::ReadAllText($runtime).Contains('Version 001')) 'A missing installation erased its saved default.'
                Check ((Row $root.Menu 0x800).Text.Contains('Latest')) 'The remaining installed version lost its Latest label.'
                CloseRoot
                $before = CaptureCount
                Control 6 0
                CheckNoDispatch $before
                $result.MissingPinRetained = $true

                $root = OpenPreferenceRoot 0 $false $true
                ChoosePreference $root.Menu 13
                Check ([IO.File]::ReadAllText($runtime) -notmatch '(?m)^\[\[active_version\]\]\s*$') 'Always use latest did not clear the saved pin.'
                $root = OpenPreferenceRoot 0x800 $true
                CloseRoot
                ActivateExpected $newer
                $process = RestartFixture $process $menuRun
                $root = OpenPreferenceRoot 0x800 $true
                CloseRoot
                ActivateExpected $newer
                $result.AutomaticLatestRestored = $true
            } else {
                $heading = @(Rows $root.Menu | Where-Object { $_.Id -eq 0 -and ($_.State -band 3) -ne 0 -and ($_.State -band 0x800) -eq 0 }) | Select-Object -First 1
                Check ($null -ne $heading) 'The disabled Installed versions heading was not present.'
                $childBefore = StageCount 'child-menu-opened'
                $captureBefore = CaptureCount
                $invalidBefore = StageCount 'invalid-version-row'
                [LauncherVersionMenuNative]::Mouse($script:menuPid, $root.Menu, $heading.Position, $true)
                # Windows can suppress owner notifications for disabled rows.
                # Observe the actual input without fabricating that message:
                # either its trace arrives, or the responsive native parent
                # remains unchanged throughout a bounded negative observation.
                $headingWatch = [Diagnostics.Stopwatch]::StartNew()
                do { if ((StageCount 'invalid-version-row') -gt $invalidBefore) { break }; Start-Sleep -Milliseconds 25 } while ($headingWatch.ElapsedMilliseconds -lt 500)
                $result.InvalidHeadingForwarded = (StageCount 'invalid-version-row') -gt $invalidBefore
                Check ([LauncherVersionMenuNative]::Barrier($script:menuPid) -and $null -ne (PopupFor $root.Menu)) 'Right-clicking a heading dismissed or blocked the parent menu.'
                Check ((StageCount 'child-menu-opened') -eq $childBefore -and (CaptureCount) -eq $captureBefore) 'A structural menu heading opened a version child or dispatched an action.'
                OpenShell $root.Menu 'root-shell-opened'
                $child = OpenChild $root.Menu 0x801 'Mouse'
                Check (((Row $child.Menu 0x9001).State -band 3) -ne 0) 'A document version unexpectedly enabled elevation.'
                OpenShell $child.Menu 'child-shell-opened'
                CancelChild $root.Menu
                Check ((CaptureCount) -eq $captureBefore) 'Canceling the version child captured an action.'
                $child = OpenChild $root.Menu 0x801 'ShiftF10'
                CancelChild $root.Menu
                $child = OpenChild $root.Menu 0x801 'Apps'
                # Change only this owned fixture while the actual child popup
                # is tracking. Its accepted row must retain its original path
                # even after the selected file vanishes and a newer one exists.
                $addedDirectory = Join-Path $menuRun 'Version 003'
                $addedTarget = Join-Path $addedDirectory $filename
                $olderContent = [IO.File]::ReadAllText($older)
                [void][IO.Directory]::CreateDirectory($addedDirectory)
                [IO.File]::WriteAllText($addedTarget, 'Owned newer version added while the older row menu is open.')
                [IO.File]::Delete($older)
                try {
                    Check (![IO.File]::Exists($older) -and [IO.File]::Exists($addedTarget)) 'The owned file race was not established while the child menu was open.'
                    Check ($null -ne (FindPopup 0x9000)) 'The owned file mutation dismissed the tracking child popup.'
                    ChooseChild $child 0x9000 '' $false $older
                    $result.FrozenPathRace = $true
                } finally {
                    [IO.File]::WriteAllText($older, $olderContent)
                    [IO.File]::Delete($addedTarget)
                    [IO.Directory]::Delete($addedDirectory)
                }
                $root = OpenRoot
                $child = OpenChild $root.Menu 0x801 'Apps'
                ChooseChild $child 0x9003 'properties' $false $older $true
                $root = OpenRoot
                $child = OpenChild $root.Menu 0x801 'Mouse'
                ChooseChild $child 0x9002 '' $true $older
                $root = OpenRoot
                $child = OpenChild $root.Menu 0x801 'ShiftF10'
                ChooseChild $child 0x9100 'inspect-owned-fixture' $false $older
            }
            NoMenuCapture
            Check ((StageTotal 'keyboard-context-opened') -ge 1) 'The real WH_MSGFILTER keyboard context path was not observed.'
            Control 1
            Check ($process.WaitForExit(5000)) 'The owned menu fixture failed to close normally.'
            $process.Refresh()
            Check ($process.ExitCode -eq 0) 'The owned menu fixture reported a failure while closing.'
            $result.Passed = $true
            $result.Checks = $script:menuChecks
            $result.Captures = StageTotal 'action-captured'
            $result.ProcessIds = @($script:menuProcesses | ForEach-Object { $_.Id })
        } catch {
            $result.Error = $_.Exception.Message
            $result.FailurePosition = $_.InvocationInfo.PositionMessage
            $result.Checks = $script:menuChecks
            $result.Captures = StageTotal 'action-captured'
            $result.ProcessIds = @($script:menuProcesses | ForEach-Object { $_.Id })
            throw
        } finally {
            $script:menuSnapshots | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $menuRun 'menus.json') -Encoding utf8
            $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $menuRun 'results.json') -Encoding utf8
            # A replacement can start before a later restart assertion fails.
            # Admit only manifests within this fixture and verify the executable
            # before including any additional PID in cleanup.
            $evidenceRoot = Join-Path $menuRun 'evidence'
            if ([IO.Directory]::Exists($evidenceRoot)) {
                foreach ($manifestFile in @(Get-ChildItem -LiteralPath $evidenceRoot -Filter manifest.json -Recurse -File)) {
                    try {
                        $manifest = [IO.File]::ReadAllText($manifestFile.FullName) | ConvertFrom-Json
                        if (@($script:menuProcesses | Where-Object { $_.Id -eq [int]$manifest.pid }).Count) { continue }
                        $additional = [Diagnostics.Process]::GetProcessById([int]$manifest.pid)
                        if ([IO.Path]::GetFullPath([LauncherVersionMenuNative]::ProcessImage([uint32]$additional.Id)) -eq $menuLauncher) {
                            [void]$additional.Handle
                            $script:menuProcesses.Add($additional)
                        } else { $additional.Dispose() }
                    } catch { }
                }
            }
            foreach ($ownedProcess in $script:menuProcesses) {
                try {
                    if (!$ownedProcess.HasExited) {
                        try { [LauncherVersionMenuNative]::Cancel([uint32]$ownedProcess.Id) } catch { }
                        if (!$ownedProcess.WaitForExit(3000)) { $ownedProcess.Kill(); [void]$ownedProcess.WaitForExit(3000) }
                    }
                } finally { $ownedProcess.Dispose() }
            }
        }
        $menuResults.Add([pscustomobject]$result)
        "Native version menu $menuCase passed. Evidence: $menuRun"
    }
} finally { [LauncherVersionMenuNative]::Restore($savedFocus, $savedFocusPid, $savedCursor) }
$menuResults | ConvertTo-Json -Depth 5
