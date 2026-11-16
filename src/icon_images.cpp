#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/icon_images.hpp"
#include "launcher/svg_images.hpp"
#include "launcher/pe_resources.hpp"
#include "launcher/file_lookup.hpp"
#include "launcher/config_error.hpp"
#include <shlwapi.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace launcher {
namespace {

using Microsoft::WRL::ComPtr;
constexpr std::uint64_t cache_byte_limit = 32ULL * 1024 * 1024;
constexpr std::size_t image_byte_limit = 32 * 1024 * 1024;
constexpr std::size_t cache_entry_limit = 128;

// Prefer exact, then the nearest larger size; only upscale when all are smaller.
// For duplicate dimensions, prefer the higher color depth.
bool better_frame(UINT size, UINT depth, UINT best_size, UINT best_depth, UINT requested)
{
    if (best_size == 0) { return true; }
    if (size == best_size) { return depth > best_depth; }
    if ((size >= requested) != (best_size >= requested)) { return size >= requested; }
    return size >= requested ? size < best_size : size > best_size;
}

unsigned word(std::span<const BYTE> bytes, std::size_t offset)
{
    return bytes[offset] | (static_cast<unsigned>(bytes[offset + 1]) << 8);
}

struct ModuleCloser final {
    void operator()(HMODULE module) const { FreeLibrary(module); }
};
struct IconCloser final {
    void operator()(HICON icon) const { DestroyIcon(icon); }
};
using Module = std::unique_ptr<std::remove_pointer_t<HMODULE>, ModuleCloser>;
using Icon = std::unique_ptr<std::remove_pointer_t<HICON>, IconCloser>;

struct GroupSearch final {
    int remaining;
    HRSRC resource = nullptr;
};

BOOL CALLBACK find_group(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR parameter)
{
    auto& search = *reinterpret_cast<GroupSearch*>(parameter);
    if (search.remaining-- != 0) { return TRUE; }
    search.resource = FindResourceW(module, name, type);
    return FALSE;
}

SHSTOCKICONID stock_id(StockIcon stock)
{
    switch (stock) {
    case StockIcon::application: return SIID_APPLICATION;
    case StockIcon::folder: return SIID_FOLDER;
    case StockIcon::document: return SIID_DOCNOASSOC;
    case StockIcon::computer: return SIID_DESKTOPPC;
    case StockIcon::world: return SIID_WORLD;
    case StockIcon::find: return SIID_FIND;
    case StockIcon::shield: return SIID_SHIELD;
    case StockIcon::recycle: return SIID_RECYCLERFULL;
    case StockIcon::settings: return SIID_SETTINGS;
    case StockIcon::information: return SIID_INFO;
    case StockIcon::drive: return SIID_DRIVEFIXED;
    case StockIcon::network: return SIID_MYNETWORK;
    case StockIcon::audio: return SIID_AUDIOFILES;
    case StockIcon::image: return SIID_IMAGEFILES;
    case StockIcon::video: return SIID_VIDEOFILES;
    }
    return SIID_APPLICATION;
}

} // namespace

void IconImages::initialize(IWICImagingFactory* factory)
{
    clear();
    factory_ = factory;
}

void IconImages::clear()
{
    entries_.clear();
    clock_ = 0;
    bytes_ = 0;
}

