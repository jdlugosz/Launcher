#pragma once

#include "launcher/windows_pch.hpp"
#include "launcher/icon_source.hpp"

#include <cstdint>
#include <string>
#include <span>
#include <vector>

namespace launcher {

// Device-independent, fully decoded images at their final physical-pixel size.
// Direct2D bitmaps belong to the window's render target and are cached there.
// Entries are limited to 128 images / 32 MiB and invalidated by source-file
// size and timestamp. Raster and vector loaders share this size interface.
// Encoded images are bounded at 32 MiB; larger ordinary files use the Shell.
class IconImages final {
public:
    void initialize(IWICImagingFactory* factory);
    // Explicit files are pinned through FileLookup before decoding. Filesystem
    // errors throw ConfigError; invalid image data throws std::system_error.
    void load(const IconSource& source, UINT size, Microsoft::WRL::ComPtr<IWICBitmapSource>& output, const std::filesystem::path& directory = {});
    void clear();

private:
    struct Entry final {
        IconSource source;
        UINT size;
        std::uint64_t modified;
        std::uint64_t file_size;
        std::uint64_t used;
        std::uint64_t bytes;
        Microsoft::WRL::ComPtr<IWICBitmapSource> image;
    };

    HRESULT convert(IWICBitmapSource* source, Microsoft::WRL::ComPtr<IWICBitmapSource>& output);
    HRESULT resize(IWICBitmapSource* source, UINT size, Microsoft::WRL::ComPtr<IWICBitmapSource>& output);
    HRESULT file_image(const std::wstring& path, UINT size, Microsoft::WRL::ComPtr<IWICBitmapSource>& output);
    HRESULT encoded_image(std::span<const BYTE> bytes, UINT size, Microsoft::WRL::ComPtr<IWICBitmapSource>& output);
    HRESULT decode_image(IWICBitmapDecoder* decoder, UINT size, Microsoft::WRL::ComPtr<IWICBitmapSource>& output);
    HRESULT shell_image(const std::wstring& path, UINT size, Microsoft::WRL::ComPtr<IWICBitmapSource>& output);
    HRESULT resource_image(IShellItem* item, const std::wstring& path, UINT size, Microsoft::WRL::ComPtr<IWICBitmapSource>& output);
    HRESULT group_image(const std::filesystem::path& path, const IconGroupSelector& selector, UINT size, Microsoft::WRL::ComPtr<IWICBitmapSource>& output);
    HRESULT stock_image(StockIcon stock, UINT size, Microsoft::WRL::ComPtr<IWICBitmapSource>& output);

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory_;
    std::vector<Entry> entries_;
    std::uint64_t clock_ = 0;
    std::uint64_t bytes_ = 0;
};

} // namespace launcher
