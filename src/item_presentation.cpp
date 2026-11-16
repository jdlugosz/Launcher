#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/item_presentation.hpp"
#include "launcher/icon_images.hpp"
#include "launcher/config_error.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace launcher {
namespace {

using Microsoft::WRL::ComPtr;

class ComApartment final {
public:
    ComApartment() noexcept : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ComApartment() { if (SUCCEEDED(result_)) { CoUninitialize(); } }
    HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_;
};

std::wstring utf16(std::string_view text)
{
    if (text.find('\0') != std::string_view::npos || text.size() > INT_MAX) {
        throw std::invalid_argument("Presentation text contains NUL or is too long.");
    }
    if (text.empty()) { return {}; }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count == 0) { throw std::invalid_argument("Presentation text is not valid UTF-8."); }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count) != count) {
        throw std::invalid_argument("Cannot convert presentation text to Unicode.");
    }
    return result;
}

std::wstring unresolved_label(const Item& item)
{
    if (!item.launch) { return {}; }
    auto text = utf16(item.launch->target);
    // This fallback is lexical. launch_display_label could read an absolute
    // .exe's metadata, including an unresolved pattern or an unavailable drive.
    const auto letter = [](wchar_t value) { return (value >= L'A' && value <= L'Z') || (value >= L'a' && value <= L'z'); };
    const auto colon = text.find(L':');
    const bool protocol = colon != std::wstring::npos && colon > 1 && letter(text.front()) &&
        std::all_of(text.begin() + 1, text.begin() + colon, [&](wchar_t value) {
            return letter(value) || (value >= L'0' && value <= L'9') || value == L'+' || value == L'-' || value == L'.';
        });
    if (item.launch->handler == "shell" && !protocol) {
        try {
            if (const auto filename = std::filesystem::path(text).filename().wstring(); !filename.empty()) { return filename; }
        } catch (const std::filesystem::filesystem_error&) {}
    }
    return text;
}

void diagnostic(ItemPresentation& result, const char* operation, HRESULT error)
{
    result.diagnostics.push_back(std::string(operation) + ": " + std::system_category().message(static_cast<int>(error)));
}

void copy_pixels(IWICBitmapSource* image, ItemPresentation& result)
{
    UINT width = 0;
    UINT height = 0;
    HRESULT copied = image->GetSize(&width, &height);
    WICPixelFormatGUID format{};
    if (SUCCEEDED(copied)) { copied = image->GetPixelFormat(&format); }
    const auto bytes = static_cast<std::uint64_t>(width) * height * 4;
    if (SUCCEEDED(copied) && (width == 0 || height == 0 || width > 4096 || height > 4096 ||
            bytes > 64ULL * 1024 * 1024 || !IsEqualGUID(format, GUID_WICPixelFormat32bppPBGRA))) {
        copied = E_UNEXPECTED;
    }
    if (FAILED(copied)) { throw std::system_error(static_cast<int>(copied), std::system_category(), "Read presentation image"); }
    const UINT stride = width * 4;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(bytes));
    copied = image->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data());
    if (FAILED(copied)) { throw std::system_error(static_cast<int>(copied), std::system_category(), "Copy presentation pixels"); }
    result.width = width;
    result.height = height;
    result.stride = stride;
    result.pixels = std::move(pixels);
}

} // namespace

ItemPresentation prepare_item_presentation(const ItemPresentationRequest& request)
{
    if (request.pixel_size == 0 || request.pixel_size > 4096) {
        throw std::invalid_argument("Presentation icon size must be 1..4096 pixels.");
    }
    ItemPresentation result;
    if (!request.action) {
        result.launch.result = {false, ERROR_NOT_READY, L"No resolved launch target is available."};
    } else if (request.action->handler == "adobeCC" || version_target_pattern(*request.action, request.configuration_directory)) {
        result.launch.result = {false, ERROR_INVALID_PARAMETER, L"Presentation requires an already selected concrete version target."};
        result.diagnostics.push_back("Rejected an unresolved version action during presentation preparation.");
    } else {
        result.launch = prepare_launch_action(*request.action, request.configuration_directory);
    }
    result.label = request.item.label_explicit || !request.item.label.empty() || !request.item.launch
        ? utf16(request.item.label)
        : result.launch.action ? launch_display_label(*result.launch.action) : unresolved_label(request.item);
    if (result.launch.action) {
        result.can_admin = can_run_as_administrator(*result.launch.action);
        result.can_reveal = result.launch.action->shell_namespace ||
            GetFileAttributesW(result.launch.action->target.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    const ComApartment apartment;
    if (FAILED(apartment.result())) {
        diagnostic(result, "Presentation requires a COM STA worker", apartment.result());
        return result;
    }
    ComPtr<IWICImagingFactory> factory;
    const HRESULT created = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(created)) {
        diagnostic(result, "Create presentation image factory", created);
        return result;
    }
    IconImages images;
    images.initialize(factory.Get());
    ComPtr<IWICBitmapSource> image;
    const auto& icon_action = request.item.launch ? request.item.launch : request.item.shift_launch;
    const bool automatic = !request.item.icon_explicit && icon_action &&
        (icon_action->handler == "shell" || icon_action->handler == "adobeCC");
    const auto stock_fallback = [&](const std::exception& error) {
        result.diagnostics.push_back(std::string("Presentation icon unavailable; stock fallback: ") + error.what());
        try {
            images.load(StockIcon::application, request.pixel_size, image);
            copy_pixels(image.Get(), result);
        } catch (const std::system_error& fallback) {
            result.diagnostics.push_back(std::string("Presentation stock icon unavailable: ") + fallback.what());
        }
    };
    try {
        if (automatic) {
            // Alternate-only artwork is prepared on this worker without
            // supplying a normal-click action or its menu capabilities.
            const auto alternate = !request.item.launch
                ? prepare_launch_action(*icon_action, request.configuration_directory) : PreparedLaunch{};
            const auto& launch = request.item.launch ? result.launch : alternate;
            if (launch.action) {
                images.load(ShellItemIcon{launch.action->target}, request.pixel_size, image);
            } else {
                images.load(StockIcon::application, request.pixel_size, image);
            }
        } else {
            images.load(request.source, request.pixel_size, image, request.configuration_directory);
        }
        copy_pixels(image.Get(), result);
    } catch (const ConfigError& error) { stock_fallback(error); }
    catch (const std::system_error& error) { stock_fallback(error); }
    return result;
}

} // namespace launcher
