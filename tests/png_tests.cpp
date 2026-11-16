#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/png.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

class PngFixture final {
public:
    PngFixture()
    {
        apartment_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        REQUIRE(SUCCEEDED(apartment_));
        REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf()))));
        parent_ = fs::weakly_canonical(fs::temp_directory_path());
        root = parent_ / (L"LauncherPng-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        REQUIRE(fs::create_directory(root));
        // Transparent gap, partial-opacity color, opaque color, and a very faint shadow.
        std::array<BYTE, 16> pixels{0, 0, 0, 0, 16, 32, 64, 128, 10, 20, 30, 255, 1, 1, 1, 1};
        REQUIRE(SUCCEEDED(factory->CreateBitmapFromMemory(4, 1, GUID_WICPixelFormat32bppPBGRA,
            16, static_cast<UINT>(pixels.size()), pixels.data(), image.GetAddressOf())));
    }
    ~PngFixture()
    {
        image.Reset();
        factory.Reset();
        std::error_code error;
        const auto resolved = fs::weakly_canonical(root, error);
        if (!error && resolved == root && resolved.parent_path() == parent_) { fs::remove_all(root, error); }
        if (SUCCEEDED(apartment_)) { CoUninitialize(); }
    }
    std::vector<char> bytes(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.good());
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> image;
    fs::path root;
private:
    fs::path parent_;
    HRESULT apartment_ = E_FAIL;
};
} // namespace

TEST_CASE("PNG snapshots preserve alpha, straight colors, dimensions and DPI", "[snapshot]")
{
    PngFixture fixture;
    const auto path = fixture.root / L"Window 资料.png";
    REQUIRE(SUCCEEDED(launcher::write_png(fixture.factory.Get(), fixture.image.Get(), path, 144.0)));
    ComPtr<IWICBitmapDecoder> decoder;
    REQUIRE(SUCCEEDED(fixture.factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())));
    GUID container{};
    REQUIRE(SUCCEEDED(decoder->GetContainerFormat(&container)));
    REQUIRE(container == GUID_ContainerFormatPng);
    ComPtr<IWICBitmapFrameDecode> frame;
    REQUIRE(SUCCEEDED(decoder->GetFrame(0, frame.GetAddressOf())));
    UINT width = 0;
    UINT height = 0;
    REQUIRE(SUCCEEDED(frame->GetSize(&width, &height)));
    REQUIRE(width == 4);
    REQUIRE(height == 1);
    double xdpi = 0;
    double ydpi = 0;
    REQUIRE(SUCCEEDED(frame->GetResolution(&xdpi, &ydpi)));
    REQUIRE(std::abs(xdpi - 144.0) < 0.1);
    REQUIRE(std::abs(ydpi - 144.0) < 0.1);
    ComPtr<IWICBitmapSource> straight;
    REQUIRE(SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame.Get(), straight.GetAddressOf())));
    std::array<BYTE, 16> pixels{};
    REQUIRE(SUCCEEDED(straight->CopyPixels(nullptr, 16, static_cast<UINT>(pixels.size()), pixels.data())));
    REQUIRE(pixels[3] == 0);
    REQUIRE(pixels[7] == 128);
    REQUIRE(std::abs(int(pixels[4]) - 32) <= 1);
    REQUIRE(std::abs(int(pixels[5]) - 64) <= 1);
    REQUIRE(std::abs(int(pixels[6]) - 128) <= 1);
    REQUIRE(pixels[8] == 10);
    REQUIRE(pixels[9] == 20);
    REQUIRE(pixels[10] == 30);
    REQUIRE(pixels[11] == 255);
    REQUIRE(pixels[15] == 1);
}

TEST_CASE("Snapshot timestamps create folders and never replace an existing image", "[snapshot]")
{
    PngFixture fixture;
    const auto directory = fixture.root / L"Pictures 资料" / L"Launcher";
    SYSTEMTIME time{2026, 10, 5, 16, 14, 35, 22, 123};
    fs::path first;
    fs::path second;
    REQUIRE(SUCCEEDED(launcher::save_snapshot_png(fixture.factory.Get(), fixture.image.Get(), directory, 120, time, first)));
    REQUIRE(first.filename() == L"Launcher-2026-10-16_14-35-22-123.png");
    const auto original = fixture.bytes(first);
    REQUIRE(FAILED(launcher::write_png(fixture.factory.Get(), fixture.image.Get(), first, 96)));
    REQUIRE(fixture.bytes(first) == original);
    REQUIRE(SUCCEEDED(launcher::save_snapshot_png(fixture.factory.Get(), fixture.image.Get(), directory, 120, time, second)));
    REQUIRE(second.filename() == L"Launcher-2026-10-16_14-35-22-123-2.png");
    REQUIRE(fixture.bytes(first) == original);
    REQUIRE(fixture.bytes(second) == original);
}

TEST_CASE("Snapshot save failures preserve existing files and report no saved image", "[snapshot]")
{
    PngFixture fixture;
    const auto blocked = fixture.root / L"blocked";
    { std::ofstream output(blocked); output << "preserve"; }
    const auto original = fixture.bytes(blocked);
    SYSTEMTIME time{2026, 10, 5, 16, 14, 35, 22, 123};
    fs::path written = L"old.png";
    REQUIRE(FAILED(launcher::save_snapshot_png(fixture.factory.Get(), fixture.image.Get(), blocked, 96, time, written)));
    REQUIRE(written.empty());
    REQUIRE(fixture.bytes(blocked) == original);
    const auto invalid = fixture.root / L"invalid.png";
    REQUIRE(FAILED(launcher::write_png(fixture.factory.Get(), nullptr, invalid, 96)));
    REQUIRE_FALSE(fs::exists(invalid));
}

TEST_CASE("PNG exports keep their exact destination and existing-file protection with a search context", "[snapshot]")
{
    PngFixture fixture;
    const auto program = fixture.root / L"portable";
    const auto user = fixture.root / L"user";
    REQUIRE(fs::create_directory(program));
    REQUIRE(fs::create_directory(user));
    const launcher::FileLookup files(program / L"Launcher.exe", user);
    const auto decoy = program / L"export.png";
    { std::ofstream output(decoy, std::ios::binary); output << "preserve this artwork"; }
    const auto original = fixture.bytes(decoy);
    const auto destination = fixture.root / L"export.png";
    REQUIRE(SUCCEEDED(launcher::write_png(fixture.factory.Get(), fixture.image.Get(), destination, 96, files)));
    const auto exported = fixture.bytes(destination);
    REQUIRE(FAILED(launcher::write_png(fixture.factory.Get(), fixture.image.Get(), destination, 144, files)));
    CHECK(fixture.bytes(destination) == exported);
    CHECK(fixture.bytes(decoy) == original);
    CHECK_FALSE(fs::exists(user / L"export.png"));
}
