#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/item_presentation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

class Apartment final {
public:
    explicit Apartment(DWORD flags) noexcept : result_(CoInitializeEx(nullptr, flags)) {}
    ~Apartment() { if (SUCCEEDED(result_)) { CoUninitialize(); } }
    HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_;
};

std::string utf8(std::wstring_view text)
{
    if (text.empty()) { return {}; }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size == 0) { throw std::runtime_error("Cannot convert presentation fixture text."); }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr) != size) {
        throw std::runtime_error("Cannot convert presentation fixture text.");
    }
    return result;
}

class Fixture final {
public:
    Fixture() : parent_(fs::weakly_canonical(fs::temp_directory_path()))
    {
        static std::atomic<unsigned> sequence = 0;
        root = parent_ / (L"LauncherPresentation-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence++));
        REQUIRE(fs::create_directory(root));
    }
    ~Fixture()
    {
        std::error_code error;
        const auto resolved = fs::weakly_canonical(root, error);
        if (!error && resolved == root && resolved.parent_path() == parent_) { fs::remove_all(root, error); }
    }
    fs::path write(std::wstring_view name, std::string_view text) const
    {
        const auto path = root / name;
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        REQUIRE(output.good());
        return path;
    }
    fs::path probe() const
    {
        std::wstring module(32768, L'\0');
        const auto count = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        REQUIRE(count > 0);
        REQUIRE(count < module.size());
        module.resize(count);
        const auto source = fs::path(module).parent_path() / L"LauncherLaunchGuiProbe.exe";
        const auto destination = root / L"Renamed application 资料.exe";
        REQUIRE(fs::copy_file(source, destination));
        return destination;
    }
    launcher::ItemPresentationRequest request() const
    {
        launcher::ItemPresentationRequest result;
        result.configuration_directory = root;
        result.pixel_size = 24;
        return result;
    }

    fs::path root;

private:
    fs::path parent_;
};

void complete_pixels(const launcher::ItemPresentation& result, std::uint32_t size)
{
    REQUIRE(result.width == size);
    REQUIRE(result.height == size);
    REQUIRE(result.stride == size * 4);
    REQUIRE(result.pixels.size() == static_cast<std::size_t>(size) * size * 4);
    bool visible = false;
    bool premultiplied = true;
    for (std::size_t pixel = 0; pixel < result.pixels.size(); pixel += 4) {
        const auto alpha = result.pixels[pixel + 3];
        premultiplied = premultiplied && result.pixels[pixel] <= alpha &&
            result.pixels[pixel + 1] <= alpha && result.pixels[pixel + 2] <= alpha;
        visible = visible || alpha != 0;
    }
    CHECK(premultiplied);
    CHECK(visible);
}

} // namespace

TEST_CASE("Unresolved presentation never discovers its item's original version target", "[presentation][native]")
{
    Fixture fixture;
    fixture.write(L"Blender 5.9/blender.exe", "A matching file must not become a presentation action.");
    auto request = fixture.request();
    launcher::LaunchAction original;
    original.target = utf8((fixture.root / L"Blender */blender.exe").wstring());
    request.item.launch = original;
    const auto result = launcher::prepare_item_presentation(request);
    CHECK_FALSE(result.launch.action);
    CHECK_FALSE(result.launch.result.success);
    CHECK(result.launch.result.error_code == ERROR_NOT_READY);
    CHECK(result.label == L"blender.exe");
    CHECK_FALSE(result.can_admin);
    CHECK_FALSE(result.can_reveal);
    complete_pixels(result, request.pixel_size);

    request.item.launch->handler = "adobeCC";
    request.item.launch->target = "Photoshop";
    const auto adobe = launcher::prepare_item_presentation(request);
    CHECK_FALSE(adobe.launch.action);
    CHECK(adobe.launch.result.error_code == ERROR_NOT_READY);
    CHECK(adobe.label == L"Photoshop");
}

TEST_CASE("Presentation rejects unresolved actions supplied accidentally", "[presentation][native]")
{
    Fixture fixture;
    fixture.write(L"Version 2/tool.exe", "This matching target must not be discovered.");
    auto request = fixture.request();
    launcher::LaunchAction action;
    action.target = "Version */tool.exe";
    request.item.launch = action;
    request.action = action;
    const auto wildcard = launcher::prepare_item_presentation(request);
    CHECK_FALSE(wildcard.launch.action);
    CHECK(wildcard.launch.result.error_code == ERROR_INVALID_PARAMETER);
    CHECK_FALSE(wildcard.diagnostics.empty());

    request.action->handler = "adobeCC";
    request.action->target = "Photoshop";
    const auto adobe = launcher::prepare_item_presentation(request);
    CHECK_FALSE(adobe.launch.action);
    CHECK(adobe.launch.result.error_code == ERROR_INVALID_PARAMETER);
}