void IconImages::load(const IconSource& requested_source, UINT size, ComPtr<IWICBitmapSource>& output, const std::filesystem::path& directory)
{
    output.Reset();
    if (!factory_ || size == 0 || size > 4096) { throw std::system_error(static_cast<int>(E_INVALIDARG), std::system_category(), "Icon image arguments"); }
    auto source = requested_source;
    // Substitutions are complete before decoding. Prepared Shell items may name
    // virtual folders, so only explicit filesystem sources need a base directory.
    std::visit([&](auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (!std::is_same_v<T, StockIcon> && !std::is_same_v<T, ShellItemIcon>) {
            resolve_icon_path(value.path, directory);
            value.path = FileLookup({}, {}).require(value.path, FileFallback::exact_file);
        }
    }, source);
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    const auto* path = std::visit([](const auto& value) -> const std::filesystem::path* {
        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, StockIcon>) { return nullptr; }
        else { return &value.path; }
    }, source);
    if (path) { GetFileAttributesExW(path->c_str(), GetFileExInfoStandard, &attributes); }
    const std::uint64_t modified = (static_cast<std::uint64_t>(attributes.ftLastWriteTime.dwHighDateTime) << 32) |
        attributes.ftLastWriteTime.dwLowDateTime;
    const std::uint64_t file_size = (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
    for (auto& entry : entries_) {
        if (entry.source == source && entry.size == size &&
                entry.modified == modified && entry.file_size == file_size) {
            entry.used = ++clock_;
            output = entry.image;
            return;
        }
    }
    ComPtr<IWICBitmapSource> original;
    HRESULT loaded = E_INVALIDARG;
    if (const auto* stock = std::get_if<StockIcon>(&source)) {
        loaded = stock_image(*stock, size, original);
    } else if (const auto* file = std::get_if<FileIcon>(&source)) {
        const bool svg = _wcsicmp(file->path.extension().c_str(), L".svg") == 0;
        loaded = svg ? render_svg_image(factory_.Get(), file->path.wstring(), size, original)
            : file_size > image_byte_limit ? HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)
            : file_image(file->path.wstring(), size, original);
        if (SUCCEEDED(loaded) && svg) { loaded = convert(original.Get(), original); }
        if (FAILED(loaded) && !svg) { loaded = shell_image(file->path.wstring(), size, original); }
    } else if (const auto* group = std::get_if<IconGroup>(&source)) {
        loaded = group_image(group->path, group->selector, size, original);
    } else if (const auto* resource = std::get_if<PeResourceIcon>(&source)) {
        const auto bytes = read_pe_resource(*resource);
        const bool svg = is_svg_image(bytes);
        loaded = svg ? render_svg_data(factory_.Get(),
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), size, original)
            : encoded_image(bytes, size, original);
        if (SUCCEEDED(loaded) && svg) { loaded = convert(original.Get(), original); }
    } else if (const auto* item = std::get_if<ShellItemIcon>(&source)) {
        loaded = shell_image(item->path.wstring(), size, original);
    }
    if (FAILED(loaded)) { throw std::system_error(static_cast<int>(loaded), std::system_category(), "Load icon image"); }
    const HRESULT result = resize(original.Get(), size, output);
    if (FAILED(result)) { throw std::system_error(static_cast<int>(result), std::system_category(), "Resize icon image"); }
    const std::uint64_t bytes = static_cast<std::uint64_t>(size) * size * 4;
    if (bytes <= cache_byte_limit) {
        while (!entries_.empty() && (entries_.size() >= cache_entry_limit || bytes_ + bytes > cache_byte_limit)) {
            const auto oldest = std::min_element(entries_.begin(), entries_.end(),
                [](const auto& left, const auto& right) { return left.used < right.used; });
            bytes_ -= oldest->bytes;
            entries_.erase(oldest);
        }
        entries_.push_back({source, size, modified, file_size, ++clock_, bytes, output});
        bytes_ += bytes;
    }
}

HRESULT IconImages::convert(IWICBitmapSource* source, ComPtr<IWICBitmapSource>& output)
{
    ComPtr<IWICFormatConverter> converter;
    HRESULT result = factory_->CreateFormatConverter(converter.GetAddressOf());
    if (SUCCEEDED(result)) {
        result = converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }
    ComPtr<IWICBitmap> copied;
    if (SUCCEEDED(result)) { result = factory_->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnLoad, copied.GetAddressOf()); }
    if (FAILED(result)) { return result; }
    UINT width = 0;
    UINT height = 0;
    result = copied->GetSize(&width, &height);
    if (FAILED(result) || width == 0 || height == 0) { return FAILED(result) ? result : E_FAIL; }
    ComPtr<IWICBitmapLock> locked;
    result = copied->Lock(nullptr, WICBitmapLockRead, locked.GetAddressOf());
    if (FAILED(result)) { return result; }
    UINT stride = 0;
    UINT length = 0;
    BYTE* bytes = nullptr;
    result = locked->GetStride(&stride);
    if (SUCCEEDED(result)) { result = locked->GetDataPointer(&length, &bytes); }
    const std::uint64_t row_bytes = static_cast<std::uint64_t>(width) * 4;
    const std::uint64_t needed = static_cast<std::uint64_t>(height - 1) * stride + row_bytes;
    if (FAILED(result) || !bytes || row_bytes > stride || needed > length) { return FAILED(result) ? result : E_FAIL; }
    for (UINT row = 0; row < height; ++row) {
        for (UINT column = 0; column < width; ++column) {
            if (bytes[static_cast<std::size_t>(row) * stride + static_cast<std::size_t>(column) * 4 + 3] != 0) {
                output = copied;
                return S_OK;
            }
        }
    }
    return E_FAIL;
}

