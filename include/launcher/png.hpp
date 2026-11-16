#pragma once

#include "launcher/windows_pch.hpp"
#include "launcher/file_lookup.hpp"

#include <filesystem>

namespace launcher {

// Write straight-alpha PNG pixels, converting premultiplied drawing sources.
// Existing files are never replaced; an unsuccessful new file is removed.
HRESULT write_png(IWICImagingFactory* factory, IWICBitmapSource* image, const std::filesystem::path& path, double dpi, const FileLookup& files = FileLookup({}, {}));

// Create the directory as needed and add a numeric suffix on timestamp collisions.
HRESULT save_snapshot_png(IWICImagingFactory* factory, IWICBitmapSource* image, const std::filesystem::path& directory, double dpi, const SYSTEMTIME& time, std::filesystem::path& written, const FileLookup& files = FileLookup({}, {}));

} // namespace launcher
