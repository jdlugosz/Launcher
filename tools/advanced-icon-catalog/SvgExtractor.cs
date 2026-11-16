using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Threading.Tasks;
using System.Windows.Forms;

[assembly: AssemblyTitle("Launcher SVG Extractor")]
[assembly: AssemblyProduct("D\u0142ugosz Launcher SVG Extractor")]
[assembly: AssemblyCopyright("Copyright (c) 2026 John D\u0142ugosz")]
#if !LAUNCHER_BUNDLED_EXTRACTOR
[assembly: AssemblyDescription("Find and extract SVG artwork from a program or installation folder")]
[assembly: AssemblyVersion("1.0.0.0")]
[assembly: AssemblyFileVersion("1.0.0.0")]
#endif

namespace LauncherTools
{
    // Kept separate from the window so the exact UI-to-scanner settings can be
    // checked against native, managed and nested-directory fixtures.
    static class SvgExtraction
    {
        public static string KeepDirectory { get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "D\u0142ugosz", "Launcher", "Icons"); } }
        public static Options CreateOptions(string source, string destination)
        {
            source = Path.GetFullPath(source.Trim().Trim('"'));
            destination = Path.GetFullPath(destination.Trim().Trim('"'));
            if (!File.Exists(source) && !Directory.Exists(source))
                throw new IOException("Choose an existing program file or installation folder.");
            string name = Path.GetFileName(source.TrimEnd(Path.DirectorySeparatorChar));
            if (name.Length == 0) name = "drive";
            foreach (char c in Path.GetInvalidFileNameChars()) name = name.Replace(c, '-');
            if (name.Length > 48) name = name.Substring(0, 48);
            string output = Path.Combine(destination, name + "-SVG-" + DateTime.Now.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture) + "-" + Guid.NewGuid().ToString("N").Substring(0, 8));
            Options options = new Options { Output = output, Export = true, SvgOnly = true };
            options.Roots.Add(source);
            return options;
        }
    }

    sealed class SvgExtractorForm : Form
    {
        readonly TextBox source = new TextBox { Name = "source", Dock = DockStyle.Fill };
        readonly TextBox destination = new TextBox { Name = "destination", Dock = DockStyle.Fill };
        readonly Button chooseFile = new Button { Name = "chooseFile", Text = "Choose program...", AutoSize = true };
        readonly Button chooseFolder = new Button { Name = "chooseFolder", Text = "Choose folder...", AutoSize = true };
        readonly Button chooseDestination = new Button { Name = "chooseDestination", Text = "Browse...", AutoSize = true };
        readonly Button start = new Button { Name = "start", Text = "Find and extract SVGs", AutoSize = true };
        readonly Button stop = new Button { Name = "stop", Text = "Stop", AutoSize = true, Enabled = false };
        readonly Button gallery = new Button { Name = "gallery", Text = "Open gallery", AutoSize = true, Enabled = false };
        readonly Button folder = new Button { Name = "folder", Text = "Open results folder", AutoSize = true, Enabled = false };
        readonly Button saved = new Button { Name = "saved", Text = "Open saved gallery...", AutoSize = true };
        readonly CheckBox openWhenFinished = new CheckBox { Name = "openWhenFinished", Text = "Open the gallery when finished", AutoSize = true, Checked = true };
        readonly Label status = new Label { Name = "status", Text = "Choose a program or a folder to begin.", Dock = DockStyle.Fill, AutoEllipsis = true };
        readonly TextBox details = new TextBox { Name = "details", Multiline = true, ReadOnly = true, Dock = DockStyle.Fill, ScrollBars = ScrollBars.Vertical, BorderStyle = BorderStyle.FixedSingle };
        readonly ProgressBar progress = new ProgressBar { Name = "progress", Dock = DockStyle.Fill, Height = 16 };
        Catalog active;
        readonly Dictionary<string, SvgGalleryServer> galleries = new Dictionary<string, SvgGalleryServer>(StringComparer.OrdinalIgnoreCase);
        string output;
        bool closing;

        public SvgExtractorForm(string initialSource)
        {
            Text = "Launcher SVG Extractor";
            Name = "SvgExtractor";
            AutoScaleDimensions = new SizeF(96, 96);
            AutoScaleMode = AutoScaleMode.Dpi;
            ClientSize = new Size(800, 600);
            MinimumSize = new Size(640, 470);
            StartPosition = FormStartPosition.CenterScreen;
            Font = SystemFonts.MessageBoxFont;
            Icon = System.Drawing.Icon.ExtractAssociatedIcon(Assembly.GetExecutingAssembly().Location) ?? SystemIcons.Application;
            AllowDrop = true;
            source.Text = initialSource ?? "";
            destination.Text = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "Launcher SVG artwork");
            Panel viewport = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
            TableLayoutPanel layout = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, Padding = new Padding(18), ColumnCount = 1, RowCount = 12 };
            for (int i = 0; i < 12; ++i) layout.RowStyles.Add(new RowStyle(i == 9 ? SizeType.Absolute : SizeType.AutoSize, i == 9 ? 100 : 0));
            layout.Controls.Add(new Label { Text = "Find SVG artwork in a program or its installation folder.", AutoSize = true, Margin = new Padding(3, 0, 3, 12) });
            layout.Controls.Add(new Label { Text = "Program file or folder (folders include all subfolders):", AutoSize = true });
            layout.Controls.Add(source);
            layout.Controls.Add(Buttons(chooseFile, chooseFolder));
            layout.Controls.Add(new Label { Text = "Save results in (a new subfolder is created for each scan):", AutoSize = true, Margin = new Padding(3, 8, 3, 3) });
            TableLayoutPanel outputRow = new TableLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 2, Margin = new Padding(0) };
            outputRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100)); outputRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            outputRow.Controls.Add(destination); outputRow.Controls.Add(chooseDestination); layout.Controls.Add(outputRow);
            layout.Controls.Add(openWhenFinished);
            layout.Controls.Add(Buttons(start, stop, gallery, folder, saved));
            layout.Controls.Add(status);
            layout.Controls.Add(details);
            layout.Controls.Add(progress);
            layout.Controls.Add(new Label { Text = "The program being scanned is read, never run. Only SVG artwork is copied.\nUse a saved SVG filename in a Launcher item's icon field.", AutoSize = true, Margin = new Padding(3, 8, 3, 0) });
            status.Height = 38;
            details.MinimumSize = new Size(0, 60);
            viewport.Controls.Add(layout); Controls.Add(viewport);
            AcceptButton = start;
            chooseFile.Click += delegate { ChooseFile(); };
            chooseFolder.Click += delegate { ChooseSourceFolder(); };
            chooseDestination.Click += delegate { ChooseDestination(); };
            start.Click += async delegate { await StartScan(); };
            stop.Click += delegate { StopScan(); };
            gallery.Click += delegate { OpenGallery(); };
            folder.Click += delegate { OpenResult(output); };
            saved.Click += delegate { OpenSavedGallery(); };
            FormClosing += delegate(object sender, FormClosingEventArgs e) { if (active != null) { closing = true; e.Cancel = true; StopScan(); } };
            DragEnter += delegate(object sender, DragEventArgs e) { e.Effect = active == null && e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy : DragDropEffects.None; };
            DragDrop += delegate(object sender, DragEventArgs e) { string[] paths = e.Data.GetData(DataFormats.FileDrop) as string[]; if (active == null && paths != null && paths.Length != 0) source.Text = paths[0]; };
        }
        protected override void Dispose(bool disposing)
        {
            if (disposing) foreach (SvgGalleryServer server in galleries.Values) server.Dispose();
            base.Dispose(disposing);
        }
        static FlowLayoutPanel Buttons(params Control[] controls)
        {
            FlowLayoutPanel row = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, Margin = new Padding(0, 4, 0, 4) };
            row.Controls.AddRange(controls);
            return row;
        }
        void ChooseFile()
        {
            using (OpenFileDialog dialog = new OpenFileDialog { Title = "Choose the program or library to check", Filter = "Programs and libraries|*.exe;*.dll;*.mun;*.ocx;*.cpl|All files|*.*", CheckFileExists = true })
                if (dialog.ShowDialog(this) == DialogResult.OK) source.Text = dialog.FileName;
        }
        void ChooseSourceFolder()
        {
            using (FolderBrowserDialog dialog = new FolderBrowserDialog { Description = "Choose an installation folder. Its subfolders are included.", ShowNewFolderButton = false })
                if (dialog.ShowDialog(this) == DialogResult.OK) source.Text = dialog.SelectedPath;
        }
        void ChooseDestination()
        {
            using (FolderBrowserDialog dialog = new FolderBrowserDialog { Description = "Choose where new result folders will be saved.", SelectedPath = destination.Text })
                if (dialog.ShowDialog(this) == DialogResult.OK) destination.Text = dialog.SelectedPath;
        }
        void OpenSavedGallery()
        {
            using (OpenFileDialog dialog = new OpenFileDialog { Title = "Open a saved SVG gallery", Filter = "SVG galleries (examples.html)|examples.html|HTML files|*.html", CheckFileExists = true })
                if (dialog.ShowDialog(this) == DialogResult.OK)
                {
                    string previous = output;
                    try
                    {
                        output = Path.GetDirectoryName(dialog.FileName);
                        if (OpenGallery()) { status.Text = "Saved gallery opened."; details.Text = "Results: " + output; }
                        else output = previous;
                        SetRunning(false);
                    }
                    catch (Exception ex) { output = previous; details.Text = ex.Message; }
                }
        }
        bool OpenGallery()
        {
            try
            {
                SvgGalleryServer server;
                if (!galleries.TryGetValue(output, out server))
                {
                    server = new SvgGalleryServer(output, SvgExtraction.KeepDirectory);
                    galleries.Add(output, server);
                }
                OpenResult(server.Url);
                return true;
            }
            catch (Exception ex) { details.Text = "Could not open the gallery: " + ex.Message; return false; }
        }
        void StopScan()
        {
            if (active == null) return;
            active.RequestCancel(); stop.Enabled = false;
            status.Text = "Stopping; saving the results found so far...";
        }
        void SetRunning(bool running)
        {
            source.Enabled = destination.Enabled = chooseFile.Enabled = chooseFolder.Enabled = chooseDestination.Enabled = start.Enabled = !running;
            saved.Enabled = !running;
            stop.Enabled = running;
            progress.Style = running ? ProgressBarStyle.Marquee : ProgressBarStyle.Blocks;
            gallery.Enabled = folder.Enabled = !running && output != null && File.Exists(Path.Combine(output, "examples.html"));
        }
        internal async Task StartScan()
        {
            if (active != null) return;
            bool started = false;
            try
            {
                if (string.IsNullOrWhiteSpace(source.Text) || string.IsNullOrWhiteSpace(destination.Text)) throw new IOException("Choose a source and a location for the results.");
                Options options = SvgExtraction.CreateOptions(source.Text, destination.Text);
                options.ProgressChanged = message => BeginInvoke(new Action(() => { if (active != null && stop.Enabled) details.Text = message; }));
                active = new Catalog(options);
                output = options.Output;
                started = true;
                SetRunning(true);
                status.Text = "Searching for SVG artwork..."; details.Text = "Results: " + output;
                int result = await Task.Run(() => active.Run());
                status.Text = (result == 130 ? "Stopped. " : result == 2 ? "Scan limit reached. " : "Finished. ") + active.ResultText;
                details.Text = "Results: " + output + Environment.NewLine + "Open the gallery to view, keep or download SVGs. Keep copies go to: " + SvgExtraction.KeepDirectory + Environment.NewLine + "See skips.csv for files or resources that could not be checked.";
            }
            catch (Exception ex)
            {
                status.Text = "The scan could not finish.";
                details.Text = ex.Message + (!started ? "" : Environment.NewLine + "Any results saved so far are in: " + output);
            }
            finally
            {
                if (active != null) { active.Dispose(); active = null; }
                SetRunning(false);
            }
            if (closing) { Close(); return; }
            if (started && openWhenFinished.Checked && gallery.Enabled) OpenGallery();
        }
        void OpenResult(string path)
        {
            try { Process.Start(new ProcessStartInfo(path) { UseShellExecute = true }); }
            catch (Exception ex) { details.Text = "Could not open the results automatically. Open this location yourself: " + path + Environment.NewLine + ex.Message; }
        }
    }

    static class SvgExtractorProgram
    {
        [STAThread]
        public static int Main(string[] args)
        {
            AppContext.SetSwitch("Switch.System.IO.UseLegacyPathHandling", false);
            AppContext.SetSwitch("Switch.System.IO.BlockLongPaths", false);
            Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new SvgExtractorForm(args.Length == 0 ? null : args[0]));
            return 0;
        }
    }
}