HRESULT IconImages::resize(IWICBitmapSource* source, UINT size, ComPtr<IWICBitmapSource>& output)
{
    UINT width = 0;
    UINT height = 0;
    HRESULT result = source->GetSize(&width, &height);
    if (FAILED(result) || width == 0 || height == 0) { return FAILED(result) ? result : E_FAIL; }
    if (width == size && height == size) { output = source; return S_OK; }
    ComPtr<IWICBitmapScaler> scaler;
    result = factory_->CreateBitmapScaler(scaler.GetAddressOf());
    if (SUCCEEDED(result)) {
        // Bilinear enlargement smooths tiny raster artwork without contrast
        // overshoot around hard edges. Cubic's negative lobes add visible halos.
        result = scaler->Initialize(source, size, size, size > std::max(width, height)
            ? WICBitmapInterpolationModeLinear : WICBitmapInterpolationModeFant);
    }
    return SUCCEEDED(result) ? convert(scaler.Get(), output) : result;
}

HRESULT IconImages::file_image(const std::wstring& path, UINT size, ComPtr<IWICBitmapSource>& output)
{
    const auto bytes = FileLookup::read_file(std::filesystem::path(path), image_byte_limit);
    return encoded_image(std::span(reinterpret_cast<const BYTE*>(bytes.data()), bytes.size()), size, output);
}

HRESULT IconImages::encoded_image(std::span<const BYTE> bytes, UINT size, ComPtr<IWICBitmapSource>& output)
{
    if (bytes.size() > std::numeric_limits<UINT>::max()) { return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE); }
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
    if (!stream) { return E_OUTOFMEMORY; }
    ComPtr<IWICBitmapDecoder> decoder;
    const auto result = factory_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf());
    return SUCCEEDED(result) ? decode_image(decoder.Get(), size, output) : result;
}

HRESULT IconImages::decode_image(IWICBitmapDecoder* decoder, UINT size, ComPtr<IWICBitmapSource>& output)
{
    UINT frames = 0;
    const auto result = decoder->GetFrameCount(&frames);
    if (FAILED(result) || frames == 0) { return FAILED(result) ? result : E_FAIL; }
    UINT best_size = 0;
    UINT best_depth = 0;
    ComPtr<IWICBitmapFrameDecode> selected;
    for (UINT index = 0; index < frames; ++index) {
        ComPtr<IWICBitmapFrameDecode> frame;
        UINT width = 0;
        UINT height = 0;
        WICPixelFormatGUID format{};
        if (FAILED(decoder->GetFrame(index, frame.GetAddressOf())) || FAILED(frame->GetSize(&width, &height))) { continue; }
        UINT depth = 0;
        ComPtr<IWICComponentInfo> component;
        ComPtr<IWICPixelFormatInfo> information;
        if (SUCCEEDED(frame->GetPixelFormat(&format)) && SUCCEEDED(factory_->CreateComponentInfo(format, component.GetAddressOf())) &&
                SUCCEEDED(component.As(&information))) { information->GetBitsPerPixel(&depth); }
        const UINT candidate_size = std::max(width, height);
        if (better_frame(candidate_size, depth, best_size, best_depth, size)) {
            selected = frame;
            best_size = candidate_size;
            best_depth = depth;
        }
    }
    return selected ? convert(selected.Get(), output) : E_FAIL;
}

