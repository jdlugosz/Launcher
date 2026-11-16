using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;

namespace LauncherTools
{
    // A loopback-only bridge lets the gallery ask this process to keep an SVG.
    // The browser never receives general filesystem access. All routes require
    // a random capability, and writes accept only cataloged, hash-verified SVGs.
    sealed class SvgGalleryServer : IDisposable
    {
        sealed class Asset { public string Relative, Hash, Kind; }
        readonly Dictionary<string, Asset> assets = new Dictionary<string, Asset>(StringComparer.Ordinal);
        readonly TcpListener listener = new TcpListener(IPAddress.Loopback, 0);
        readonly string root, keepDirectory, prefix, authority;
        readonly string galleryHtml;
        readonly object clientLock = new object();
        readonly HashSet<TcpClient> clients = new HashSet<TcpClient>();
        readonly SemaphoreSlim slots = new SemaphoreSlim(8);
        volatile bool disposed;
        public string Url { get; private set; }
        public SvgGalleryServer(string directory, string destination)
        {
            root = Path.GetFullPath(directory);
            keepDirectory = Path.GetFullPath(destination);
            List<Dictionary<string, object>> report = new List<Dictionary<string, object>>();
            if (!File.Exists(Path.Combine(root, "examples.html"))) throw new IOException("Choose a results folder containing examples.html and resources.csv.");
            using (IEnumerator<string[]> input = Csv.Read(Path.Combine(root, "resources.csv")).GetEnumerator())
            {
                if (!input.MoveNext()) throw new InvalidDataException("The resource catalog is empty.");
                string[] header = input.Current;
                int kind = Array.IndexOf(header, "Kind"), file = Array.IndexOf(header, "ExportedFile"), hash = Array.IndexOf(header, "AssetSha256");
                if (kind < 0 || file < 0 || hash < 0) throw new InvalidDataException("This catalog has no exported assets. Run an extraction first.");
                int required = Math.Max(kind, Math.Max(file, hash));
                while (input.MoveNext())
                {
                    string[] row = input.Current;
                    if (row.Length <= required || row[file].Length == 0) continue;
                    if (!Regex.IsMatch(row[file], @"\Aassets/(?:[\p{L}\p{Nd}_-]{1,48}--)?[a-f0-9]{64}\.(?:svg|xaml|baml|xbf|pri|bin)\z") ||
                        !Regex.IsMatch(row[hash], @"\A[a-fA-F0-9]{64}\z")) throw new InvalidDataException("The catalog contains an invalid SVG copy path or hash.");
                    if (row[kind] == "svg" && !row[file].EndsWith(".svg", StringComparison.Ordinal)) throw new InvalidDataException("The SVG copy has an invalid extension.");
                    Asset previous;
                    if (assets.TryGetValue(row[file], out previous))
                    {
                        if (!string.Equals(previous.Hash, row[hash], StringComparison.OrdinalIgnoreCase) || previous.Kind != row[kind]) throw new InvalidDataException("The catalog has conflicting entries for one copy.");
                        continue;
                    }
                    assets.Add(row[file], new Asset { Relative = row[file], Hash = row[hash], Kind = row[kind] });
                    Func<string, string> value = key => { int index = Array.IndexOf(header, key); return index < 0 || index >= row.Length ? "" : row[index]; };
                    report.Add(new Dictionary<string, object> {
                        {"File", value("File")}, {"Member", value("Member")}, {"Location", value("Location")},
                        {"Kind", row[kind]}, {"Confidence", value("Confidence")}, {"Evidence", value("Evidence")},
                        {"ExportedFile", row[file]}, {"AssetSha256", row[hash]}
                    });
                }
            }
            // Rebuild from CSV with our own renderer. A saved HTML file never
            // supplies executable script to the native Keep bridge.
            galleryHtml = Catalog.BuildGallery(report.OrderBy(e => (string)e["Kind"] == "svg" ? 0 : 1).ToList(), "Saved gallery: " + report.Count + " unique exported assets.",
                "These copies are already extracted. The source program or archive need not remain available.", report.All(e => (string)e["Kind"] == "svg"))
                .Replace("style-src 'unsafe-inline'; img-src", "style-src 'unsafe-inline'; connect-src 'self'; img-src")
                .Replace("</html>", KeepScript + "</html>");
            byte[] token = new byte[24];
            using (RandomNumberGenerator random = RandomNumberGenerator.Create()) random.GetBytes(token);
            prefix = "/" + BitConverter.ToString(token).Replace("-", "").ToLowerInvariant() + "/";
            listener.Start();
            authority = "127.0.0.1:" + ((IPEndPoint)listener.LocalEndpoint).Port.ToString(CultureInfo.InvariantCulture);
            Url = "http://" + authority + prefix;
            Task.Run((Action)AcceptClients);
        }
        public void Dispose()
        {
            disposed = true;
            listener.Stop();
            lock (clientLock) foreach (TcpClient client in clients) client.Close();
        }
        void AcceptClients()
        {
            while (!disposed)
            {
                slots.Wait();
                if (disposed) { slots.Release(); return; }
                TcpClient client;
                try { client = listener.AcceptTcpClient(); }
                catch (SocketException) { slots.Release(); if (disposed) return; throw; }
                catch (ObjectDisposedException) { slots.Release(); return; }
                lock (clientLock) { if (disposed) { client.Close(); slots.Release(); return; } clients.Add(client); }
                Task.Run(() =>
                {
                    try { using (client) Serve(client); }
                    catch (IOException) { }
                    catch (SocketException) { }
                    catch (ObjectDisposedException) { }
                    finally { lock (clientLock) clients.Remove(client); slots.Release(); }
                });
            }
        }
        static string Line(Stream input, ref int remaining)
        {
            StringBuilder result = new StringBuilder();
            int c;
            while ((c = input.ReadByte()) >= 0)
            {
                if (--remaining < 0) throw new InvalidDataException("Request headers are too large.");
                if (c == '\n') return result.ToString().TrimEnd('\r');
                result.Append((char)c);
            }
            throw new EndOfStreamException();
        }
        void Serve(TcpClient client)
        {
            JavaScriptSerializer json = Serializer();
            client.ReceiveTimeout = client.SendTimeout = 5000;
            using (NetworkStream network = client.GetStream())
            using (BufferedStream input = new BufferedStream(network, 8192))
            {
                try
                {
                    int remaining = 16384;
                    string[] request = Line(input, ref remaining).Split(' ');
                    if (request.Length != 3 || request[2] != "HTTP/1.1") { Reply(network, 400, "Invalid request."); return; }
                    Dictionary<string, string> headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                    string line;
                    while ((line = Line(input, ref remaining)).Length != 0)
                    {
                        int colon = line.IndexOf(':');
                        if (colon < 1 || headers.ContainsKey(line.Substring(0, colon))) { Reply(network, 400, "Invalid request headers."); return; }
                        headers.Add(line.Substring(0, colon), line.Substring(colon + 1).Trim());
                    }
                    string host;
                    if (!headers.TryGetValue("Host", out host) || host != authority || !request[1].StartsWith(prefix, StringComparison.Ordinal)) { Reply(network, 403, "This gallery address is not valid."); return; }
                    string route = Uri.UnescapeDataString(request[1].Substring(prefix.Length));
                    if (request[0] == "POST" && route == "keep")
                    {
                        string origin, contentType, length;
                        int size;
                        if (!headers.TryGetValue("Origin", out origin) || origin != "http://" + authority) { Reply(network, 403, "Keep is available only from this gallery."); return; }
                        if (!headers.TryGetValue("Content-Type", out contentType) || !contentType.StartsWith("application/json", StringComparison.OrdinalIgnoreCase) ||
                            headers.ContainsKey("Transfer-Encoding") || !headers.TryGetValue("Content-Length", out length) || !int.TryParse(length, out size) || size < 1 || size > 4096) { Reply(network, 400, "Invalid Keep request."); return; }
                        byte[] body = new byte[size];
                        int offset = 0;
                        while (offset < size) { int count = input.Read(body, offset, size - offset); if (count == 0) throw new EndOfStreamException(); offset += count; }
                        Dictionary<string, string> values = json.Deserialize<Dictionary<string, string>>(Encoding.UTF8.GetString(body));
                        string asset;
                        if (values == null || !values.TryGetValue("asset", out asset) || asset == null || !assets.ContainsKey(asset) || assets[asset].Kind != "svg") { Reply(network, 404, "That SVG is not in this gallery."); return; }
                        string kept = Keep(assets[asset]);
                        Write(network, 200, "application/json; charset=utf-8", Encoding.UTF8.GetBytes(json.Serialize(new { path = kept })), false);
                        return;
                    }
                    if (request[0] != "GET") { Reply(network, 405, "Unsupported request."); return; }
                    if (route.Length == 0 || route == "examples.html")
                    {
                        Write(network, 200, "text/html; charset=utf-8", Encoding.UTF8.GetBytes(galleryHtml), false);
                    }
                    else if (assets.ContainsKey(route)) Write(network, 200, assets[route].Kind == "svg" ? "image/svg+xml" : "application/octet-stream", ReadAsset(assets[route]), assets[route].Kind == "svg");
                    else if (new[] { "resources.csv", "files.csv", "skips.csv", "summary.json" }.Contains(route))
                        Write(network, 200, route.EndsWith(".csv", StringComparison.Ordinal) ? "text/csv; charset=utf-8" : "application/json; charset=utf-8", File.ReadAllBytes(Path.Combine(root, route)), false);
                    else Reply(network, 404, "Not found.");
                }
                catch (InvalidDataException ex) { Reply(network, 400, ex.Message); }
                catch (ArgumentException) { Reply(network, 400, "Invalid request data."); }
                catch (UnauthorizedAccessException ex) { Reply(network, 500, ex.Message); }
                catch (IOException ex) { Reply(network, 500, ex.Message); }
            }
        }
        byte[] ReadAsset(Asset asset)
        {
            string path = Path.Combine(root, asset.Relative);
            using (FileStream file = File.OpenRead(path))
            {
                if (file.Length > 32 * 1024 * 1024) throw new InvalidDataException("The SVG exceeds the 32 MiB limit.");
                byte[] bytes = new byte[(int)file.Length];
                int offset = 0;
                while (offset < bytes.Length) { int n = file.Read(bytes, offset, bytes.Length - offset); if (n == 0) throw new EndOfStreamException(); offset += n; }
                using (SHA256 hash = SHA256.Create())
                    if (!string.Equals(BitConverter.ToString(hash.ComputeHash(bytes)).Replace("-", ""), asset.Hash, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("The exported SVG has changed since it was cataloged. Extract it again before keeping it.");
                return bytes;
            }
        }
        string Keep(Asset asset)
        {
            byte[] bytes = ReadAsset(asset);
            Directory.CreateDirectory(keepDirectory);
            string target = Path.Combine(keepDirectory, Path.GetFileName(asset.Relative));
            if (File.Exists(target)) { CheckExisting(target, asset.Hash); return target; }
            string temporary = Path.Combine(keepDirectory, ".svg-" + Guid.NewGuid().ToString("N") + ".tmp");
            try
            {
                using (FileStream file = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write)) file.Write(bytes, 0, bytes.Length);
                try { File.Move(temporary, target); }
                catch (IOException) { if (!File.Exists(target)) throw; CheckExisting(target, asset.Hash); }
            }
            finally { if (File.Exists(temporary)) File.Delete(temporary); }
            return target;
        }
        static void CheckExisting(string path, string expected)
        {
            using (Stream file = File.OpenRead(path))
            using (SHA256 hash = SHA256.Create())
                if (!string.Equals(BitConverter.ToString(hash.ComputeHash(file)).Replace("-", ""), expected, StringComparison.OrdinalIgnoreCase)) throw new IOException("A different file already has this name in Launcher Icons; it has been preserved.");
        }
        void Reply(Stream output, int status, string message)
        {
            Write(output, status, "application/json; charset=utf-8", Encoding.UTF8.GetBytes(Serializer().Serialize(new { error = message })), false);
        }
        static JavaScriptSerializer Serializer() { return new JavaScriptSerializer { MaxJsonLength = 4096, RecursionLimit = 8 }; }
        static void Write(Stream output, int status, string type, byte[] bytes, bool svg)
        {
            string policy = svg ? "sandbox; default-src 'none'; style-src 'unsafe-inline'; img-src data:" :
                "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'";
            string header = "HTTP/1.1 " + status.ToString(CultureInfo.InvariantCulture) + " " + (status == 200 ? "OK" : "Error") + "\r\nContent-Type: " + type + "\r\nContent-Length: " + bytes.Length.ToString(CultureInfo.InvariantCulture) +
                "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: " + policy + "\r\n\r\n";
            byte[] encoded = Encoding.ASCII.GetBytes(header);
            output.Write(encoded, 0, encoded.Length); output.Write(bytes, 0, bytes.Length); output.Flush();
        }
        const string KeepScript = @"<script>
(()=>{
 const remembered=new Map(),body=document.getElementById('rows');
 const note=document.createElement('p');note.textContent='Keep copies an SVG into Launcher\u2019s Icons folder. Leave the extractor open while choosing images.';body.closest('table').parentElement.before(note);
 function attach(){
  for(const link of body.querySelectorAll('a[download]')){
   const asset=link.getAttribute('href');if(!asset.endsWith('.svg')||link.dataset.keepAdded)continue;link.dataset.keepAdded='yes';
   const button=document.createElement('button'),message=document.createElement('div');button.textContent=remembered.has(asset)?'Kept':'Keep';button.disabled=remembered.has(asset);button.style.marginTop='.5rem';message.className='note';
   if(remembered.has(asset))message.textContent=remembered.get(asset);link.after(button,message);
   button.addEventListener('click',async()=>{
    button.disabled=true;message.textContent='Keeping...';
    try{const response=await fetch('keep',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({asset})});const result=await response.json();if(!response.ok)throw new Error(result.error||'Could not keep this SVG.');button.textContent='Kept';message.textContent=result.path;remembered.set(asset,result.path)}
    catch(error){button.disabled=false;message.textContent=error.message+' Leave the extractor open, or reopen this gallery from it.'}
   });
  }
 }
 new MutationObserver(attach).observe(body,{childList:true,subtree:true});attach();
})();
</script>";
    }
}
