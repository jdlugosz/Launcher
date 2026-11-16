// Standalone .NET Framework 4.6.2+ console tool. No scanned assembly is loaded.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Resources;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;
using System.Xml;
using System.Xml.Linq;

namespace LauncherTools
{
    sealed class Data : IDisposable
    {
        readonly Stream stream;
        public long Length { get { return stream.Length; } }
        public Data(Stream value) { stream = value; }
        public byte[] Read(long offset, int count)
        {
            if (offset < 0 || count < 0 || offset > Length || count > Length - offset)
                throw new InvalidDataException("Offset or length lies outside the file.");
            byte[] result = new byte[count];
            stream.Position = offset;
            int done = 0;
            while (done < count)
            {
                int read = stream.Read(result, done, count - done);
                if (read == 0) throw new EndOfStreamException();
                done += read;
            }
            return result;
        }
        public ushort U16(long p) { return BitConverter.ToUInt16(Read(p, 2), 0); }
        public uint U32(long p) { return BitConverter.ToUInt32(Read(p, 4), 0); }
        public ulong U64(long p) { return BitConverter.ToUInt64(Read(p, 8), 0); }
        public void Dispose() { stream.Dispose(); }
    }

    sealed class PeImage
    {
        struct Section { public uint Rva, Size, Offset; }
        readonly Data data;
        readonly List<Section> sections = new List<Section>();
        readonly long directories;
        readonly uint directoryCount, headerSize;
        public PeImage(Data input)
        {
            data = input;
            if (data.U16(0) != 0x5a4d) throw new InvalidDataException("Not a PE image (no MZ header).");
            long pe = data.U32(0x3c);
            if (data.U32(pe) != 0x4550) throw new InvalidDataException("Not a PE image (no PE signature).");
            int count = data.U16(pe + 6), optionalSize = data.U16(pe + 20);
            long optional = pe + 24;
            ushort magic = data.U16(optional);
            int directoryOffset = magic == 0x10b ? 96 : magic == 0x20b ? 112 : 0;
            if (directoryOffset == 0 || optionalSize < directoryOffset || count > 512)
                throw new InvalidDataException("Invalid PE optional header or section count.");
            directories = optional + directoryOffset;
            directoryCount = Math.Min(data.U32(directories - 4), (uint)((optionalSize - directoryOffset) / 8));
            headerSize = data.U32(optional + 60);
            for (int i = 0; i < count; ++i)
            {
                long p = optional + optionalSize + i * 40;
                sections.Add(new Section { Rva = data.U32(p + 12), Size = data.U32(p + 16), Offset = data.U32(p + 20) });
            }
        }
        public long Rva(uint rva, uint length)
        {
            foreach (Section s in sections)
                if (rva >= s.Rva && (ulong)rva - s.Rva <= s.Size && length <= s.Size - (rva - s.Rva))
                {
                    long p = (long)s.Offset + rva - s.Rva;
                    if (p <= data.Length && length <= data.Length - p) return p;
                }
            if (rva < headerSize && length <= (ulong)headerSize - rva && (ulong)rva + length <= (ulong)data.Length) return rva;
            throw new InvalidDataException("PE resource RVA is not backed by file bytes.");
        }
        public uint[] Directory(int index)
        {
            return index < directoryCount ? new[] { data.U32(directories + index * 8), data.U32(directories + index * 8 + 4) } : new uint[2];
        }
        public void NativeResources(Action<long, uint, string, string> visit)
        {
            uint[] resource = Directory(2);
            if (resource[0] == 0 || resource[1] == 0) return;
            long root = Rva(resource[0], resource[1]);
            HashSet<uint> seen = new HashSet<uint>();
            int entries = 0;
            Action<uint, string, string, int> walk = null;
            walk = (relative, location, label, depth) =>
            {
                if (depth > 8 || !seen.Add(relative)) throw new InvalidDataException("Cyclic or excessively deep resource directory.");
                if ((ulong)relative + 16 > resource[1]) throw new InvalidDataException("Resource directory exceeds its declared size.");
                long dir = root + relative;
                int count = data.U16(dir + 12) + data.U16(dir + 14);
                if ((ulong)relative + 16 + (ulong)count * 8 > resource[1] || (entries += count) > 200000)
                    throw new InvalidDataException("Invalid or excessive resource entries.");
                for (int i = 0; i < count; ++i)
                {
                    uint name = data.U32(dir + 16 + i * 8), target = data.U32(dir + 20 + i * 8);
                    string part;
                    if ((name & 0x80000000) != 0)
                    {
                        uint np = name & 0x7fffffff;
                        if ((ulong)np + 2 > resource[1]) throw new InvalidDataException("Invalid resource name.");
                        int chars = data.U16(root + np);
                        if ((ulong)np + 2 + (ulong)chars * 2 > resource[1]) throw new InvalidDataException("Resource name exceeds its directory.");
                        part = Encoding.Unicode.GetString(data.Read(root + np + 2, chars * 2));
                    }
                    else part = "#" + name.ToString(CultureInfo.InvariantCulture);
                    string path = location + "/" + part;
                    string assetName = depth < 2 ? label + "/" + part : label;
                    if ((target & 0x80000000) != 0) walk(target & 0x7fffffff, path, assetName, depth + 1);
                    else
                    {
                        if ((ulong)target + 16 > resource[1]) throw new InvalidDataException("Invalid resource data entry.");
                        uint length = data.U32(root + target + 4);
                        visit(Rva(data.U32(root + target), length), length, path, assetName);
                    }
                }
            };
            walk(0, "PE", "", 0);
        }

