using System;
using System.Drawing;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using LauncherTools;

static class SvgExtractorProbe
{
    static readonly JavaScriptSerializer Json = new JavaScriptSerializer();
    static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }
    static T Control<T>(Form form, string name) where T : Control { return (T)form.Controls.Find(name, true).Single(); }
    static string Latest(string directory)
    {
        return Directory.GetDirectories(directory).OrderByDescending(Directory.GetCreationTimeUtc).First();
    }
    static int Number(string directory, string key)
    {
        return Convert.ToInt32(Json.Deserialize<System.Collections.Generic.Dictionary<string, object>>(File.ReadAllText(Path.Combine(directory, "summary.json")))[key]);
    }
    sealed class Response { public int Status; public string Text, Policy; }
    static Response Request(string url, string method, string body, string origin)
    {
        HttpWebRequest request = (HttpWebRequest)WebRequest.Create(url);
        request.Proxy = null; request.Method = method; request.Timeout = 10000;
        if (origin != null) request.Headers["Origin"] = origin;
        if (body != null)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(body);
            request.ContentType = "application/json"; request.ContentLength = bytes.Length;
            using (Stream output = request.GetRequestStream()) output.Write(bytes, 0, bytes.Length);
        }
        HttpWebResponse response;
        try { response = (HttpWebResponse)request.GetResponse(); }
        catch (WebException ex) { if (ex.Response == null) throw; response = (HttpWebResponse)ex.Response; }
        using (response)
        using (StreamReader input = new StreamReader(response.GetResponseStream()))
            return new Response { Status = (int)response.StatusCode, Text = input.ReadToEnd(), Policy = response.Headers["Content-Security-Policy"] };
    }
    static string Hash(string path)
    {
        using (Stream input = File.OpenRead(path))
        using (SHA256 hash = SHA256.Create()) return BitConverter.ToString(hash.ComputeHash(input)).Replace("-", "");
    }
    static void CheckKeep(string results, string keepDirectory)
    {
        string[] header = null, selected = null, unicode = null;
        foreach (string[] row in Csv.Read(Path.Combine(results, "resources.csv")))
        {
            if (header == null) { header = row; continue; }
            if (row[Array.IndexOf(header, "Location")] == "PE/SVG/APP_LOGO/#1033") selected = row;
            if (row[Array.IndexOf(header, "Location")].EndsWith("/\u56fe\u6807.svg", StringComparison.Ordinal)) unicode = row;
        }
        Check(selected != null, "Native SVG missing from GUI results.");
        File.WriteAllText(Path.Combine(results, "examples.html"), "<html><script>untrusted_saved_script()</script></html>");
        string asset = selected[Array.IndexOf(header, "ExportedFile")];
        using (SvgGalleryServer server = new SvgGalleryServer(results, keepDirectory))
        {
            string origin = new Uri(server.Url).GetLeftPart(UriPartial.Authority), body = Json.Serialize(new { asset = asset });
            Response html = Request(server.Url, "GET", null, null);
            File.WriteAllText(Path.Combine(Path.GetDirectoryName(results), "gallery-with-keep.html"), html.Text, Encoding.UTF8);
            Check(html.Status == 200 && html.Text.Contains("button.textContent=remembered") && html.Policy.Contains("connect-src 'self'") && !html.Text.Contains("untrusted_saved_script"), "Gallery has no Keep action or executed saved HTML instead of rebuilding it.");
            Response image = Request(server.Url + asset, "GET", null, null);
            Check(image.Status == 200 && image.Policy.Contains("sandbox") && !image.Policy.Contains("script-src 'unsafe-inline'"), "SVG response was not isolated from scripting.");
            Check(Request(origin + "/wrong-token/keep", "POST", body, origin).Status == 403, "Wrong capability was accepted.");
            Check(Request(server.Url + "keep", "POST", body, "https://outside.invalid").Status == 403, "Other-origin Keep was accepted.");
            Check(Request(server.Url + "keep", "GET", null, null).Status != 200, "GET was allowed to keep an image.");
            Check(Request(server.Url + "keep", "POST", Json.Serialize(new { asset = "../outside.svg" }), origin).Status == 404, "Non-cataloged path was accepted.");
            Check(Request(server.Url + "keep", "POST", "{\"asset\":null}", origin).Status == 404, "Null Keep request was accepted.");
            Check(!Directory.Exists(keepDirectory), "Invalid requests wrote to the keep directory.");
            Response kept = Request(server.Url + "keep", "POST", body, origin);
            Check(kept.Status == 200, "Keep failed: " + kept.Text);
            string target = Json.Deserialize<System.Collections.Generic.Dictionary<string, string>>(kept.Text)["path"];
            Check(Hash(target) == selected[Array.IndexOf(header, "AssetSha256")].ToUpperInvariant(), "Keep changed the SVG bytes.");
            DateTime written = File.GetLastWriteTimeUtc(target);
            Check(Request(server.Url + "keep", "POST", body, origin).Status == 200 && File.GetLastWriteTimeUtc(target) == written, "Repeated Keep rewrote an identical copy.");
            Task<Response>[] repeats = Enumerable.Range(0, 8).Select(i => Task.Run(() => Request(server.Url + "keep", "POST", body, origin))).ToArray();
            Task.WaitAll(repeats);
            Check(repeats.All(t => t.Result.Status == 200) && File.GetLastWriteTimeUtc(target) == written, "Concurrent duplicate Keep was not idempotent.");
            Check(unicode != null, "Unicode SVG missing from the fixture.");
            Response unicodeKept = Request(server.Url + "keep", "POST", Json.Serialize(new { asset = unicode[Array.IndexOf(header, "ExportedFile")] }), origin);
            Check(unicodeKept.Status == 200 && Json.Deserialize<System.Collections.Generic.Dictionary<string, string>>(unicodeKept.Text)["path"].Contains("\u56fe\u6807"), "Unicode SVG Keep failed.");
            File.WriteAllText(target, "intentional existing edit");
            Check(Request(server.Url + "keep", "POST", body, origin).Status == 500 && File.ReadAllText(target) == "intentional existing edit", "Keep overwrote different existing content.");
            File.WriteAllText(Path.Combine(results, asset), "modified after export");
            Check(Request(server.Url + "keep", "POST", body, origin).Status == 400, "Keep accepted an export whose hash changed.");
        }
    }
    static void CheckMixedGallery(string results, string keepDirectory)
    {
        string[] header = null;
        string baml = null;
        foreach (string[] row in Csv.Read(Path.Combine(results, "resources.csv")))
        {
            if (header == null) { header = row; continue; }
            if (row[Array.IndexOf(header, "Kind")] == "baml") { baml = row[Array.IndexOf(header, "ExportedFile")]; break; }
        }
        Check(baml != null, "Mixed catalog fixture has no BAML export.");
        using (SvgGalleryServer server = new SvgGalleryServer(results, keepDirectory))
        {
            string origin = new Uri(server.Url).GetLeftPart(UriPartial.Authority);
            Check(Request(server.Url + baml, "GET", null, null).Status == 200, "Mixed-gallery raw downloads were lost.");
            Check(Request(server.Url + "keep", "POST", Json.Serialize(new { asset = baml }), origin).Status == 404 && !Directory.Exists(keepDirectory), "Keep copied a non-SVG resource.");
        }
    }
    delegate bool WindowVisitor(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(WindowVisitor visitor, IntPtr data);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out int process);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll", SetLastError = true)] static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr word, IntPtr data, uint flags, uint timeout, out IntPtr result);
    static IntPtr ExtractorWindow(int processId)
    {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr data) {
            int owner; GetWindowThreadProcessId(window, out owner);
            if (owner == processId)
            {
                StringBuilder title = new StringBuilder(256); GetWindowText(window, title, title.Capacity);
                if (title.ToString() == "Launcher SVG Extractor") found = window;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    static void CloseOwnedWindows(int processId)
    {
        EnumWindows(delegate(IntPtr window, IntPtr data) {
            int owner; GetWindowThreadProcessId(window, out owner);
            if (owner == processId)
            {
                IntPtr ignored;
                SendMessageTimeout(window, 0x10, IntPtr.Zero, IntPtr.Zero, 3, 2000, out ignored);
            }
            return true;
        }, IntPtr.Zero);
    }
    static void CheckPackagedExecutable(string executable, string workingDirectory)
    {
        // Exercise this exact packaged GUI image. Idle startup/close never
        // starts a scan, opens a gallery, or writes to the user's Keep folder.
        using (Process process = Process.Start(new ProcessStartInfo(Path.GetFullPath(executable)) {
            UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden,
            WorkingDirectory = workingDirectory
        }))
        {
            try
            {
                Check(process.WaitForInputIdle(10000), "The packaged SVG Extractor did not become input-idle.");
                Stopwatch elapsed = Stopwatch.StartNew();
                IntPtr window = IntPtr.Zero;
                while (elapsed.ElapsedMilliseconds < 10000 && !process.HasExited)
                {
                    window = ExtractorWindow(process.Id);
                    if (window != IntPtr.Zero) break;
                    System.Threading.Thread.Sleep(20);
                }
                Check(window != IntPtr.Zero && !process.HasExited, "The exact packaged SVG Extractor did not create its expected window.");
                IntPtr ignored;
                Check(SendMessageTimeout(window, 0x10, IntPtr.Zero, IntPtr.Zero, 3, 2000, out ignored) != IntPtr.Zero,
                    "The packaged SVG Extractor did not accept a normal close.");
                Check(process.WaitForExit(5000), "The packaged SVG Extractor did not exit after a normal close.");
                Check(process.ExitCode == 0, "The packaged SVG Extractor exited unsuccessfully.");
                Console.WriteLine("PASS: exact packaged SVG Extractor image reached its idle window and closed normally. " + executable);
            }
            finally
            {
                if (!process.HasExited)
                {
                    // Only this probe's child is eligible for cleanup. An
                    // unexpected startup error must not leave a hidden test
                    // window or keep the shipping image locked indefinitely.
                    CloseOwnedWindows(process.Id);
                    if (!process.WaitForExit(5000)) { process.Kill(); process.WaitForExit(5000); }
                }
            }
        }
    }
    [STAThread]
    public static int Main(string[] args)
    {
        try
        {
            AppContext.SetSwitch("Switch.System.IO.UseLegacyPathHandling", false);
            AppContext.SetSwitch("Switch.System.IO.BlockLongPaths", false);
            Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
            string fixtures = args[0], output = args[1]; Directory.CreateDirectory(output);
            if (args.Length > 2) CheckPackagedExecutable(args[2], output);
            string native = Path.Combine(fixtures, "Uninstalled-x64.dll");
            CheckMixedGallery(Path.Combine(Path.GetDirectoryName(fixtures), "exported"), Path.Combine(output, "mixed-keep"));
            string recursive = Path.Combine(output, "nested-source"); Directory.CreateDirectory(Path.Combine(recursive, "child"));
            File.Copy(native, Path.Combine(recursive, "child", "uninstalled.dll"));
            Exception failure = null;
            Task closing = null;
            string destination = Path.Combine(output, "results"); Directory.CreateDirectory(destination);
            using (SvgExtractorForm form = new SvgExtractorForm(native))
            {
                form.ShowInTaskbar = false; form.StartPosition = FormStartPosition.Manual; form.Location = new Point(-32000, -32000);
                form.Shown += async delegate
                {
                    try
                    {
                        Control<CheckBox>(form, "openWhenFinished").Checked = false;
                        Control<TextBox>(form, "destination").Text = destination;
                        await form.StartScan();
                        string first = Latest(destination);
                        Check(Number(first, "FilesInspected") == 1 && Number(first, "ExportedAssets") > 0, "GUI did not scan only the selected program.");
                        Check(Control<Button>(form, "start").Enabled && Control<Button>(form, "gallery").Enabled && !Control<Button>(form, "stop").Enabled, "Completed GUI controls were not restored.");
                        using (Bitmap bitmap = new Bitmap(form.ClientSize.Width, form.ClientSize.Height))
                        {
                            form.DrawToBitmap(bitmap, new Rectangle(Point.Empty, form.ClientSize));
                            bitmap.Save(Path.Combine(output, "extractor-window.png"), System.Drawing.Imaging.ImageFormat.Png);
                        }
                        CheckKeep(first, Path.Combine(output, "kept-icons"));
                        Control<TextBox>(form, "source").Text = recursive;
                        await form.StartScan();
                        Check(Number(Latest(destination), "FilesInspected") == 1 && Number(Latest(destination), "ExportedAssets") > 0, "GUI folder scan did not reach the nested program.");
                        string blank = Path.Combine(output, "no-svg.exe"); File.Copy(typeof(object).Assembly.Location, blank);
                        Control<TextBox>(form, "source").Text = blank; await form.StartScan();
                        Check(Number(Latest(destination), "ExportedAssets") == 0 && Control<Label>(form, "status").Text.StartsWith("Finished."), "No-SVG result was not explained.");
                        Control<TextBox>(form, "source").Text = Path.Combine(output, "missing.exe"); await form.StartScan();
                        Check(Control<Label>(form, "status").Text == "The scan could not finish." && Control<Button>(form, "start").Enabled, "Invalid source left the UI stuck.");
                        for (int i = 0; i < 256; ++i) File.WriteAllText(Path.Combine(recursive, "child", i + ".svg"), "<svg xmlns='http://www.w3.org/2000/svg'/>");
                        Control<TextBox>(form, "source").Text = recursive;
                        Task stopping = form.StartScan(); Control<Button>(form, "stop").PerformClick(); await stopping;
                        Check(Control<Label>(form, "status").Text.StartsWith("Stopped.") && File.ReadAllText(Path.Combine(Latest(destination), "summary.json")).Contains("interrupted"), "Stop did not finish a partial gallery.");
                        closing = form.StartScan(); form.Close();
                    }
                    catch (Exception ex) { failure = ex; form.Close(); }
                };
                Application.Run(form);
                if (failure != null) throw failure;
                Check(closing != null && closing.IsCompleted && form.IsDisposed, "Closing during a scan did not cancel and close the window.");
                Check(File.ReadAllText(Path.Combine(Latest(destination), "summary.json")).Contains("interrupted"), "Closing lost the interrupted catalog state.");
            }
            Console.WriteLine("PASS: GUI file/recursive/empty/error scans, rendered window, Stop/close cancellation, local gallery and byte-preserving Keep, repeat/conflict protection, origin/capability checks and changed-export rejection. " + output);
            return 0;
        }
        catch (Exception ex) { Console.Error.WriteLine(ex); return 1; }
    }
}
