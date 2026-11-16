#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/png.hpp"

#include <shlwapi.h>

#include <cmath>
#include <format>
#include <new>
#include <string>
#include <system_error>

namespace launcher {
namespace {
using Microsoft::WRL::ComPtr;

HRESULT encode_png(IWICImagingFactory* factory, IWICBitmapSource* image, IStream* stream, UINT width, UINT height, double dpi)
{
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
    if (SUCCEEDED(result)) { result = encoder->Initialize(stream, WICBitmapEncoderNoCache); }
    if (SUCCEEDED(result)) { result = encoder->CreateNewFrame(frame.GetAddressOf(), nullptr); }
    if (SUCCEEDED(result)) { result = frame->Initialize(nullptr); }
    if (SUCCEEDED(result)) { result = frame->SetSize(width, height); }
    if (SUCCEEDED(result)) { result = frame->SetResolution(dpi, dpi); }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(result)) { result = frame->SetPixelFormat(&format); }
    if (SUCCEEDED(result) && format != GUID_WICPixelFormat32bppBGRA) { result = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT; }
    if (SUCCEEDED(result)) { result = frame->WriteSource(image, nullptr); }
    if (SUCCEEDED(result)) { result = frame->Commit(); }
    if (SUCCEEDED(result)) { result = encoder->Commit(); }
    return result;
}
} // namespace

HRESULT write_png(IWICImagingFactory* factory, IWICBitmapSource* image, const std::filesystem::path& path, double dpi, const FileLookup& files)
{
    if (!factory || !image || !std::isfinite(dpi) || dpi <= 0.0) { return E_INVALIDARG; }
    UINT width = 0;
    UINT height = 0;
    HRESULT result = image->GetSize(&width, &height);
    if (FAILED(result)) { return result; }
    if (!width || !height) { return E_INVALIDARG; }
    ComPtr<IWICBitmapSource> straight;
    result = WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, image, straight.GetAddressOf());
    if (FAILED(result)) { return result; }
    try {
        return files.open_exact(path, [&](const auto& exact) {
            ComPtr<IStream> stream;
            HRESULT encoded = SHCreateStreamOnFileEx(exact.c_str(), STGM_WRITE | STGM_SHARE_EXCLUSIVE | STGM_FAILIFTHERE,
                FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, stream.GetAddressOf());
            if (FAILED(encoded)) { return encoded; }
            encoded = encode_png(factory, straight.Get(), stream.Get(), width, height, dpi);
            stream.Reset();
            if (FAILED(encoded)) { DeleteFileW(exact.c_str()); }
            return encoded;
        });
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}

HRESULT save_snapshot_png(IWICImagingFactory* factory, IWICBitmapSource* image, const std::filesystem::path& directory, double dpi, const SYSTEMTIME& time, std::filesystem::path& written, const FileLookup& files)
{
    written.clear();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) { return HRESULT_FROM_WIN32(error.value()); }
    const auto stem = std::format(L"Launcher-{:04}-{:02}-{:02}_{:02}-{:02}-{:02}-{:03}",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    for (unsigned attempt = 0; attempt < 10000; ++attempt) {
        const auto path = directory / (stem + (attempt ? L"-" + std::to_wstring(attempt + 1) : L"") + L".png");
        const HRESULT result = write_png(factory, image, path, dpi, files);
        if (result == STG_E_FILEALREADYEXISTS || result == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS) ||
                result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) { continue; }
        if (SUCCEEDED(result)) { written = path; }
        return result;
    }
    return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
}
} // namespace launcher