        // ECMA-335 table schemas through ManifestResource (table 40). Reading
        // metadata directly works with missing dependencies and any CPU target.
        static readonly string[] Schemas = {
            "2 s g g g", "ResolutionScope s s", "4 s s TypeDefOrRef t4 t6", "t4", "2 s b", "t6",
            "4 2 2 s b t8", "t8", "2 2 s", "t2 TypeDefOrRef", "MemberRefParent s b", "2 HasConstant b",
            "HasCustomAttribute CustomAttributeType b", "HasFieldMarshal b", "2 HasDeclSecurity b", "2 4 t2",
            "4 t4", "b", "t2 t20", "t20", "2 s TypeDefOrRef", "t2 t23", "t23", "2 s b",
            "2 t6 HasSemantics", "t2 MethodDefOrRef MethodDefOrRef", "s", "b", "2 MemberForwarded s t26",
            "4 t4", "4 4", "4", "4 2 2 2 2 4 b s s", "4", "4 4 4", "2 2 2 2 4 b s s b",
            "4 t35", "4 4 4 t35", "4 s b", "4 4 s s Implementation", "4 4 s Implementation"
        };
        static readonly Dictionary<string, int[]> Coded = new Dictionary<string, int[]> {
            {"ResolutionScope", new[] {2, 0, 26, 35, 1}}, {"TypeDefOrRef", new[] {2, 2, 1, 27}},
            {"MemberRefParent", new[] {3, 2, 1, 26, 6, 27}}, {"HasConstant", new[] {2, 4, 8, 23}},
            {"HasCustomAttribute", new[] {5, 6, 4, 1, 2, 8, 9, 10, 0, 14, 23, 20, 17, 26, 27, 32, 35, 38, 39, 40, 42, 44, 43}},
            {"CustomAttributeType", new[] {3, 6, 10}}, {"HasFieldMarshal", new[] {1, 4, 8}},
            {"HasDeclSecurity", new[] {2, 2, 6, 32}}, {"HasSemantics", new[] {1, 20, 23}},
            {"MethodDefOrRef", new[] {1, 6, 10}}, {"MemberForwarded", new[] {1, 4, 6}},
            {"Implementation", new[] {2, 38, 35, 39}}
        };
        public void ManagedResources(Action<long, uint, string, string> visit, Action<string, string> linked)
        {
            uint[] cli = Directory(14);
            if (cli[0] == 0 || cli[1] < 32) return;
            long header = Rva(cli[0], 32);
            uint resourceRva = data.U32(header + 24), resourceSize = data.U32(header + 28);
            uint metadataSize = data.U32(header + 12);
            long metadata = Rva(data.U32(header + 8), metadataSize);
            if (data.U32(metadata) != 0x424a5342) throw new InvalidDataException("Invalid CLR metadata signature.");
            uint versionBytes = data.U32(metadata + 12);
            if (versionBytes > metadataSize || versionBytes > 4096) throw new InvalidDataException("Invalid CLR version string.");
            long p = metadata + ((16 + versionBytes + 3) & ~3U);
            int streamCount = data.U16(p + 2);
            p += 4;
            Dictionary<string, long[]> streams = new Dictionary<string, long[]>();
            for (int i = 0; i < streamCount; ++i)
            {
                uint offset = data.U32(p), size = data.U32(p + 4);
                byte[] name = new byte[32];
                int n = 0;
                while (n < name.Length && (name[n] = data.Read(p + 8 + n, 1)[0]) != 0) ++n;
                if (n == name.Length || (ulong)offset + size > metadataSize) throw new InvalidDataException("Invalid CLR metadata stream.");
                streams[Encoding.ASCII.GetString(name, 0, n)] = new[] { metadata + offset, (long)size };
                p += 8 + ((n + 1 + 3) & ~3);
            }
            long[] tables, strings;
            if ((!streams.TryGetValue("#~", out tables) && !streams.TryGetValue("#-", out tables)) || !streams.TryGetValue("#Strings", out strings)) return;
            p = tables[0];
            byte heaps = data.Read(p + 6, 1)[0];
            ulong valid = data.U64(p + 8);
            uint[] rows = new uint[64];
            p += 24;
            for (int i = 0; i < 64; ++i) if ((valid & (1UL << i)) != 0) { rows[i] = data.U32(p); p += 4; }
            Func<string, int> width = column =>
            {
                if (column == "2" || column == "4") return int.Parse(column, CultureInfo.InvariantCulture);
                if (column == "s") return (heaps & 1) != 0 ? 4 : 2;
                if (column == "g") return (heaps & 2) != 0 ? 4 : 2;
                if (column == "b") return (heaps & 4) != 0 ? 4 : 2;
                if (column[0] == 't') return rows[int.Parse(column.Substring(1), CultureInfo.InvariantCulture)] >= 65536 ? 4 : 2;
                int[] coded = Coded[column];
                uint maximum = 0;
                for (int i = 1; i < coded.Length; ++i) maximum = Math.Max(maximum, rows[coded[i]]);
                return maximum >= (1U << (16 - coded[0])) ? 4 : 2;
            };
            for (int table = 0; table < 40; ++table) p += (long)rows[table] * Schemas[table].Split(' ').Sum(width);
            int stringWidth = width("s"), implementationWidth = width("Implementation");
            int rowWidth = 8 + stringWidth + implementationWidth;
            if (rows[40] > 200000 || p + (long)rows[40] * rowWidth > tables[0] + tables[1]) throw new InvalidDataException("Invalid ManifestResource table.");
            for (uint i = 0; i < rows[40]; ++i, p += rowWidth)
            {
                uint offset = data.U32(p), nameIndex = stringWidth == 2 ? data.U16(p + 8) : data.U32(p + 8);
                uint implementation = implementationWidth == 2 ? data.U16(p + 8 + stringWidth) : data.U32(p + 8 + stringWidth);
                if (nameIndex >= strings[1]) throw new InvalidDataException("Invalid manifest resource name.");
                byte[] nameBytes = data.Read(strings[0] + nameIndex, (int)Math.Min(4096, strings[1] - nameIndex));
                int end = Array.IndexOf(nameBytes, (byte)0);
                if (end < 0) throw new InvalidDataException("Unterminated manifest resource name.");
                string name = Encoding.UTF8.GetString(nameBytes, 0, end);
                if (implementation != 0) { linked(name, "Linked CLR resource; bytes live outside this assembly."); continue; }
                if (resourceRva == 0 || (ulong)offset + 4 > resourceSize) throw new InvalidDataException("Invalid embedded CLR resource offset.");
                long payload = Rva(resourceRva, resourceSize) + offset;
                uint length = data.U32(payload);
                if ((ulong)offset + 4 + length > resourceSize) throw new InvalidDataException("CLR resource exceeds its declared area.");
                visit(payload + 4, length, "CLR/" + name, name);
            }
        }
    }

    sealed class Options
    {
        public readonly List<string> Roots = new List<string>(), Excludes = new List<string>();
        public string Output, CancelFile, CatalogInput;
        public bool AllDrives, IncludeZip, Export, SvgOnly;
        public Action<string> ProgressChanged = null;
        public int MaxBytes = 32 * 1024 * 1024, MaxFiles = int.MaxValue;
        public long MaxExportBytes = 512L * 1024 * 1024;
        public double MaxSeconds = double.MaxValue;
        public static Options Parse(string[] args)
        {
            Options o = new Options();
            for (int i = 0; i < args.Length; ++i)
            {
                string arg = args[i];
                if (arg == "--all-drives") o.AllDrives = true;
                else if (arg == "--include-zip") o.IncludeZip = true;
                else if (arg == "--export") o.Export = true;
                else if (arg == "--svg-only") o.SvgOnly = true;
                else if (arg == "--output" && i + 1 < args.Length) o.Output = Path.GetFullPath(args[++i]);
                else if (arg == "--from-catalog" && i + 1 < args.Length) o.CatalogInput = Path.GetFullPath(args[++i]);
                else if (arg == "--cancel-file" && i + 1 < args.Length) o.CancelFile = Path.GetFullPath(args[++i]);
                else if (arg == "--exclude" && i + 1 < args.Length) o.Excludes.Add(Path.GetFullPath(args[++i]).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar);
                else if (arg == "--max-mb" && i + 1 < args.Length) o.MaxBytes = checked(int.Parse(args[++i], CultureInfo.InvariantCulture) * 1024 * 1024);
                else if (arg == "--max-export-mb" && i + 1 < args.Length) o.MaxExportBytes = checked(long.Parse(args[++i], CultureInfo.InvariantCulture) * 1024 * 1024);
                else if (arg == "--max-files" && i + 1 < args.Length) o.MaxFiles = int.Parse(args[++i], CultureInfo.InvariantCulture);
                else if (arg == "--max-seconds" && i + 1 < args.Length) o.MaxSeconds = double.Parse(args[++i], CultureInfo.InvariantCulture);
                else if (arg.StartsWith("--", StringComparison.Ordinal)) throw new ArgumentException("Unknown or incomplete option: " + arg);
                else o.Roots.Add(Path.GetFullPath(arg));
            }
            if (o.MaxBytes < 1 || o.MaxExportBytes < 1 || o.MaxFiles < 1 || o.MaxSeconds <= 0 || double.IsNaN(o.MaxSeconds)) throw new ArgumentException("Limits must be positive.");
            if (o.AllDrives)
                foreach (DriveInfo d in DriveInfo.GetDrives())
                    try { if (d.IsReady && (d.DriveType == DriveType.Fixed || d.DriveType == DriveType.Removable || d.DriveType == DriveType.Network)) o.Roots.Add(d.RootDirectory.FullName); }
                    catch (IOException) { }
                    catch (UnauthorizedAccessException) { }
            if (o.CatalogInput != null && (o.Roots.Count != 0 || o.AllDrives)) throw new ArgumentException("Use either --from-catalog or roots/--all-drives.");
            if ((o.Roots.Count == 0 && o.CatalogInput == null) || o.Output == null) throw new ArgumentException("Supply roots, --all-drives or --from-catalog, and --output <new-directory>.");
            return o;
        }
    }

