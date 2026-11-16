#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/pending_overlay.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;

class OverlayFixture final {
public:
    OverlayFixture() : apartment_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))
    {
        REQUIRE(SUCCEEDED(apartment_));
        REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()))));
    }
    ~OverlayFixture()
    {
        factory.Reset();
        if (SUCCEEDED(apartment_)) { CoUninitialize(); }
    }
    ComPtr<IWICImagingFactory> factory;
private:
    HRESULT apartment_;
};
} // namespace

TEST_CASE("Pending blinds preserve transparent gaps and scale density independently of DPI", "[overlay][native]")
{
    OverlayFixture fixture;
    struct Sample { float logical; UINT pixels; unsigned slats; };
    for (const auto sample : std::array{Sample{24, 48, 4}, Sample{48, 96, 5}, Sample{96, 192, 6}}) {
        CAPTURE(sample.logical, sample.pixels);
        REQUIRE(launcher::pending_overlay_slats(sample.logical) == sample.slats);
        ComPtr<IWICBitmapSource> image;
        REQUIRE(SUCCEEDED(launcher::render_pending_overlay(fixture.factory.Get(), sample.logical, sample.pixels, image)));
        UINT width = 0, height = 0;
        REQUIRE(SUCCEEDED(image->GetSize(&width, &height)));
        REQUIRE(width == sample.pixels);
        REQUIRE(height == sample.pixels);
        GUID format{};
        REQUIRE(SUCCEEDED(image->GetPixelFormat(&format)));
        REQUIRE(format == GUID_WICPixelFormat32bppPBGRA);
        std::vector<BYTE> pixels(width * height * 4);
        REQUIRE(SUCCEEDED(image->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data())));
        unsigned bands = 0;
        bool in_band = false;
        bool transparent_gap = false;
        for (UINT y = 0; y < height; ++y) {
            const auto offset = (static_cast<std::size_t>(y) * width + width / 2) * 4;
            const auto alpha = pixels[offset + 3];
            const bool band = alpha > 32;
            if (band && !in_band) { ++bands; }
            in_band = band;
            transparent_gap = transparent_gap || alpha == 0;
            // WIC input remains correctly premultiplied for D2D composition.
            CHECK(pixels[offset] <= alpha);
            CHECK(pixels[offset + 1] <= alpha);
            CHECK(pixels[offset + 2] <= alpha);
        }
        CHECK(bands == sample.slats);
        CHECK(transparent_gap);
    }
}

TEST_CASE("Pending overlay rejects invalid extents without leaving an old bitmap", "[overlay][native]")
{
    OverlayFixture fixture;
    ComPtr<IWICBitmapSource> image;
    REQUIRE(SUCCEEDED(launcher::render_pending_overlay(fixture.factory.Get(), 32, 32, image)));
    REQUIRE(image);
    CHECK(launcher::render_pending_overlay(fixture.factory.Get(), std::numeric_limits<float>::quiet_NaN(), 32, image) == E_INVALIDARG);
    CHECK_FALSE(image);
    CHECK(launcher::render_pending_overlay(fixture.factory.Get(), 0, 32, image) == E_INVALIDARG);
    CHECK(launcher::render_pending_overlay(fixture.factory.Get(), 32, 4097, image) == E_INVALIDARG);
    CHECK(launcher::pending_overlay_slats(32) == 4);
    CHECK(launcher::pending_overlay_slats(64) == 5);
    CHECK(launcher::pending_overlay_slats(65) == 6);
}