TEST_CASE("Presentation keeps configured launch options and executable metadata", "[presentation][native]")
{
    Fixture fixture;
    const auto probe = fixture.probe();
    REQUIRE(fs::create_directory(fixture.root / L"Working folder"));
    auto request = fixture.request();
    launcher::LaunchAction action;
    action.target = "./" + utf8(probe.filename().wstring());
    action.arguments = "\"A test argument\" --presentation-only";
    action.working_directory = "Working folder";
    action.show = launcher::LaunchShow::minimized;
    request.item.launch = action;
    request.item.icon_explicit = true;
    request.source = launcher::StockIcon::information;
    request.action = action;
    const auto result = launcher::prepare_item_presentation(request);
    REQUIRE(result.launch.action);
    CHECK(result.launch.result.success);
    CHECK(result.launch.action->target == probe.wstring());
    CHECK(result.launch.action->arguments == L"\"A test argument\" --presentation-only");
    CHECK(result.launch.action->working_directory == (fixture.root / L"Working folder").wstring());
    CHECK(result.launch.action->show == launcher::LaunchShow::minimized);
    CHECK(result.label == L"Launcher");
    CHECK(result.can_admin);
    CHECK(result.can_reveal);
    complete_pixels(result, request.pixel_size);
}

TEST_CASE("Alternate-only items infer their program icon without a normal action", "[presentation][native][shift-target]")
{
    Fixture fixture;
    const auto probe = fixture.probe();
    auto request = fixture.request();
    launcher::LaunchAction action;
    action.target = utf8(probe.filename().wstring());
    request.item.launch = action;
    request.item.shift_launch = action;
    request.action = action;
    const auto normal = launcher::prepare_item_presentation(request);
    REQUIRE(normal.diagnostics.empty());
    complete_pixels(normal, request.pixel_size);

    request.item.launch.reset();
    request.action.reset();
    const auto alternate = launcher::prepare_item_presentation(request);
    REQUIRE(alternate.diagnostics.empty());
    CHECK(alternate.pixels == normal.pixels);
    CHECK_FALSE(alternate.launch.action);
    CHECK_FALSE(alternate.can_admin);
    CHECK_FALSE(alternate.can_reveal);

    request.item.icon_explicit = true;
    request.source = launcher::StockIcon::information;
    const auto explicit_icon = launcher::prepare_item_presentation(request);
    request.item.shift_launch.reset();
    const auto decoration = launcher::prepare_item_presentation(request);
    CHECK(explicit_icon.pixels == decoration.pixels);
    CHECK(explicit_icon.pixels != alternate.pixels);
}

TEST_CASE("Presentation honors explicit empty labels and explicit icon artwork", "[presentation][native]")
{
    Fixture fixture;
    const auto svg = fixture.write(L"Color 资料.svg", "<svg xmlns='http://www.w3.org/2000/svg' width='24' height='24'><rect width='24' height='24' fill='#204080' fill-opacity='.5'/></svg>");
    auto request = fixture.request();
    launcher::LaunchAction action;
    action.target = "https://example.invalid/presentation-test";
    request.item.launch = action;
    request.item.label_explicit = true;
    request.item.icon_explicit = true;
    request.source = launcher::FileIcon{svg};
    request.action = action;
    auto result = launcher::prepare_item_presentation(request);
    REQUIRE(result.launch.action);
    CHECK(result.label.empty());
    CHECK_FALSE(result.can_admin);
    CHECK_FALSE(result.can_reveal);
    complete_pixels(result, request.pixel_size);
    const std::size_t center = (request.pixel_size / 2 * result.stride) + (request.pixel_size / 2 * 4);
    const std::array<unsigned, 4> expected{64, 32, 16, 128};
    for (std::size_t channel = 0; channel < expected.size(); ++channel) {
        CHECK(std::abs(static_cast<int>(result.pixels[center + channel]) - static_cast<int>(expected[channel])) <= 2);
    }
    request.item.label = "A supplied label";
    result = launcher::prepare_item_presentation(request);
    CHECK(result.label == L"A supplied label");
}