    sealed class Csv : IDisposable
    {
        readonly StreamWriter writer;
        public Csv(string path, params string[] header)
        {
            writer = new StreamWriter(new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read), new UTF8Encoding(true));
            Row(header.Cast<object>().ToArray());
        }
        public void Row(params object[] fields)
        {
            writer.WriteLine(string.Join(",", fields.Select(v =>
            {
                string text = SafeText(Convert.ToString(v, CultureInfo.InvariantCulture));
                return "\"" + text.Replace("\"", "\"\"") + "\"";
            })));
        }
        public static string SafeText(string text)
        {
            return text.Length != 0 && "=+-@\t\r\n".IndexOf(text[0]) >= 0 ? "'" + text : text;
        }
        public static IEnumerable<string[]> Read(string path)
        {
            using (StreamReader input = new StreamReader(path, Encoding.UTF8, true))
            {
                List<string> fields = new List<string>();
                StringBuilder value = new StringBuilder();
                bool quoted = false;
                int next;
                while ((next = input.Read()) >= 0)
                {
                    char c = (char)next;
                    if (c == '"')
                    {
                        if (quoted && input.Peek() == '"') { input.Read(); value.Append('"'); }
                        else if (!quoted && value.Length != 0) throw new InvalidDataException("Unexpected quote in catalog CSV.");
                        else quoted = !quoted;
                    }
                    else if (!quoted && (c == ',' || c == '\r' || c == '\n'))
                    {
                        fields.Add(value.ToString()); value.Clear();
                        if (c != ',')
                        {
                            if (c == '\r' && input.Peek() == '\n') input.Read();
                            yield return fields.ToArray(); fields.Clear();
                        }
                    }
                    else value.Append(c);
                }
                if (quoted) throw new InvalidDataException("Catalog CSV ends inside a quoted field.");
                if (fields.Count != 0 || value.Length != 0) { fields.Add(value.ToString()); yield return fields.ToArray(); }
            }
        }
        public void Flush() { writer.Flush(); }
        public void Dispose() { writer.Dispose(); }
    }

    sealed class Catalog : IDisposable
    {
        sealed class Request { public string Member, Location, Kind; }
        readonly Options options;
        readonly Csv resources, files, skips;
        readonly Stopwatch elapsed = Stopwatch.StartNew();
        readonly Dictionary<string, int> kinds = new Dictionary<string, int>();
        readonly List<Dictionary<string, object>> examples = new List<Dictionary<string, object>>();
        readonly Dictionary<string, Dictionary<string, object>> exported = new Dictionary<string, Dictionary<string, object>>();
        readonly Dictionary<string, string> exportedPaths = new Dictionary<string, string>();
        long exportedBytes;
        readonly Dictionary<string, Dictionary<string, Request>> catalogTargets;
        Dictionary<string, Request> pendingRequests;
        HashSet<string> requestedLocations, requestedMembers;
        int requestedResources;
        readonly HashSet<string> visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string, int> currentKinds = new Dictionary<string, int>();
        readonly List<string> sampleNames = new List<string>();
        string currentFile, currentMember;
        int currentMatches, matchedFiles, findings, skipped, inspected, encountered;
        double nextProgress = 10;
        double nextUserProgress;
        volatile bool cancelled;
        bool limited, failed;
        double nextCancelCheck;
        static readonly HashSet<string> PeExtensions = new HashSet<string>(new[] { ".exe", ".dll", ".mui", ".mun", ".ocx", ".cpl", ".scr", ".sys", ".icl", ".ax", ".winmd", ".wcx", ".wfx", ".wlx", ".wdx", ".wll", ".xll", ".pyd", ".node", ".acm", ".drv", ".api", ".aip", ".aex", ".8bf", ".8bi", ".vst3", ".bpl" });
        static readonly HashSet<string> AssetExtensions = new HashSet<string>(new[] { ".svg", ".svgz", ".xaml", ".axaml", ".baml", ".xbf", ".pri", ".resources", ".xml", ".manifest" });
        static readonly HashSet<string> PackageExtensions = new HashSet<string>(new[] { ".appx", ".msix", ".appxbundle", ".msixbundle", ".vsix", ".nupkg" });
        static readonly HashSet<string> VectorElements = new HashSet<string>(new[] { "DrawingImage", "DrawingGroup", "GeometryDrawing", "PathGeometry", "StreamGeometry", "Path", "PathIcon", "Geometry", "VectorImageSource" });
        public Catalog(Options o)
        {
            options = o;
            if (o.CatalogInput != null) catalogTargets = ReadTargets(o.CatalogInput);
            if (Directory.Exists(o.Output) || File.Exists(o.Output)) throw new IOException("Output already exists. Choose a new directory to preserve earlier catalogs: " + o.Output);
            Directory.CreateDirectory(o.Output);
            resources = new Csv(Path.Combine(o.Output, "resources.csv"), "File", "Member", "Location", "Kind", "Confidence", "Bytes", "IconHint", "Evidence", "ExportedFile", "AssetSha256");
            files = new Csv(Path.Combine(o.Output, "files.csv"), "File", "Findings", "Svg", "VectorXaml", "Baml", "Xbf", "Pri", "OtherCandidates", "References", "SampleNames");
            skips = new Csv(Path.Combine(o.Output, "skips.csv"), "File", "Member", "Location", "Reason");
        }
        public void Dispose() { resources.Dispose(); files.Dispose(); skips.Dispose(); }
        static string RequestKey(string member, string location, string kind)
        {
            return member.Length + ":" + member + location.Length + ":" + location + kind;
        }
        static int KindRank(string kind)
        {
            return kind == "svg" ? 0 : kind == "vector-xaml" ? 1 : kind == "xaml" ? 2 :
                kind == "baml" ? 3 : kind == "xbf" ? 4 : kind == "pri" ? 5 : 6;
        }
        Dictionary<string, Dictionary<string, Request>> ReadTargets(string path)
        {
            Dictionary<string, Dictionary<string, Request>> targets = new Dictionary<string, Dictionary<string, Request>>(StringComparer.OrdinalIgnoreCase);
            using (IEnumerator<string[]> input = Csv.Read(path).GetEnumerator())
            {
                if (!input.MoveNext()) throw new InvalidDataException("Catalog CSV is empty.");
                string[] header = input.Current;
                int fileColumn = Array.IndexOf(header, "File"), memberColumn = Array.IndexOf(header, "Member"),
                    locationColumn = Array.IndexOf(header, "Location"), kindColumn = Array.IndexOf(header, "Kind");
                if (fileColumn < 0 || memberColumn < 0 || locationColumn < 0 || kindColumn < 0)
                    throw new InvalidDataException("Use resources.csv; File, Member, Location and Kind columns are required.");
                int required = Math.Max(Math.Max(fileColumn, memberColumn), Math.Max(locationColumn, kindColumn));
                while (input.MoveNext())
                {
                    string[] row = input.Current;
                    if (row.Length == 1 && row[0].Length == 0) continue;
                    if (row.Length <= required || !Path.IsPathRooted(row[fileColumn])) throw new InvalidDataException("Invalid or incomplete resource catalog row.");
                    if (options.SvgOnly && row[kindColumn] != "svg") continue;
                    string file = Path.GetFullPath(row[fileColumn]);
                    Dictionary<string, Request> group;
                    if (!targets.TryGetValue(file, out group)) targets.Add(file, group = new Dictionary<string, Request>());
                    Request request = new Request { Member = row[memberColumn], Location = row[locationColumn], Kind = row[kindColumn] };
                    string key = RequestKey(request.Member, request.Location, request.Kind);
                    if (!group.ContainsKey(key)) { group.Add(key, request); ++requestedResources; }
                }
            }
            return targets;
        }
        void SelectRequests(string file)
        {
            if (catalogTargets == null) return;
            pendingRequests = new Dictionary<string, Request>(catalogTargets[file]);
            requestedLocations = new HashSet<string>(); requestedMembers = new HashSet<string>();
            foreach (Request request in pendingRequests.Values)
            {
                requestedLocations.Add(request.Location);
                for (int p = request.Location.IndexOf('/'); p >= 0; p = request.Location.IndexOf('/', p + 1)) requestedLocations.Add(request.Location.Substring(0, p));
                requestedMembers.Add(request.Member);
                for (int p = request.Member.IndexOf("!/", StringComparison.Ordinal); p >= 0; p = request.Member.IndexOf("!/", p + 2, StringComparison.Ordinal)) requestedMembers.Add(request.Member.Substring(0, p));
            }
        }
        void Skip(string location, string reason)
        {
            ++skipped;
            skips.Row(currentFile, currentMember, location, reason);
        }
        void Finding(string location, string kind, string confidence, long bytes, string label, string evidence, byte[] payload = null)
        {
            if (options.SvgOnly && kind != "svg") return;
            if (pendingRequests != null && !pendingRequests.Remove(RequestKey(Csv.SafeText(currentMember), location, kind))) return;
            string lower = label.ToLowerInvariant();
            bool hint = lower.Contains("icon") || lower.Contains("logo") || lower.Contains("brand") || lower.Contains("appimage");
            string assetHash = "", assetFile = "";
            if (options.Export && payload != null) assetFile = ExportAsset(payload, kind, location, label, out assetHash);
            resources.Row(currentFile, currentMember, location, kind, confidence, bytes, hint ? "name suggests icon/logo" : "purpose unknown", evidence, assetFile, assetHash);
            ++findings; ++currentMatches;
            kinds[kind] = kinds.ContainsKey(kind) ? kinds[kind] + 1 : 1;
            currentKinds[kind] = currentKinds.ContainsKey(kind) ? currentKinds[kind] + 1 : 1;
            if (sampleNames.Count < 6) sampleNames.Add((currentMember.Length == 0 ? "" : currentMember + "!/") + location);
            int rank = kind == "svg" && location.StartsWith("PE/", StringComparison.Ordinal) ? 0 :
                kind == "svg" && location.StartsWith("CLR/", StringComparison.Ordinal) ? 1 :
                kind == "vector-xaml" ? 2 : kind == "baml" ? 3 : kind == "svg" ? 4 : 5;
            Dictionary<string, object> example = new Dictionary<string, object> {
                {"Rank", rank}, {"File", currentFile}, {"Member", currentMember}, {"Location", location},
                {"Kind", kind}, {"Confidence", confidence}, {"IconHint", hint}, {"Evidence", evidence},
                {"ExportedFile", assetFile}, {"AssetSha256", assetHash}
            };
            if (assetFile.Length != 0 && !exported.ContainsKey(assetFile)) exported.Add(assetFile, example);
            if ((examples.Count < 250 || rank < (int)examples[examples.Count - 1]["Rank"]) &&
                !examples.Any(e => (string)e["File"] == currentFile && (string)e["Member"] == currentMember && (string)e["Kind"] == kind))
            {
                examples.Add(example);
                examples.Sort((a, b) => ((int)a["Rank"]).CompareTo((int)b["Rank"]));
                if (examples.Count > 250) examples.RemoveAt(examples.Count - 1);
            }
        }
        string ExportAsset(byte[] payload, string kind, string location, string label, out string digest)
        {
            using (System.Security.Cryptography.SHA256 hash = System.Security.Cryptography.SHA256.Create())
                digest = BitConverter.ToString(hash.ComputeHash(payload)).Replace("-", "").ToLowerInvariant();
            string extension = kind == "svg" ? "svg" : kind == "vector-xaml" || kind == "xaml" ? "xaml" :
                kind == "baml" ? "baml" : kind == "xbf" ? "xbf" : kind == "pri" ? "pri" : "bin";
            string identity = digest + "." + extension, relative;
            if (exportedPaths.TryGetValue(identity, out relative)) return relative;
            // A sanitized basename is decorative; the full hash supplies identity.
            // Neither resource paths nor archive paths become output directories.
            relative = "assets/" + (options.SvgOnly ? ExportStem(label) + "--" : "") + identity;
            if (payload.Length > options.MaxExportBytes - exportedBytes)
            {
                Skip(location, "Export skipped: total --max-export-mb budget reached; cataloging continues.");
                return "";
            }
            string directory = Path.Combine(options.Output, "assets");
            Directory.CreateDirectory(directory);
            using (FileStream output = new FileStream(Path.Combine(options.Output, relative), FileMode.CreateNew, FileAccess.Write))
                output.Write(payload, 0, payload.Length);
            exportedBytes += payload.Length;
            exportedPaths.Add(identity, relative);
            return relative;
        }
        static string ExportStem(string label)
        {
            int slash = Math.Max(label.LastIndexOf('/'), label.LastIndexOf('\\'));
            string name = label.Substring(slash + 1), extension = Extension(name);
            if (extension.Length != 0) name = name.Substring(0, name.Length - extension.Length);
            name = Regex.Replace(name, @"[^\p{L}\p{Nd}_-]+", "-").Trim('-', '_');
            if (name.Length > 48) name = name.Substring(0, 48);
            return name.Length == 0 ? "svg" : name;
        }
        byte[] ExportBytes(Data data, long offset, long length, string location)
        {
            if (!options.Export || options.SvgOnly) return null;
            if (length > options.MaxBytes)
            {
                Skip(location, "Export skipped: resource exceeds --max-mb.");
                return null;
            }
            return data.Read(offset, (int)length);
        }
        bool StopRequested()
        {
            if (cancelled) return true;
            if (elapsed.Elapsed.TotalSeconds >= nextCancelCheck)
            {
                nextCancelCheck = elapsed.Elapsed.TotalSeconds + 1;
                if (File.Exists(options.CancelFile ?? Path.Combine(options.Output, "cancel.request"))) { cancelled = true; return true; }
            }
            if (inspected >= options.MaxFiles || elapsed.Elapsed.TotalSeconds >= options.MaxSeconds) { limited = true; return true; }
            return false;
        }
        bool Wanted(string path)
        {
            string ext = Extension(path);
            return PeExtensions.Contains(ext) || AssetExtensions.Contains(ext) || PackageExtensions.Contains(ext) || ((options.IncludeZip || catalogTargets != null) && ext == ".zip");
        }
        static string Extension(string name)
        {
            // CLR resource keys and ZIP member names are identifiers, not
            // Windows filesystem paths (they can contain ':' or '<', etc.).
            int slash = Math.Max(name.LastIndexOf('/'), name.LastIndexOf('\\'));
            int dot = name.LastIndexOf('.');
            return dot > slash ? name.Substring(dot).ToLowerInvariant() : "";
        }
        public int Run()
        {
            Console.CancelKeyPress += Cancel;
            Console.WriteLine("Scanning read-only. Ctrl+C finishes a partial catalog. Output: " + options.Output);
            try
            {
                IEnumerable<string> targets = catalogTargets == null ? options.Roots.Distinct(StringComparer.OrdinalIgnoreCase) :
                    catalogTargets.OrderBy(pair => pair.Value.Values.Min(r => KindRank(r.Kind))).Select(pair => pair.Key);
                Stack<string> pending = new Stack<string>(targets.Reverse());
                while (pending.Count != 0 && !StopRequested())
                {
                    string path = pending.Pop();
                    currentFile = path; currentMember = "";
                    try
                    {
                        string canonical = Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
                        if (!visited.Add(canonical)) continue;
                        if (canonical.StartsWith(options.Output.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) ||
                            options.Excludes.Any(e => canonical.StartsWith(e, StringComparison.OrdinalIgnoreCase))) continue;
                        FileAttributes attributes = File.GetAttributes(path);
                        if ((attributes & FileAttributes.ReparsePoint) != 0) { Skip("filesystem", "Reparse point not followed (junction, symbolic link or cloud placeholder)."); continue; }
                        if ((attributes & FileAttributes.Directory) != 0)
                        {
                            if (catalogTargets != null) { Skip("catalog", "Catalog target is a directory; it was not traversed."); continue; }
                            foreach (string child in Directory.EnumerateFileSystemEntries(path))
                            {
                                if (StopRequested()) break;
                                // Keep only directories and relevant file extensions on the stack.
                                try { if (Wanted(child) || (File.GetAttributes(child) & FileAttributes.Directory) != 0) pending.Push(child); }
                                catch (Exception ex) { if (!Recoverable(ex)) throw; Skip(child, ex.Message); }
                                ++encountered;
                                Progress();
                            }
                        }
                        else if (catalogTargets != null || Wanted(path)) { SelectRequests(path); ScanFile(path); }
                    }
                    catch (Exception ex) { if (!Recoverable(ex)) throw; Skip("filesystem", ex.GetType().Name + ": " + ex.Message); }
                    Progress();
                }
            }
            catch { failed = true; throw; }
            finally
            {
                Console.CancelKeyPress -= Cancel;
                WriteSummary();
                resources.Flush(); files.Flush(); skips.Flush();
            }
            return cancelled ? 130 : limited ? 2 : 0;
        }
        static bool Recoverable(Exception ex)
        {
            return ex is IOException || ex is UnauthorizedAccessException || ex is System.Security.SecurityException || ex is InvalidDataException || ex is BadImageFormatException ||
                ex is ArgumentException || ex is FormatException || ex is OverflowException || ex is XmlException || ex is NotSupportedException;
        }
        public void RequestCancel() { cancelled = true; }
        public string ResultText
        {
            get { return string.Format(CultureInfo.CurrentCulture, "Files checked: {0:N0}; SVGs found: {1:N0}; unique SVGs saved: {2:N0}; skips: {3:N0}.", inspected, CountAll("svg"), exported.Count, skipped); }
        }
        int CountAll(string kind) { int value; return kinds.TryGetValue(kind, out value) ? value : 0; }
        void Cancel(object sender, ConsoleCancelEventArgs e) { e.Cancel = true; RequestCancel(); }
        void Progress()
        {
            if (options.ProgressChanged != null && elapsed.Elapsed.TotalSeconds >= nextUserProgress)
            {
                nextUserProgress = elapsed.Elapsed.TotalSeconds + 1;
                options.ProgressChanged(ResultText + Environment.NewLine + currentFile);
            }
            if (elapsed.Elapsed.TotalSeconds < nextProgress) return;
            nextProgress = elapsed.Elapsed.TotalSeconds + 10;
            resources.Flush(); files.Flush(); skips.Flush();
            string status = string.Format(CultureInfo.InvariantCulture, "{0:n0} entries; {1:n0} inspected; {2:n0} matched files; {3:n0} findings; {4:n0} skips; {5:0}s", encountered, inspected, matchedFiles, findings, skipped, elapsed.Elapsed.TotalSeconds);
            Console.WriteLine(status + "\n  " + currentFile);
            File.WriteAllText(Path.Combine(options.Output, "progress.txt"), status + Environment.NewLine + currentFile, Encoding.UTF8);
        }
        void ScanFile(string path)
        {
            ++inspected;
            currentMatches = 0; currentKinds.Clear(); sampleNames.Clear();
            try
            {
                using (FileStream stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                {
                    string ext = Extension(path);
                    if (PackageExtensions.Contains(ext) || ((options.IncludeZip || catalogTargets != null) && ext == ".zip")) ScanZip(stream, "", 0);
                    else using (Data data = new Data(stream)) ScanData(data, path);
                }
            }
            catch (Exception ex) { if (!Recoverable(ex)) throw; Skip("file", ex.GetType().Name + ": " + ex.Message); }
            if (pendingRequests != null)
                foreach (Request missing in pendingRequests.Values)
                {
                    currentMember = missing.Member;
                    Skip(missing.Location, "Cataloged resource was not found or its format changed since the scan.");
                }
            currentMember = "";
            if (currentMatches != 0)
            {
                ++matchedFiles;
                files.Row(path, currentMatches, Count("svg"), Count("vector-xaml"), Count("baml"), Count("xbf"), Count("pri"), Count("svg-named-resource") + Count("compiled-resource") + Count("xaml"), Count("svg-reference") + Count("package-logo") + Count("linked-resource"), string.Join("; ", sampleNames));
            }
        }
        int Count(string kind) { int value; return currentKinds.TryGetValue(kind, out value) ? value : 0; }
        void ScanData(Data data, string label)
        {
            if (data.Length >= 64 && data.U16(0) == 0x5a4d)
            {
                PeImage image = new PeImage(data);
                Action<long, uint, string, string> visit = (offset, size, location, name) =>
                {
                    try { ScanPayload(data, offset, size, location, name, 0); }
                    catch (Exception ex) { if (!Recoverable(ex)) throw; Skip(location, ex.Message); }
                };
                try { image.NativeResources(visit); }
                catch (Exception ex) { if (!Recoverable(ex)) throw; Skip("PE resources", ex.Message); }
                try { image.ManagedResources(visit,
                    (name, reason) => { if (AssetExtensions.Contains(Extension(name))) Finding("CLR/" + name, "linked-resource", "reference", 0, name, reason); }); }
                catch (Exception ex) { if (!Recoverable(ex)) throw; Skip("CLR resources", ex.Message); }
            }
            else if (PeExtensions.Contains(Extension(label))) Skip("PE", "File extension suggests PE, but no PE header was found.");
            else ScanPayload(data, 0, data.Length, "file", label, 0);
        }
        void ScanZip(Stream stream, string prefix, int depth)
        {
            if (depth > 2) { Skip(prefix, "Package nesting exceeds two levels."); return; }
            using (ZipArchive zip = new ZipArchive(stream, ZipArchiveMode.Read, true))
            {
                int entries = 0;
                foreach (ZipArchiveEntry entry in zip.Entries)
                {
                    if (StopRequested()) break;
                    if (++entries > 200000) { Skip(prefix, "Archive entry limit exceeded."); break; }
                    if (entry.Name.Length == 0 || !Wanted(entry.FullName)) continue;
                    currentMember = prefix + entry.FullName;
                    if (requestedMembers != null && !requestedMembers.Contains(Csv.SafeText(currentMember))) continue;
                    if (entry.Length > options.MaxBytes) { Skip("archive member", "Member exceeds --max-mb; not decompressed."); continue; }
                    using (Stream input = entry.Open())
                    using (MemoryStream memory = new MemoryStream())
                    {
                        CopyBounded(input, memory);
                        memory.Position = 0;
                        string ext = Extension(entry.FullName);
                        try
                        {
                            if (PackageExtensions.Contains(ext) || ((options.IncludeZip || catalogTargets != null) && ext == ".zip")) ScanZip(memory, currentMember + "!/", depth + 1);
                            else using (Data data = new Data(memory)) ScanData(data, entry.FullName);
                        }
                        catch (Exception ex) { if (!Recoverable(ex)) throw; Skip("archive member", ex.Message); }
                    }
                }
            }
            currentMember = prefix;
        }
        void CopyBounded(Stream input, Stream output)
        {
            byte[] buffer = new byte[81920];
            int n;
            while ((n = input.Read(buffer, 0, buffer.Length)) != 0)
            {
                if (output.Length + n > options.MaxBytes) throw new InvalidDataException("Decompressed asset exceeds --max-mb.");
                output.Write(buffer, 0, n);
            }
        }
        void ScanPayload(Data data, long offset, long length, string location, string label, int depth)
        {
            if (cancelled || length == 0) return;
            if (requestedLocations != null && !requestedLocations.Contains(location)) return;
            if (depth > 4) { Skip(location, "Resource nesting exceeds four levels."); return; }
            byte[] prefix = data.Read(offset, (int)Math.Min(length, 4096));
            string ext = Extension(label);
            if (ext == ".dtd") return;
            bool resourcesMagic = prefix.Length >= 4 && BitConverter.ToUInt32(prefix, 0) == 0xbeefcace;
            bool gzip = prefix.Length >= 2 && prefix[0] == 0x1f && prefix[1] == 0x8b;
            string peek = Decode(prefix).TrimStart('\ufeff', ' ', '\t', '\r', '\n');
            // Ordinary localized strings such as "<None>", HTML error pages
            // and binary resources beginning with byte 0x3c are not XML assets.
            bool xml = peek.StartsWith("<", StringComparison.Ordinal) &&
                (ext == ".svg" || ext == ".xaml" || ext == ".axaml" ||
                 peek.Contains("http://www.w3.org/2000/svg") || peek.Contains("schemas.microsoft.com/winfx/") ||
                 peek.Contains("https://github.com/avaloniaui") || peek.Contains("schemas.microsoft.com/appx/manifest/") ||
                 peek.IndexOf(".svg", StringComparison.OrdinalIgnoreCase) >= 0 ||
                 Regex.IsMatch(peek, @"<(?:[\w.-]+:)?svg(?:\s|>)", RegexOptions.CultureInvariant));
            if (ext == ".baml") { Finding(location, "baml", "candidate", length, label, "Compiled WPF XAML resource; not decompiled. May describe ordinary UI rather than an icon.", ExportBytes(data, offset, length, location)); return; }
            if (ext == ".xbf" || (prefix.Length >= 4 && Encoding.ASCII.GetString(prefix, 0, 4) == "XBF\0"))
            { Finding(location, "xbf", "candidate", length, label, "Compiled XAML/XBF; not decompiled. Icon content is unconfirmed.", ExportBytes(data, offset, length, location)); return; }
            if (ext == ".pri" || (prefix.Length >= 7 && Encoding.ASCII.GetString(prefix, 0, 7) == "mrm_pri"))
            { Finding(location, "pri", "candidate", length, label, "Windows package resource index; presence alone does not establish vector icons. Use SDK MakePri dump for its index.", ExportBytes(data, offset, length, location)); return; }
            if (!resourcesMagic && !gzip && !xml)
            {
                if (ext == ".svg" || ext == ".svgz" || Regex.IsMatch(label, @"(?:^|/)(?:SVG|SVGZ)(?:/|$)", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant))
                    Finding(location, "svg-named-resource", "candidate", length, label, "Name suggests SVG, but XML content was not recognized. May be compressed, wrapped or invalid.", ExportBytes(data, offset, length, location));
                return;
            }
            if (length > options.MaxBytes) { Skip(location, "Asset exceeds --max-mb; content not inspected."); return; }
            byte[] bytes = data.Read(offset, (int)length);
            if (resourcesMagic) { ScanResources(bytes, location, depth + 1); return; }
            if (gzip)
            {
                try
                {
                    using (GZipStream input = new GZipStream(new MemoryStream(bytes, false), CompressionMode.Decompress))
                    using (MemoryStream output = new MemoryStream())
                    {
                        CopyBounded(input, output);
                        using (Data expanded = new Data(new MemoryStream(output.ToArray(), false))) ScanPayload(expanded, 0, expanded.Length, location + "/gzip", label, depth + 1);
                    }
                }
                catch (Exception ex) { if (!Recoverable(ex)) throw; Skip(location + "/gzip", ex.Message); }
                return;
            }
            ScanXml(bytes, location, label);
        }
        static string Decode(byte[] bytes)
        {
            if (bytes.Length >= 4 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0xfe && bytes[3] == 0xff) return new UTF32Encoding(true, true).GetString(bytes);
            if (bytes.Length >= 4 && bytes[0] == 0xff && bytes[1] == 0xfe && bytes[2] == 0 && bytes[3] == 0) return Encoding.UTF32.GetString(bytes);
            if (bytes.Length >= 2 && (bytes[0] == 0xff && bytes[1] == 0xfe || bytes.Length >= 4 && bytes[0] == '<' && bytes[1] == 0)) return Encoding.Unicode.GetString(bytes);
            if (bytes.Length >= 2 && (bytes[0] == 0xfe && bytes[1] == 0xff || bytes.Length >= 4 && bytes[0] == 0 && bytes[1] == '<')) return Encoding.BigEndianUnicode.GetString(bytes);
            return Encoding.UTF8.GetString(bytes);
        }
        void ScanResources(byte[] bytes, string location, int depth)
        {
            // Bound the allocation-driving counts before the framework reader
            // constructs its name/type tables from potentially damaged files.
            if (bytes.Length < 12) throw new InvalidDataException("Truncated .resources header.");
            long headerEnd = 12L + BitConverter.ToUInt32(bytes, 8);
            if (headerEnd + 12 > bytes.Length) throw new InvalidDataException("Invalid .resources header size.");
            int resourceCount = BitConverter.ToInt32(bytes, (int)headerEnd + 4), typeCount = BitConverter.ToInt32(bytes, (int)headerEnd + 8);
            if (resourceCount < 0 || resourceCount > 200000 || typeCount < 0 || typeCount > 200000)
                throw new InvalidDataException("Managed resource count limit exceeded.");
            // Enumerating keys and GetResourceData returns raw bytes and type
            // names. Never request enumerator.Value: it may deserialize objects.
            using (ResourceReader reader = new ResourceReader(new MemoryStream(bytes, false)))
            {
                System.Collections.IDictionaryEnumerator entries = reader.GetEnumerator();
                int count = 0;
                while (entries.MoveNext() && !cancelled)
                {
                    if (++count > 200000) { Skip(location, "Managed resource count limit exceeded."); break; }
                    string name = (string)entries.Key, type;
                    string child = location + "/" + name;
                    if (requestedLocations != null && !requestedLocations.Contains(child)) continue;
                    byte[] raw;
                    reader.GetResourceData(name, out type, out raw);
                    try
                    {
                        byte[] payload = raw;
                        using (BinaryReader value = new BinaryReader(new MemoryStream(raw, false), Encoding.UTF8))
                        {
                            if (type == "ResourceTypeCode.Stream" || type == "ResourceTypeCode.ByteArray")
                            {
                                int size = value.ReadInt32();
                                if (size < 0 || size > raw.Length - 4) throw new InvalidDataException("Invalid managed stream length.");
                                payload = value.ReadBytes(size);
                            }
                            else if (type == "ResourceTypeCode.String" || type.StartsWith("System.String,", StringComparison.Ordinal)) payload = Encoding.UTF8.GetBytes(value.ReadString());
                            else if (type.StartsWith("ResourceTypeCode.", StringComparison.Ordinal)) continue;
                            else
                            {
                                if (name.EndsWith(".baml", StringComparison.OrdinalIgnoreCase) || name.EndsWith(".xbf", StringComparison.OrdinalIgnoreCase))
                                    Finding(child, "compiled-resource", "candidate", raw.Length, name, "Custom serialized resource type " + type + "; not deserialized.", options.Export ? raw : null);
                                continue;
                            }
                        }
                        using (Data asset = new Data(new MemoryStream(payload, false))) ScanPayload(asset, 0, payload.Length, child, name, depth);
                    }
                    catch (Exception ex) { if (!Recoverable(ex)) throw; Skip(child, ex.Message); }
                }
            }
        }
        void ScanXml(byte[] bytes, string location, string label)
        {
            // Disable external entity resolution. Permit a DOCTYPE without
            // resolving it: old SVG files often name an external SVG DTD.
            XmlReaderSettings settings = new XmlReaderSettings { DtdProcessing = DtdProcessing.Ignore, XmlResolver = null, MaxCharactersInDocument = options.MaxBytes, MaxCharactersFromEntities = 1024 };
            try
            {
                using (XmlReader reader = XmlReader.Create(new MemoryStream(bytes, false), settings))
                {
                    XDocument doc = XDocument.Load(reader, LoadOptions.None);
                    XElement root = doc.Root;
                    if (root == null) return;
                    if (root.Name.LocalName == "svg" && (root.Name.NamespaceName.Length == 0 || root.Name.NamespaceName == "http://www.w3.org/2000/svg"))
                    {
                        Finding(location, "svg", "confirmed", bytes.Length, label, "SVG XML root; width=" + (string)root.Attribute("width") + "; height=" + (string)root.Attribute("height") + "; viewBox=" + (string)root.Attribute("viewBox"), bytes);
                        return;
                    }
                    bool xaml = root.Name.NamespaceName.Contains("schemas.microsoft.com/winfx/2006/xaml/presentation") ||
                        root.Name.NamespaceName.Contains("schemas.microsoft.com/winfx/2006/xaml") ||
                        root.Name.NamespaceName.Contains("schemas.microsoft.com/winfx/2009/xaml/presentation") ||
                        root.Name.NamespaceName == "https://github.com/avaloniaui";
                    if (xaml)
                    {
                        string[] shapes = root.DescendantsAndSelf().Where(e => VectorElements.Contains(e.Name.LocalName)).Select(e => e.Name.LocalName).Distinct().Take(12).ToArray();
                        string keys = string.Join("; ", root.DescendantsAndSelf().Attributes().Where(a => a.Name.LocalName == "Key" || a.Name.LocalName == "Name").Select(a => a.Value).Take(12));
                        Finding(location, shapes.Length != 0 ? "vector-xaml" : "xaml", shapes.Length != 0 ? "confirmed" : "candidate", bytes.Length, label + " " + keys,
                            "XAML root=" + root.Name.LocalName + "; vector elements=" + string.Join("/", shapes) + "; keys=" + keys + ". UI artwork is not necessarily the Shell app icon.", bytes);
                    }
                    foreach (XAttribute attribute in root.DescendantsAndSelf().Attributes())
                    {
                        string name = attribute.Name.LocalName;
                        bool package = root.Name.LocalName == "Package" && (name.IndexOf("Logo", StringComparison.OrdinalIgnoreCase) >= 0 || name == "Executable");
                        if (package) Finding(location + "/@" + name, "package-logo", "reference", bytes.Length, attribute.Value, name + "=" + attribute.Value + "; path is relative to the package, not proof of vector data.");
                        else if (attribute.Value.IndexOf(".svg", StringComparison.OrdinalIgnoreCase) >= 0)
                            Finding(location + "/@" + name, "svg-reference", "reference", bytes.Length, attribute.Value, "XML attribute names an SVG asset: " + attribute.Value);
                    }
                }
            }
            catch (XmlException ex) { Skip(location, "XML could not be parsed: " + ex.Message); }
        }
        void WriteSummary()
        {
            string state = failed ? "failed" : cancelled ? "interrupted" : limited ? "limited" : "finished";
            Dictionary<string, object> summary = new Dictionary<string, object> {
                {"State", state}, {"StartedUtc", DateTime.UtcNow.Subtract(elapsed.Elapsed).ToString("o", CultureInfo.InvariantCulture)},
                {"FinishedUtc", DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture)}, {"Roots", options.Roots},
                {"EntriesEncountered", encountered}, {"FilesInspected", inspected}, {"FilesWithFindings", matchedFiles},
                {"Findings", findings}, {"Skips", skipped}, {"Seconds", elapsed.Elapsed.TotalSeconds}, {"Kinds", kinds},
                {"MaxAssetBytes", options.MaxBytes}, {"IncludeZip", options.IncludeZip}, {"ExcludedPaths", options.Excludes},
                {"ExportEnabled", options.Export}, {"ExportedAssets", exported.Count}, {"ExportedBytes", exportedBytes}, {"MaxExportBytes", options.MaxExportBytes}, {"SvgOnly", options.SvgOnly}
            };
            summary["SourceCatalog"] = options.CatalogInput;
            summary["RequestedResources"] = requestedResources;
            summary["RequestedFiles"] = catalogTargets == null ? 0 : catalogTargets.Count;
            string self = System.Reflection.Assembly.GetExecutingAssembly().Location;
            using (System.Security.Cryptography.SHA256 hash = System.Security.Cryptography.SHA256.Create())
            using (Stream input = File.OpenRead(self))
                summary["ScannerSha256"] = BitConverter.ToString(hash.ComputeHash(input)).Replace("-", "");
            JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
            File.WriteAllText(Path.Combine(options.Output, "summary.json"), json.Serialize(summary), Encoding.UTF8);
            List<Dictionary<string, object>> report = options.Export ? exported.Values.OrderBy(e => KindRank((string)e["Kind"])).ToList() : examples;
            string description = options.Export ? exported.Count + " unique assets copied (" + exportedBytes + " bytes). Gallery shows each unique copy; SVG images have previews. Keep the assets folder beside this page." :
                "Up to 250 selected examples, favoring embedded SVG and vector XAML. Use --export to copy assets and show SVG previews.";
            string status = state + ": " + inspected + " files inspected, " + matchedFiles + " matching files, " + findings + " findings, " + skipped + " skips.";
            File.WriteAllText(Path.Combine(options.Output, "examples.html"), BuildGallery(report, status, description, options.SvgOnly), Encoding.UTF8);
            File.WriteAllText(Path.Combine(options.Output, "progress.txt"), "Catalog " + state + ": " + inspected + " inspected; " + findings + " findings; " + skipped + " skips." + Environment.NewLine, Encoding.UTF8);
            Console.WriteLine("Catalog " + state + ": " + matchedFiles + " matching files, " + findings + " findings, " + skipped + " skips.\n" + options.Output);
        }
        public static string BuildGallery(List<Dictionary<string, object>> report, string status, string description, bool svgOnly)
        {
            JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
            string rows = json.Serialize(report).Replace("<", "\\u003c").Replace(">", "\\u003e").Replace("&", "\\u0026");
            status = System.Security.SecurityElement.Escape(status);
            description = System.Security.SecurityElement.Escape(description);
            string reportTitle = svgOnly ? "Extracted SVG artwork" : "Advanced icon catalog";
            string reportNote = svgOnly ? "SVG artwork can include buttons and other graphics as well as application logos. Select the image you want, then use its saved SVG filename in a Launcher item's icon field." :
                "Compiled XAML may describe ordinary UI; artwork presence does not establish the app's Shell icon.";
            string html = @"<!doctype html>
<html lang=""en""><meta charset=""utf-8""><meta name=""viewport"" content=""width=device-width, initial-scale=1""><meta http-equiv=""Content-Security-Policy"" content=""default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src 'self' file: data:; object-src 'none'; base-uri 'none'; form-action 'none'""><title>Advanced icon catalog</title>
<style>
*{box-sizing:border-box}body{font:16px system-ui,sans-serif;margin:2rem;background:#fafafa;color:#222;line-height:1.45}h1{margin-bottom:.4rem}p{max-width:90rem}a{color:#075aa7}input,button{font:inherit;padding:.45rem .7rem}input{width:100%;margin:.3rem 0 1rem}button:disabled{opacity:.5}.legend{display:flex;gap:1.5rem;flex-wrap:wrap}.status{font-family:'Segoe UI Emoji','Apple Color Emoji',sans-serif;margin-right:.45em}.controls{display:flex;gap:1rem;align-items:center;flex-wrap:wrap;margin:1rem 0}.table-wrap{overflow-x:auto}table{table-layout:fixed;border-collapse:collapse;width:100%;min-width:760px;font-size:14px}td,th{padding:.8rem;text-align:left;border-bottom:1px solid #ccc;vertical-align:top}th{background:#eee}.kind{width:11rem;white-space:nowrap}.preview{width:150px}.evidence{width:27%;overflow-wrap:anywhere}.path{white-space:pre-wrap;overflow-wrap:anywhere;margin-bottom:.6rem}.location{color:#44545d}.thumbnail{width:120px;height:120px;object-fit:contain;padding:8px;background:repeating-conic-gradient(#e6e6e6 0% 25%,#fff 0% 50%) 0 / 16px 16px;border:1px solid #ccc;display:block;margin-bottom:.5rem}.note{color:#555;font-size:.9em}.sr-only{position:absolute;width:1px;height:1px;overflow:hidden;clip-path:inset(50%)}@media(max-width:700px){body{margin:1rem}}
</style>
<h1>" + reportTitle + "</h1><p>" + status + @"</p>
<p>" + description + @"</p>
<p>See <a href=""resources.csv"">resources.csv</a> for every finding and its asset link, <a href=""files.csv"">files.csv</a> for matching files, and <a href=""skips.csv"">skips.csv</a> for coverage gaps. " + reportNote + @"</p>
<p class=""legend""><span><span class=""status"">&#x2705;</span>Confirmed format</span><span><span class=""status"">&#x1f536;</span>Candidate</span><span><span class=""status"">&#x1f517;</span>Reference</span></p>
<label for=""filter"">Find by file, kind or resource name</label><input id=""filter"" type=""search""><div class=""controls""><button id=""previous"">Previous</button><span id=""count"" role=""status""></span><button id=""next"">Next</button></div>
<div class=""table-wrap""><table><thead><tr><th class=""kind"">Kind</th><th class=""preview"">View / copy</th><th>File / resource</th><th class=""evidence"">Evidence</th></tr></thead><tbody id=""rows""></tbody></table></div>
<script>
const data=" + rows + @";
const labels={'svg':'SVG','vector-xaml':'XAML drawing','baml':'BAML','xbf':'XBF','pri':'PRI','xaml':'XAML','svg-named-resource':'SVG name','compiled-resource':'Serialized data','svg-reference':'SVG reference','package-logo':'Logo reference','linked-resource':'Linked resource'};
const symbols={'confirmed':'\u2705','candidate':'\ud83d\udd36','reference':'\ud83d\udd17'};
let page=0;const pageSize=100,byId=id=>document.getElementById(id);
const searchable=data.map(x=>JSON.stringify(x).toLowerCase());
function element(tag,text,cls){const e=document.createElement(tag);if(text!==undefined)e.textContent=text;if(cls)e.className=cls;return e}
function render(){
 const query=byId('filter').value.toLowerCase(),filtered=data.filter((x,i)=>searchable[i].includes(query));
 page=Math.min(page,Math.max(0,Math.ceil(filtered.length/pageSize)-1));
 const start=page*pageSize,end=Math.min(start+pageSize,filtered.length),body=byId('rows');body.replaceChildren();
 for(const x of filtered.slice(start,end)){
  const tr=element('tr'),kind=element('td',undefined,'kind'),icon=element('span',symbols[x.Confidence]||'','status');
  icon.setAttribute('aria-label',x.Confidence);kind.append(icon,document.createTextNode(labels[x.Kind]||x.Kind));tr.append(kind);
  const preview=element('td',undefined,'preview');
  if(/^assets\/(?:[^/\\:]{1,48}--)?[a-f0-9]{64}\.(svg|xaml|baml|xbf|pri|bin)$/.test(x.ExportedFile)){
   if(x.Kind==='svg'){const img=element('img',undefined,'thumbnail');img.src=x.ExportedFile;img.alt='SVG preview';img.loading='lazy';preview.append(img)}
   const link=element('a','Save '+x.ExportedFile.split('.').pop().toUpperCase());link.href=x.ExportedFile;link.download=x.ExportedFile.split('/').pop();preview.append(link);
   if(x.Kind!=='svg')preview.append(element('p',x.Kind==='xaml'||x.Kind==='vector-xaml'?'Markup copy':'Raw resource copy','note'));
  }else preview.append(element('span','No exported copy','note'));
  tr.append(preview);const source=element('td');source.append(element('div',x.File,'path'));
  if(x.Member)source.append(element('div',x.Member,'path location'));source.append(element('div',x.Location,'path location'));tr.append(source,element('td',x.Evidence,'evidence'));body.append(tr);
 }
 byId('count').textContent=filtered.length?(start+1)+'\u2013'+end+' of '+filtered.length:'No matching entries';
 byId('previous').disabled=page===0;byId('next').disabled=end>=filtered.length;
}
byId('filter').addEventListener('input',()=>{page=0;render()});byId('previous').addEventListener('click',()=>{--page;render()});byId('next').addEventListener('click',()=>{++page;render()});render();
</script></html>";
            return html;
        }
    }

    static class Program
    {
        public static int Main(string[] args)
        {
            // Modern .NET Framework path handling supports archive trees with
            // paths beyond MAX_PATH. These affect only this scanner process.
            AppContext.SetSwitch("Switch.System.IO.UseLegacyPathHandling", false);
            AppContext.SetSwitch("Switch.System.IO.BlockLongPaths", false);
            if (args.Length == 0 || args.Contains("--help"))
            {
                Console.WriteLine("AdvancedIconCatalog [roots... | --all-drives | --from-catalog <resources.csv>]\n  --output <new-directory> [--include-zip] [--export] [--svg-only] [--max-export-mb 512]\n  [--exclude <directory>] [--max-mb 32] [--max-files N] [--max-seconds N]\n  [--cancel-file <path>]\nDefault cancel file: <output>/cancel.request\nRead-only SVG/XAML/compiled-XAML/PRI inventory, with optional asset copies and SVG previews.\n--from-catalog opens only the cataloged files/resources, without directory traversal.\n--svg-only records SVG only and gives copies sanitized descriptive basenames plus a hash.\nNo target code is run or loaded.\nExit codes: 0 finished (consult skips.csv), 2 explicit limit, 130 Ctrl+C/cancel file, 1 failure.");
                return 0;
            }
            try { using (Catalog catalog = new Catalog(Options.Parse(args))) return catalog.Run(); }
            catch (Exception ex) { Console.Error.WriteLine(ex.GetType().Name + ": " + ex.Message); return 1; }
        }
    }
}