HRESULT IconImages::resource_image(IShellItem* item, const std::wstring& path, UINT size, ComPtr<IWICBitmapSource>& output)
{
    ComPtr<IExtractIconW> extractor;
    HRESULT result = item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(extractor.GetAddressOf()));
    if (FAILED(result)) { return result; }
    std::wstring location(32768, L'\0');
    int index = 0;
    UINT flags = 0;
    result = extractor->GetIconLocation(0, location.data(), static_cast<UINT>(location.size()), &index, &flags);
    if (result != S_OK || (flags & GIL_SIMULATEDOC)) { return E_FAIL; }
    location.resize(std::char_traits<wchar_t>::length(location.c_str()));
    if (flags & GIL_NOTFILENAME) {
        // Current Windows returns an opaque cache index even for ordinary EXEs.
        // Their application icon is the first native group. Do not reinterpret
        // opaque indices for shortcuts, documents, or virtual/custom locations.
        const auto extension = std::filesystem::path(path).extension().wstring();
        if (_wcsicmp(extension.c_str(), L".exe") == 0 && _wcsicmp(path.c_str(), location.c_str()) == 0) {
            index = 0;
        } else if (_wcsicmp(extension.c_str(), L".lnk") == 0) {
            ComPtr<IShellLinkW> shortcut;
            ComPtr<IPersistFile> persistent;
            result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(shortcut.GetAddressOf()));
            if (SUCCEEDED(result)) { result = shortcut.As(&persistent); }
            if (SUCCEEDED(result)) { result = persistent->Load(path.c_str(), STGM_READ); }
            location.assign(32768, L'\0');
            if (SUCCEEDED(result)) { result = shortcut->GetIconLocation(location.data(), static_cast<int>(location.size()), &index); }
            if (FAILED(result)) { return result; }
            if (location.front() == L'\0') {
                result = shortcut->GetPath(location.data(), static_cast<int>(location.size()), nullptr, SLGP_RAWPATH);
                if (FAILED(result) || _wcsicmp(std::filesystem::path(location.c_str()).extension().c_str(), L".exe") != 0) {
                    return E_FAIL;
                }
                index = 0;
            }
            location.resize(std::char_traits<wchar_t>::length(location.c_str()));
        } else { return E_FAIL; }
    }
    if (_wcsicmp(std::filesystem::path(location).extension().c_str(), L".ico") == 0) {
        return file_image(location, size, output);
    }
    if (index < -65535) { return E_FAIL; }
    const IconGroupSelector selector = index < 0 ? IconGroupSelector{IconResourceId{static_cast<std::uint16_t>(-index)}}
        : IconGroupSelector{IconIndex{index}};
    return group_image(location, selector, size, output);
}