TEST_CASE("Presentation pixels remain owned after the worker apartment exits", "[presentation][native]")
{
    Fixture fixture;
    auto request = fixture.request();
    request.item.label = "An owned pixel snapshot";
    auto result = std::async(std::launch::async, [request] { return launcher::prepare_item_presentation(request); }).get();
    complete_pixels(result, request.pixel_size);
    const auto expected = result.pixels;
    const Apartment apartment(COINIT_APARTMENTTHREADED);
    REQUIRE(SUCCEEDED(apartment.result()));
    {
        ComPtr<IWICImagingFactory> factory;
        REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()))));
        ComPtr<IWICBitmap> image;
        REQUIRE(SUCCEEDED(factory->CreateBitmapFromMemory(result.width, result.height, GUID_WICPixelFormat32bppPBGRA,
            result.stride, static_cast<UINT>(result.pixels.size()), result.pixels.data(), image.GetAddressOf())));
        std::vector<std::uint8_t> copied(expected.size());
        REQUIRE(SUCCEEDED(image->CopyPixels(nullptr, result.stride, static_cast<UINT>(copied.size()), copied.data())));
        CHECK(copied == expected);
    }
}

TEST_CASE("Presentation icon failures retain prepared actions and use a stock image", "[presentation][native]")
{
    Fixture fixture;
    const auto probe = fixture.probe();
    auto request = fixture.request();
    launcher::LaunchAction action;
    action.target = utf8(probe.wstring());
    request.item.launch = action;
    request.item.icon_explicit = true;
    request.source = launcher::FileIcon{fixture.root / L"Missing artwork.svg"};
    request.action = action;
    const auto result = launcher::prepare_item_presentation(request);
    REQUIRE(result.launch.action);
    CHECK(result.launch.action->target == probe.wstring());
    CHECK(result.can_admin);
    CHECK(result.can_reveal);
    CHECK_FALSE(result.diagnostics.empty());
    CHECK(std::ranges::any_of(result.diagnostics, [&](const auto& message) {
        return message.find(utf8((fixture.root / L"Missing artwork.svg").wstring())) != std::string::npos;
    }));
    complete_pixels(result, request.pixel_size);
}

TEST_CASE("Presentation caches file action capabilities without requiring an image", "[presentation][native]")
{
    Fixture fixture;
    const auto document = fixture.write(L"Document 资料.txt", "A document can be revealed but not elevated.");
    auto request = fixture.request();
    launcher::LaunchAction action;
    action.target = utf8(document.wstring());
    request.item.launch = action;
    request.item.icon_explicit = true;
    request.action = action;
    const auto result = launcher::prepare_item_presentation(request);
    REQUIRE(result.launch.action);
    CHECK(result.label == document.filename().wstring());
    CHECK_FALSE(result.can_admin);
    CHECK(result.can_reveal);
    REQUIRE(fs::remove(document));
    // The completed result is a snapshot; no later filesystem change mutates it.
    CHECK(result.can_reveal);
    const auto missing = launcher::prepare_item_presentation(request);
    REQUIRE(missing.launch.action);
    CHECK_FALSE(missing.can_reveal);
    CHECK_FALSE(missing.can_admin);
}

TEST_CASE("Presentation rejects unbounded output sizes before decoding", "[presentation][native]")
{
    launcher::ItemPresentationRequest request;
    for (const std::uint32_t invalid : {0U, 4097U, std::numeric_limits<std::uint32_t>::max()}) {
        request.pixel_size = invalid;
        CHECK_THROWS_AS(launcher::prepare_item_presentation(request), std::invalid_argument);
    }
}

TEST_CASE("Presentation apartment failure does not invalidate a prepared action", "[presentation][native]")
{
    Fixture fixture;
    auto request = fixture.request();
    launcher::LaunchAction action;
    action.target = "https://example.invalid/presentation-test";
    request.item.launch = action;
    request.action = action;
    const auto result = std::async(std::launch::async, [request] {
        const Apartment apartment(COINIT_MULTITHREADED);
        if (FAILED(apartment.result())) { throw std::runtime_error("Cannot enter MTA fixture apartment."); }
        return launcher::prepare_item_presentation(request);
    }).get();
    REQUIRE(result.launch.action);
    CHECK(result.launch.result.success);
    CHECK_FALSE(result.diagnostics.empty());
    CHECK(result.pixels.empty());
    CHECK(result.width == 0);
    CHECK(result.height == 0);
    CHECK(result.stride == 0);
}
