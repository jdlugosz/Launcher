[CmdletBinding()]
param([string] $OutputDirectory)
$ErrorActionPreference = 'Stop'
$catalogTestProject = Split-Path -Parent $PSScriptRoot
if (!$OutputDirectory) { $OutputDirectory = Join-Path $catalogTestProject ('build\icon-catalog-tests\' + [guid]::NewGuid().ToString('N')) }
$catalogTestRoot = [IO.Path]::GetFullPath($OutputDirectory)
$null = New-Item -ItemType Directory -Path $catalogTestRoot
$catalogTestExe = & (Join-Path $catalogTestProject 'tools\scan-advanced-icons.ps1') -BuildOnly
$catalogTestCompiler = Join-Path ([Environment]::GetFolderPath('Windows')) 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$catalogTestBuilder = Join-Path $catalogTestRoot 'FixtureBuilder.cs'
@'
using System;
using System.IO;
using System.IO.Compression;
using System.Resources;
using System.Runtime.Serialization;
using System.Text;
[Serializable] public class Payload : ISerializable {
    readonly string marker;
    public Payload(string path) { marker = path; }
    protected Payload(SerializationInfo info, StreamingContext context) { File.WriteAllText(info.GetString("marker"), "deserialized"); }
    public void GetObjectData(SerializationInfo info, StreamingContext context) { info.AddValue("marker", marker); }
}
class FixtureBuilder {
    static void Text(BinaryWriter w, string text) { foreach(char c in text) w.Write((ushort)c); w.Write((ushort)0); }
    static void Align(BinaryWriter w) { while(w.BaseStream.Position % 4 != 0) w.Write((byte)0); }
    static void Resource(BinaryWriter w, string type, string name, byte[] bytes) {
        int header = (8 + (type.Length + name.Length + 2) * 2 + 3) & ~3;
        w.Write(bytes.Length); w.Write(header + 16); Text(w,type); Text(w,name); Align(w);
        w.Write(0); w.Write((ushort)0x30); w.Write((ushort)1033); w.Write(0); w.Write(0); w.Write(bytes); Align(w);
    }
    public static void Main(string[] args) {
        AppContext.SetSwitch("Switch.System.IO.UseLegacyPathHandling",false);
        AppContext.SetSwitch("Switch.System.IO.BlockLongPaths",false);
        string root=args[0]; Directory.CreateDirectory(root);
        string svg="<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'><path d='M0 0 L32 32'/></svg>";
        string xaml="<ResourceDictionary xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml'><DrawingImage x:Key='AppIcon'><DrawingImage.Drawing><GeometryDrawing Brush='Red' Geometry='M0,0 L10,10'/></DrawingImage.Drawing></DrawingImage></ResourceDictionary>";
        File.WriteAllText(Path.Combine(root,"logo.svg"),svg,new UTF8Encoding(false));
        File.WriteAllText(Path.Combine(root,"vector.xaml"),xaml,new UTF8Encoding(false));
        File.WriteAllText(Path.Combine(root,"doctype.svg"),"<!DOCTYPE svg SYSTEM 'file:///not-a-file.dtd'>"+svg);
        File.WriteAllText(Path.Combine(root,"ordinary.xml"),"<configuration><value>not an icon</value></configuration>");
        string deep=Path.Combine(root,new string('a',140),new string('b',140));Directory.CreateDirectory(deep);
        File.WriteAllText(Path.Combine(deep,"long-path-logo.svg"),svg);
        File.WriteAllText(Path.Combine(root,"broken.svg"),"<svg><broken>");
        File.WriteAllBytes(Path.Combine(root,"broken.dll"), new byte[]{77,90,0,0});
        File.WriteAllBytes(Path.Combine(root,"vector.xbf"),new byte[]{88,66,70,0,1});
        File.WriteAllBytes(Path.Combine(root,"resources.pri"),Encoding.ASCII.GetBytes("mrm_pri2test"));
        byte[] baml=Encoding.Unicode.GetBytes("MSBAML");
        using(ResourceWriter w=new ResourceWriter(Path.Combine(root,"Assets.g.resources"))) {
            w.AddResource("images/logo.svg",svg);
            w.AddResource("weird:<logo>.svg",svg);
            w.AddResource("comma,quote\"and\nnewline.svg",svg);
            w.AddResource("../../outside:icon?.svg",svg.Replace("32", "48"));
            w.AddResource("images/\u56fe\u6807.svg",svg.Replace("32", "64"));
            w.AddResource(">>Name:Type", "ordinary localized string");
            w.AddResource("images/vector.xaml",new MemoryStream(Encoding.UTF8.GetBytes(xaml)));
            w.AddResource("ui/window.baml",new MemoryStream(baml));
            w.AddResource("opaque",new Payload(Path.Combine(root,"DESERIALIZED.txt")));
            w.AddResource("opaque.baml",new Payload(Path.Combine(root,"DESERIALIZED.txt")));
        }
        using(BinaryWriter w=new BinaryWriter(File.Create(Path.Combine(root,"native.res")))) {
            w.Write(0); w.Write(32); w.Write((ushort)0xffff);w.Write((ushort)0);w.Write((ushort)0xffff);w.Write((ushort)0);w.Write(0);w.Write(0);w.Write(0);w.Write(0);
            Resource(w,"SVG","APP_LOGO",Encoding.UTF8.GetBytes(svg));
            Resource(w,"SVG","PACKED_LOGO",new byte[]{1,2,3,4});
            Resource(w,"CUSTOM","UTF16_LOGO",Encoding.Unicode.GetPreamble().Concat(Encoding.Unicode.GetBytes(svg)));
            using(MemoryStream compressed=new MemoryStream()) {
                using(GZipStream gz=new GZipStream(compressed,CompressionMode.Compress,true)) { byte[] b=Encoding.UTF8.GetBytes(svg);gz.Write(b,0,b.Length); }
                Resource(w,"RCDATA","GZIP_LOGO",compressed.ToArray());
            }
        }
        using(FileStream output=File.Create(Path.Combine(root,"package.msix")))
        using(ZipArchive zip=new ZipArchive(output,ZipArchiveMode.Create)) {
            Action<string,string> add=(name,text)=> { using(StreamWriter w=new StreamWriter(zip.CreateEntry(name).Open()))w.Write(text); };
            add("Assets/Logo.svg",svg);
            add("=1+1.svg",svg);
            add("AppxManifest.xml","<Package xmlns='http://schemas.microsoft.com/appx/manifest/foundation/windows10'><Applications><Application Executable='Demo.exe'><VisualElements Square44x44Logo='Assets/Logo.svg'/></Application></Applications></Package>");
            using(MemoryStream nested=new MemoryStream()) {
                using(ZipArchive inner=new ZipArchive(nested,ZipArchiveMode.Create,true))
                using(StreamWriter w=new StreamWriter(inner.CreateEntry("nested-logo.svg").Open()))w.Write(svg);
                using(Stream member=zip.CreateEntry("nested.zip").Open()) { byte[] b=nested.ToArray();member.Write(b,0,b.Length); }
            }
        }
    }
}
static class Bytes { public static byte[] Concat(this byte[] a,byte[] b) { byte[] c=new byte[a.Length+b.Length];Buffer.BlockCopy(a,0,c,0,a.Length);Buffer.BlockCopy(b,0,c,a.Length,b.Length);return c; } }
'@ | Set-Content -LiteralPath $catalogTestBuilder -Encoding UTF8
$catalogFixtureBuilderExe = Join-Path $catalogTestRoot 'FixtureBuilder.exe'
& $catalogTestCompiler /nologo /optimize+ /warnaserror+ "/out:$catalogFixtureBuilderExe" "/win32manifest:$(Join-Path $catalogTestProject 'tools\advanced-icon-catalog\app.manifest')" /reference:System.IO.Compression.dll $catalogTestBuilder
if ($LASTEXITCODE -ne 0) { throw 'Fixture builder compilation failed' }
$catalogFixtures = Join-Path $catalogTestRoot 'fixtures'
& $catalogFixtureBuilderExe $catalogFixtures
if ($LASTEXITCODE -ne 0) { throw 'Fixture generation failed' }
$catalogFixtureSource = Join-Path $catalogTestRoot 'Uninstalled.cs'
'public static class Uninstalled { public static void Unused() { throw new System.Exception("Do not execute target code."); } }' | Set-Content -LiteralPath $catalogFixtureSource -Encoding ASCII
foreach ($catalogCpu in @('x86', 'x64')) {
    & $catalogTestCompiler /nologo /target:library "/platform:$catalogCpu" "/out:$(Join-Path $catalogFixtures "Uninstalled-$catalogCpu.dll")" `
        "/resource:$(Join-Path $catalogFixtures 'Assets.g.resources'),Demo.g.resources" `
        "/resource:$(Join-Path $catalogFixtures 'logo.svg'),Demo.Brand.svg" `
        "/win32res:$(Join-Path $catalogFixtures 'native.res')" $catalogFixtureSource
    if ($LASTEXITCODE -ne 0) { throw 'Fixture assembly compilation failed' }
}
Copy-Item -LiteralPath (Join-Path $catalogFixtures 'Uninstalled-x64.dll') -Destination (Join-Path $catalogFixtures 'native.mun')
Copy-Item -LiteralPath (Join-Path $catalogFixtures 'Uninstalled-x64.dll') -Destination (Join-Path $catalogFixtures 'plugin.api')
$catalogResult = Join-Path $catalogTestRoot 'results'
& $catalogTestExe $catalogFixtures --output $catalogResult --include-zip
if ($LASTEXITCODE -ne 0) { throw 'Fixture scan failed' }
$catalogFindings = @(Import-Csv -LiteralPath (Join-Path $catalogResult 'resources.csv'))
foreach ($catalogCpu in @('x86', 'x64')) {
    $catalogAssemblyFindings = @($catalogFindings | Where-Object { $_.File.EndsWith("Uninstalled-$catalogCpu.dll") })
    foreach ($catalogExpected in @('PE/SVG/APP_LOGO/#1033', 'PE/CUSTOM/UTF16_LOGO/#1033', 'PE/RCDATA/GZIP_LOGO/#1033/gzip',
        'CLR/Demo.Brand.svg', 'CLR/Demo.g.resources/images/logo.svg', 'CLR/Demo.g.resources/images/vector.xaml', 'CLR/Demo.g.resources/ui/window.baml', 'CLR/Demo.g.resources/weird:<logo>.svg')) {
        if (!($catalogAssemblyFindings | Where-Object Location -EQ $catalogExpected)) { throw "Missing $catalogCpu finding: $catalogExpected" }
    }
}
if (($catalogFindings | Where-Object { $_.File.EndsWith('ordinary.xml') }).Count) { throw 'Ordinary XML was misclassified' }
if (!($catalogFindings | Where-Object { $_.File.EndsWith('doctype.svg') -and $_.Kind -eq 'svg' })) { throw 'DTD-bearing SVG was missed' }
if (!($catalogFindings | Where-Object { $_.File.EndsWith('long-path-logo.svg') -and $_.File.Length -gt 260 -and $_.Kind -eq 'svg' })) { throw 'Long archive path was missed' }
if (!($catalogFindings | Where-Object { $_.Member -eq 'Assets/Logo.svg' -and $_.Kind -eq 'svg' })) { throw 'Package SVG was missed' }
if (!($catalogFindings | Where-Object { $_.Member -eq "'=1+1.svg" -and $_.Kind -eq 'svg' })) { throw 'Spreadsheet formula-like member name was not kept as text' }
foreach ($catalogExtraFile in @('native.mun','plugin.api')) {
    if (!($catalogFindings | Where-Object { $_.File.EndsWith($catalogExtraFile) -and $_.Kind -eq 'svg' -and $_.Location -like 'PE/*' })) { throw "PE plugin/resource file was missed: $catalogExtraFile" }
}
if (!($catalogFindings | Where-Object { $_.Kind -eq 'package-logo' -and $_.Evidence -like 'Square44x44Logo=*' })) { throw 'Package app-logo reference was missed' }
if (!($catalogFindings | Where-Object { $_.Kind -eq 'baml' -and $_.Confidence -eq 'candidate' })) { throw 'Compiled XAML candidate was missed' }
if (!($catalogFindings | Where-Object { $_.Kind -eq 'svg-named-resource' -and $_.Confidence -eq 'candidate' })) { throw 'Opaque SVG-named resource was missed' }
if (Test-Path -LiteralPath (Join-Path $catalogFixtures 'DESERIALIZED.txt')) { throw 'Scanner deserialized an object' }
$catalogSkips = @(Import-Csv -LiteralPath (Join-Path $catalogResult 'skips.csv'))
if (!($catalogSkips | Where-Object { $_.File.EndsWith('broken.svg') })) { throw 'Malformed XML was not reported' }
if (!($catalogSkips | Where-Object { $_.File.EndsWith('broken.dll') })) { throw 'Malformed PE was not reported' }
if ($catalogSkips | Where-Object { $_.Reason -like '*Illegal characters*' }) { throw 'Resource keys were treated as filesystem paths' }
$catalogExport = Join-Path $catalogTestRoot 'exported'
& $catalogTestExe --from-catalog (Join-Path $catalogResult 'resources.csv') --export --output $catalogExport
if ($LASTEXITCODE -ne 0) { throw 'CSV-targeted export failed' }
$catalogExportSummary = Get-Content -LiteralPath (Join-Path $catalogExport 'summary.json') -Raw | ConvertFrom-Json
$catalogTargetCount = @($catalogFindings.File | Sort-Object -Unique).Count
if ($catalogExportSummary.FilesInspected -ne $catalogTargetCount -or $catalogExportSummary.EntriesEncountered -ne 0) { throw 'CSV mode did not inspect exactly the cataloged physical files' }
$catalogExportRows = @(Import-Csv -LiteralPath (Join-Path $catalogExport 'resources.csv'))
if ($catalogExportRows.Count -ne $catalogFindings.Count) { throw 'CSV round-trip lost a resource (including nested ZIP, comma, quote or newline names)' }
if (!(($catalogExportRows | Where-Object Member -EQ 'nested.zip!/nested-logo.svg').ExportedFile)) { throw 'Targeted export did not find nested general ZIP member' }
if (!(($catalogExportRows | Where-Object Member -EQ "'=1+1.svg").ExportedFile)) { throw 'Formula-protected member did not round-trip' }
if (@(Import-Csv -LiteralPath (Join-Path $catalogExport 'skips.csv')).Count) { throw 'Unexpected targeted export skips' }
foreach ($catalogRow in $catalogExportRows | Where-Object ExportedFile) {
    $catalogCopiedPath = Join-Path $catalogExport $catalogRow.ExportedFile
    if ($catalogRow.ExportedFile -notmatch '^assets/[a-f0-9]{64}\.(svg|xaml|baml|xbf|pri|bin)$' -or
        (Get-FileHash -LiteralPath $catalogCopiedPath).Hash -ne $catalogRow.AssetSha256) { throw 'Unsafe export filename or incorrect asset hash' }
}
$catalogLogoHash = (Get-FileHash -LiteralPath (Join-Path $catalogFixtures 'logo.svg')).Hash
foreach ($catalogLocator in @('PE/SVG/APP_LOGO/#1033', 'PE/RCDATA/GZIP_LOGO/#1033/gzip', 'CLR/Demo.Brand.svg', 'CLR/Demo.g.resources/images/logo.svg')) {
    if (@($catalogExportRows | Where-Object { $_.Location -eq $catalogLocator -and $_.AssetSha256 -eq $catalogLogoHash }).Count -ne 4) { throw "SVG bytes changed during export: $catalogLocator" }
}
$catalogUnicodeRow = $catalogExportRows | Where-Object Location -EQ 'PE/CUSTOM/UTF16_LOGO/#1033' | Select-Object -First 1
$catalogUnicodeBytes = [IO.File]::ReadAllBytes((Join-Path $catalogExport $catalogUnicodeRow.ExportedFile))
if ($catalogUnicodeBytes[0] -ne 255 -or $catalogUnicodeBytes[1] -ne 254) { throw 'Native UTF-16 SVG lost its BOM' }
foreach ($catalogFormat in @('vector-xaml', 'baml', 'xbf', 'pri', 'compiled-resource')) {
    if (!($catalogExportRows | Where-Object { $_.Kind -eq $catalogFormat -and $_.ExportedFile })) { throw "No raw export for $catalogFormat" }
}
foreach ($catalogLoose in @('vector.xaml','vector.xbf','resources.pri')) {
    $catalogOriginalHash = (Get-FileHash -LiteralPath (Join-Path $catalogFixtures $catalogLoose)).Hash
    if (!($catalogExportRows | Where-Object { $_.File.EndsWith($catalogLoose) -and $_.AssetSha256 -eq $catalogOriginalHash })) { throw "Raw bytes changed: $catalogLoose" }
}
if ($catalogExportRows | Where-Object { $_.Confidence -eq 'reference' -and $_.ExportedFile }) { throw 'References should not export nonexistent payloads' }
if (@($catalogExportRows | Where-Object AssetSha256 -EQ $catalogLogoHash | Select-Object -ExpandProperty ExportedFile -Unique).Count -ne 1) { throw 'Identical assets were not deduplicated' }
if (Test-Path -LiteralPath (Join-Path $catalogFixtures 'DESERIALIZED.txt')) { throw 'Export deserialized a target object' }
$catalogSvgOutput = Join-Path $catalogTestRoot 'svg-only'
& $catalogTestExe --from-catalog (Join-Path $catalogResult 'resources.csv') --svg-only --export --output $catalogSvgOutput
if ($LASTEXITCODE -ne 0) { throw 'SVG-only export failed' }
$catalogSvgRows = @(Import-Csv -LiteralPath (Join-Path $catalogSvgOutput 'resources.csv'))
if ($catalogSvgRows.Count -ne @($catalogFindings | Where-Object Kind -EQ svg).Count -or ($catalogSvgRows | Where-Object Kind -NE svg)) { throw 'SVG-only selection lost SVGs or included other formats' }
if (@(Import-Csv -LiteralPath (Join-Path $catalogSvgOutput 'skips.csv')).Count) { throw 'SVG-only catalog filtering falsely reported missing non-SVG resources' }
foreach ($catalogSvgRow in $catalogSvgRows) {
    if ($catalogSvgRow.ExportedFile -notmatch '^assets/[\p{L}\p{Nd}_-]{1,48}--[a-f0-9]{64}\.svg$' -or
        (Get-FileHash -LiteralPath (Join-Path $catalogSvgOutput $catalogSvgRow.ExportedFile)).Hash -ne $catalogSvgRow.AssetSha256) { throw 'SVG filename was unsafe or bytes changed' }
}
if (!($catalogSvgRows | Where-Object { $_.Location -like '*outside:icon?.svg' -and $_.ExportedFile -like 'assets/outside-icon--*.svg' })) { throw 'Unsafe resource basename was not sanitized' }
$catalogUnicodeStem = [string][char]0x56fe + [char]0x6807
if (!($catalogSvgRows | Where-Object { $_.Location -like "*$catalogUnicodeStem.svg" -and $_.ExportedFile -like "assets/$catalogUnicodeStem--*.svg" })) { throw 'Unicode resource basename was lost' }
$catalogHtml = Get-Content -LiteralPath (Join-Path $catalogExport 'examples.html') -Raw
if ($catalogHtml -match '\btitle\s*=' -or $catalogHtml -notmatch '\.kind\{[^}]*white-space:nowrap' -or $catalogHtml -notmatch 'Segoe UI Emoji' -or $catalogHtml -notmatch "img.src=x.ExportedFile") { throw 'Report lost stable Kind sizing, emoji, no-tooltip policy or SVG previews' }
# Old catalogs need only the original locator columns; selecting one resource
# must not catalog all its neighbors, and duplicate CSV rows are harmless.
$catalogChosen = $catalogFindings | Where-Object { $_.File.EndsWith('Uninstalled-x64.dll') -and $_.Location -eq 'CLR/Demo.g.resources/images/vector.xaml' } | Select-Object -First 1
$catalogSelection = Join-Path $catalogTestRoot 'selection.csv'
@($catalogChosen, $catalogChosen) | Select-Object File, Member, Location, Kind, Confidence, Bytes, IconHint, Evidence | Export-Csv -LiteralPath $catalogSelection -NoTypeInformation -Encoding UTF8
$catalogSelectedOutput = Join-Path $catalogTestRoot 'selected'
& $catalogTestExe --from-catalog $catalogSelection --export --output $catalogSelectedOutput
if ($LASTEXITCODE -ne 0) { throw 'Selected old-format catalog export failed' }
$catalogSelectedSummary = Get-Content -LiteralPath (Join-Path $catalogSelectedOutput 'summary.json') -Raw | ConvertFrom-Json
if ($catalogSelectedSummary.FilesInspected -ne 1 -or $catalogSelectedSummary.Findings -ne 1 -or $catalogSelectedSummary.RequestedResources -ne 1) { throw 'Selection was not exact or duplicate requests were repeated' }
$catalogChanged = [pscustomobject]@{ File=$catalogChosen.File; Member=''; Location='CLR/no-longer-present.svg'; Kind='svg' }
$catalogChanged | Export-Csv -LiteralPath (Join-Path $catalogTestRoot 'changed.csv') -NoTypeInformation -Encoding UTF8
$catalogChangedOutput = Join-Path $catalogTestRoot 'changed'
& $catalogTestExe --from-catalog (Join-Path $catalogTestRoot 'changed.csv') --export --output $catalogChangedOutput
if ($LASTEXITCODE -ne 0 -or !(Import-Csv -LiteralPath (Join-Path $catalogChangedOutput 'skips.csv') | Where-Object Reason -Like '*not found*')) { throw 'Missing cataloged resource was not reported' }
@'
"File","Member","Location","Kind"
'@.TrimStart() | Set-Content -LiteralPath (Join-Path $catalogTestRoot 'empty.csv') -Encoding UTF8
& $catalogTestExe --from-catalog (Join-Path $catalogTestRoot 'empty.csv') --export --output (Join-Path $catalogTestRoot 'empty')
if ($LASTEXITCODE -ne 0) { throw 'Empty selection should make an empty report' }
& $catalogTestExe --from-catalog (Join-Path $catalogResult 'files.csv') --output (Join-Path $catalogTestRoot 'wrong-csv')
if ($LASTEXITCODE -ne 1) { throw 'files.csv should not be mistaken for resources.csv' }
$catalogBudgetRoot = Join-Path $catalogTestRoot 'budget-fixtures'
$null = New-Item -ItemType Directory -Path $catalogBudgetRoot
foreach ($catalogNumber in 1..2) {
    [IO.File]::WriteAllText((Join-Path $catalogBudgetRoot "$catalogNumber.svg"), ("<svg xmlns='http://www.w3.org/2000/svg'><!--" + ('x' * 700000) + "$catalogNumber--></svg>"), [Text.UTF8Encoding]::new($false))
}
$catalogBudgetOutput = Join-Path $catalogTestRoot 'budget'
& $catalogTestExe $catalogBudgetRoot --export --max-export-mb 1 --output $catalogBudgetOutput
$catalogBudgetSummary = Get-Content -LiteralPath (Join-Path $catalogBudgetOutput 'summary.json') -Raw | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $catalogBudgetSummary.Findings -ne 2 -or $catalogBudgetSummary.ExportedAssets -ne 1 -or $catalogBudgetSummary.Skips -ne 1) { throw 'Export budget should bound copies while cataloging continues' }
$catalogLimited = Join-Path $catalogTestRoot 'limited'
& $catalogTestExe $catalogFixtures --output $catalogLimited --max-files 1
if ($LASTEXITCODE -ne 2) { throw 'An explicit scan limit should produce exit code 2' }
if ((Get-Content -LiteralPath (Join-Path $catalogLimited 'summary.json') -Raw | ConvertFrom-Json).State -ne 'limited') { throw 'Partial scan was not labelled limited' }
$catalogInterrupted = Join-Path $catalogTestRoot 'interrupted'
& $catalogTestExe $catalogFixtures --output $catalogInterrupted --cancel-file (Join-Path $catalogFixtures 'logo.svg')
if ($LASTEXITCODE -ne 130 -or (Get-Content -LiteralPath (Join-Path $catalogInterrupted 'summary.json') -Raw | ConvertFrom-Json).State -ne 'interrupted') { throw 'Cancellation should preserve a clearly labelled partial catalog' }
$catalogSavedHash = (Get-FileHash -LiteralPath (Join-Path $catalogResult 'resources.csv')).Hash
& $catalogTestExe $catalogFixtures --output $catalogResult
if ($LASTEXITCODE -ne 1 -or (Get-FileHash -LiteralPath (Join-Path $catalogResult 'resources.csv')).Hash -ne $catalogSavedHash) { throw 'Existing catalog protection failed' }
Write-Output "PASS: native/CLR/package formats, exact CSV targets, nested ZIP and CSV quoting, byte-preserving deduplicated exports, raw compiled resources without deserialization, export budget, report layout/emoji, malformed inputs, partial catalogs and overwrite protection. $catalogTestRoot"
$global:LASTEXITCODE = 0