HRESULT IconImages::group_image(const std::filesystem::path& path, const IconGroupSelector& selector, UINT size, ComPtr<IWICBitmapSource>& output)
{
    const FileLookup files({}, {});
    const auto selected_path = files.require(path, FileFallback::exact_file);
    DWORD load_error = ERROR_SUCCESS;
    Module module(files.open_exact(selected_path, [&](const auto& filename) {
        const auto loaded = LoadLibraryExW(filename.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!loaded) { load_error = GetLastError(); }
        return loaded;
    }));
    if (!module) { return HRESULT_FROM_WIN32(load_error ? load_error : ERROR_GEN_FAILURE); }
    HRSRC group = nullptr;
    if (const auto* id = std::get_if<IconResourceId>(&selector)) {
        group = FindResourceW(module.get(), MAKEINTRESOURCEW(id->value), RT_GROUP_ICON);
    } else {
        const auto index = std::get<IconIndex>(selector).value;
        if (index < 0) { return E_INVALIDARG; }
        GroupSearch search{index};
        EnumResourceNamesW(module.get(), RT_GROUP_ICON, find_group, reinterpret_cast<LONG_PTR>(&search));
        group = search.resource;
    }
    if (!group) { return E_FAIL; }
    const DWORD length = SizeofResource(module.get(), group);
    const auto* data = static_cast<const BYTE*>(LockResource(LoadResource(module.get(), group)));
    if (!data || length < 6) { return E_FAIL; }
    const std::span bytes(data, length);
    const unsigned count = word(bytes, 4);
    if (word(bytes, 0) != 0 || word(bytes, 2) != 1 || count == 0 || count > (length - 6) / 14) { return E_FAIL; }
    UINT best_size = 0;
    UINT best_depth = 0;
    UINT best_width = 0;
    UINT best_height = 0;
    HRSRC selected = nullptr;
    for (unsigned entry = 0; entry < count; ++entry) {
        const std::size_t offset = 6 + static_cast<std::size_t>(entry) * 14;
        const UINT width = bytes[offset] == 0 ? 256 : bytes[offset];
        const UINT height = bytes[offset + 1] == 0 ? 256 : bytes[offset + 1];
        const UINT depth = word(bytes, offset + 6);
        const UINT candidate_size = std::max(width, height);
        const auto resource = FindResourceW(module.get(), MAKEINTRESOURCEW(word(bytes, offset + 12)), RT_ICON);
        if (resource && better_frame(candidate_size, depth, best_size, best_depth, size)) {
            selected = resource;
            best_size = candidate_size;
            best_depth = depth;
            best_width = width;
            best_height = height;
        }
    }
    if (!selected) { return E_FAIL; }
    const DWORD image_length = SizeofResource(module.get(), selected);
    auto* image_data = static_cast<BYTE*>(LockResource(LoadResource(module.get(), selected)));
    if (!image_data || image_length == 0) { return E_FAIL; }
    // Decode at the resource's native dimensions. Launcher performs the resizing;
    // no Shell-generated transparent canvas or fallback frame enters the cache.
    Icon icon(CreateIconFromResourceEx(image_data, image_length, TRUE, 0x00030000,
        static_cast<int>(best_width), static_cast<int>(best_height), LR_DEFAULTCOLOR));
    if (!icon) { return E_FAIL; }
    ComPtr<IWICBitmap> bitmap;
    const HRESULT result = factory_->CreateBitmapFromHICON(icon.get(), bitmap.GetAddressOf());
    return SUCCEEDED(result) ? convert(bitmap.Get(), output) : result;
}

HRESULT IconImages::shell_image(const std::wstring& path, UINT size, ComPtr<IWICBitmapSource>& output)
{
    ComPtr<IShellItemImageFactory> item;
    HRESULT result = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.GetAddressOf()));
    if (FAILED(result)) { return result; }
    ComPtr<IShellItem> shell_item;
    if (SUCCEEDED(item.As(&shell_item))) {
        // Shell locations may be virtual or refer to an unavailable icon
        // resource. Preserve the ordinary Shell image fallback in that case.
        try {
            if (SUCCEEDED(resource_image(shell_item.Get(), path, size, output))) { return S_OK; }
        } catch (const ConfigError&) {}
    }
    HBITMAP bitmap = nullptr;
    result = item->GetImage(SIZE{static_cast<LONG>(size), static_cast<LONG>(size)},
        static_cast<SIIGBF>(SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK), &bitmap);
    if (FAILED(result)) { return result; }
    ComPtr<IWICBitmap> image;
    result = factory_->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUseAlpha, image.GetAddressOf());
    if (SUCCEEDED(result)) { result = convert(image.Get(), output); }
    DeleteObject(bitmap);
    return result;
}

HRESULT IconImages::stock_image(StockIcon stock, UINT size, ComPtr<IWICBitmapSource>& output)
{
    SHSTOCKICONINFO information{};
    information.cbSize = sizeof(information);
    HRESULT result = SHGetStockIconInfo(stock_id(stock), SHGSI_ICONLOCATION, &information);
    HICON extracted = nullptr;
    if (SUCCEEDED(result)) {
        result = SHDefExtractIconW(information.szPath, information.iIcon, 0, &extracted, nullptr, size);
        if (result != S_OK || !extracted) { result = E_FAIL; }
    }
    Icon icon(extracted);
    if (FAILED(result)) {
        information = {};
        information.cbSize = sizeof(information);
        result = SHGetStockIconInfo(stock_id(stock), SHGSI_ICON | SHGSI_LARGEICON, &information);
        icon.reset(information.hIcon);
    }
    if (FAILED(result) || !icon) { return FAILED(result) ? result : E_FAIL; }
    ComPtr<IWICBitmap> image;
    result = factory_->CreateBitmapFromHICON(icon.get(), image.GetAddressOf());
    return SUCCEEDED(result) ? convert(image.Get(), output) : result;
}

} // namespace launcher
