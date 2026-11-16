#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/svg_images.hpp"
#include "launcher/file_lookup.hpp"

#include <lunasvg.h>
#include <shlwapi.h>
#include <xmllite.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

namespace launcher {

bool is_svg_image(std::span<const BYTE> bytes)
{
    if (bytes.empty() || bytes.size() > 32 * 1024 * 1024) { return false; }
    constexpr std::string_view closing = "</svg>";
    if (bytes.size() >= closing.size() && std::equal(closing.begin(), closing.end(), bytes.end() - closing.size())) { return true; }
    // A suffix is only a fast positive hint. XmlLite reads just through the
    // root, handling BOMs, XML declarations, comments, doctypes and prefixes.
    Microsoft::WRL::ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
    if (!stream) { return false; }
    Microsoft::WRL::ComPtr<IXmlReader> reader;
    if (FAILED(CreateXmlReader(IID_PPV_ARGS(reader.GetAddressOf()), nullptr))) { return false; }
    if (FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Parse)) ||
        FAILED(reader->SetProperty(XmlReaderProperty_XmlResolver, 0)) || FAILED(reader->SetInput(stream.Get()))) { return false; }
    XmlNodeType node{};
    while (reader->Read(&node) == S_OK) {
        if (node != XmlNodeType_Element) { continue; }
        const WCHAR* name = nullptr;
        const WCHAR* space = nullptr;
        UINT name_length = 0;
        UINT space_length = 0;
        if (FAILED(reader->GetLocalName(&name, &name_length)) || FAILED(reader->GetNamespaceUri(&space, &space_length))) { return false; }
        return std::wstring_view(name, name_length) == L"svg" && (space_length == 0 ||
            std::wstring_view(space, space_length) == L"http://www.w3.org/2000/svg");
    }
    return false;
}

HRESULT render_svg_image(IWICImagingFactory* factory, const std::wstring& path, UINT size, Microsoft::WRL::ComPtr<IWICBitmapSource>& output)
{
    output.Reset();
    if (!factory || size == 0 || size > 4096) { return E_INVALIDARG; }
    // The selected source is exact here; decoding never repeats root lookup.
    // Owned bytes retain Unicode path support without borrowing a file handle.
    const auto xml = FileLookup::read_file(std::filesystem::path(path), 32 * 1024 * 1024);
    if (xml.empty()) { return E_FAIL; }
    return render_svg_data(factory, xml, size, output);
}

HRESULT render_svg_data(IWICImagingFactory* factory, std::string_view xml, UINT size, Microsoft::WRL::ComPtr<IWICBitmapSource>& output)
{
    output.Reset();
    if (!factory || size == 0 || size > 4096 || xml.empty() || xml.size() > 32 * 1024 * 1024) { return E_INVALIDARG; }
    // LunaSVG expects UTF-8 markup beginning with '<', rather than a byte-order
    // mark. XmlLite accepts the BOM during sniffing; discard it for rendering.
    if (xml.starts_with("\xEF\xBB\xBF")) { xml.remove_prefix(3); }
    auto document = lunasvg::Document::loadFromData(xml.data(), xml.size());
    if (!document) { return E_FAIL; }
    const float width = document->width();
    const float height = document->height();
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) { return E_INVALIDARG; }
    lunasvg::Bitmap pixels(static_cast<int>(size), static_cast<int>(size));
    if (pixels.isNull()) { return E_OUTOFMEMORY; }
    pixels.clear(0x00000000);
    const float extent = static_cast<float>(size);
    const float scale = std::min(extent / width, extent / height);
    // Fit the SVG's complete viewport, preserving authored margins, viewBox,
    // aspect-ratio policy, and non-square proportions inside the icon cell.
    document->render(pixels, lunasvg::Matrix(scale, 0, 0, scale,
        (extent - width * scale) * 0.5F, (extent - height * scale) * 0.5F));
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    // LunaSVG's premultiplied ARGB32 is BGRA bytes on Windows. WIC copies the
    // pixels, so the cached bitmap does not depend on the renderer's lifetime.
    const auto stride = static_cast<UINT>(pixels.stride());
    const HRESULT result = factory->CreateBitmapFromMemory(size, size, GUID_WICPixelFormat32bppPBGRA,
        stride, stride * size, pixels.data(), bitmap.GetAddressOf());
    if (SUCCEEDED(result)) { output = bitmap; }
    return result;
}

} // namespace launcher
