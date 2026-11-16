#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/icon_images.hpp"
#include "launcher/pe_resources.hpp"
#include "launcher/resource_module.hpp"
#include "launcher/svg_images.hpp"
#include "launcher/config_error.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

launcher::PeResourceIcon source(launcher::ResourceIdentifier type, launcher::ResourceIdentifier name, std::optional<std::uint16_t> language = {})
{
    return {LAUNCHER_RESOURCE_FIXTURE, std::move(type), std::move(name), language};
}

std::vector<BYTE> asset(std::wstring_view name)
{
    std::ifstream input(fs::path(LAUNCHER_RESOURCE_ASSETS) / name, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class ResourceFixture final {
public:
    ResourceFixture()
    {
        apartment_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        REQUIRE(SUCCEEDED(apartment_));
        REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()))));
        images.initialize(factory.Get());
    }
    ~ResourceFixture()
    {
        images.initialize(nullptr);
        factory.Reset();
        if (SUCCEEDED(apartment_)) { CoUninitialize(); }
    }
    ComPtr<IWICImagingFactory> factory;
    launcher::IconImages images;
private:
    HRESULT apartment_ = E_FAIL;
};

std::vector<BYTE> pixels(IWICBitmapSource* image, UINT expected = 48)
{
    UINT width = 0;
    UINT height = 0;
    REQUIRE(SUCCEEDED(image->GetSize(&width, &height)));
    REQUIRE(width == expected);
    REQUIRE(height == expected);
    std::vector<BYTE> bytes(width * height * 4);
    REQUIRE(SUCCEEDED(image->CopyPixels(nullptr, width * 4, static_cast<UINT>(bytes.size()), bytes.data())));
    return bytes;
}
} // namespace

TEST_CASE("ResourceModule exposes exact binary and text payloads while owning the mapping", "[resource-module][pe-resource]")
{
    launcher::ResourceModule module{fs::path(LAUNCHER_RESOURCE_FIXTURE)};
    const auto bytes = module.FetchBin(std::string("SVG"), std::string("ARTWORK"), std::uint16_t{0});
    const auto expected = asset(L"artwork.svg");
    REQUIRE(std::vector<BYTE>(bytes.begin(), bytes.end()) == expected);
    const auto text = module.FetchText(std::string("SVG"), std::string("ARTWORK"), std::uint16_t{0});
    REQUIRE(text == std::string(expected.begin(), expected.end()));
    REQUIRE(text.data() == reinterpret_cast<const char*>(bytes.data()));
    const auto binary = module.FetchBin(std::uint16_t{10}, std::string("BLOB"));
    REQUIRE(std::vector<BYTE>(binary.begin(), binary.end()) == asset(L"binary.bin"));
    const auto binary_text = module.FetchText(std::uint16_t{10}, std::string("BLOB"));
    REQUIRE(binary_text.size() == binary.size());
    REQUIRE(binary_text == std::string(binary.begin(), binary.end()));
    REQUIRE(binary_text.find('\0') != std::string_view::npos);
    const auto numeric = module.FetchBin(std::uint16_t{301}, std::uint16_t{7});
    REQUIRE(std::vector<BYTE>(numeric.begin(), numeric.end()) == asset(L"artwork.png"));
    const auto unicode = module.FetchBin(std::string("图片"), std::string("图标"));
    REQUIRE(std::vector<BYTE>(unicode.begin(), unicode.end()) == asset(L"artwork.png"));
}

TEST_CASE("ResourceModule keeps type name and explicit language selections distinct", "[resource-module][pe-resource]")
{
    launcher::ResourceModule module{fs::path(LAUNCHER_RESOURCE_FIXTURE)};
    const auto original = module.FetchText(std::string("SVG"), std::string("ARTWORK"));
    const auto other_type = module.FetchText(std::string("OTHER"), std::string("ARTWORK"));
    REQUIRE(original != other_type);
    const auto neutral = module.FetchText(std::uint16_t{10}, std::string("LOCALIZED"), std::uint16_t{0});
    const auto english = module.FetchText(std::uint16_t{10}, std::string("LOCALIZED"), std::uint16_t{1033});
    REQUIRE(neutral == original);
    REQUIRE(english == other_type);
    REQUIRE(module.FetchText(std::uint16_t{10}, std::string("ENGLISH_ONLY")) == english);
    REQUIRE_THROWS_AS(module.FetchText(std::uint16_t{10}, std::string("ENGLISH_ONLY"), std::uint16_t{0}), std::system_error);
    REQUIRE_THROWS_AS(module.FetchBin(std::uint16_t{10}, std::string("LOCALIZED"), std::uint16_t{1041}), std::system_error);
    REQUIRE_THROWS_AS(module.FetchBin(std::string("MISSING"), std::string("ARTWORK")), std::system_error);
    REQUIRE_THROWS_AS(module.FetchBin(std::string("SVG"), std::string("MISSING")), std::system_error);
    REQUIRE_THROWS_AS(module.FetchBin(std::string("301"), std::uint16_t{7}), std::system_error);
    REQUIRE_THROWS_AS(module.FetchBin(std::uint16_t{301}, std::string("7")), std::system_error);
    // Failed lookups leave both the owner and existing borrowed views usable.
    const auto repeated = module.FetchText(std::string("SVG"), std::string("ARTWORK"));
    REQUIRE(repeated.data() == original.data());
    REQUIRE(repeated == original);
    REQUIRE(module.FetchText(std::uint16_t{10}, std::string("LOCALIZED"), std::uint16_t{1033}).data() == english.data());
    const auto other_name = module.FetchBin(std::string("SVG"), std::string("PNG_IN_SVG"));
    REQUIRE(std::vector<BYTE>(other_name.begin(), other_name.end()) == asset(L"artwork.png"));
    REQUIRE(other_name.data() != reinterpret_cast<const BYTE*>(original.data()));
}

TEST_CASE("External resource copies outlive their module and missing modules throw", "[resource-module][pe-resource]")
{
    std::vector<BYTE> owned;
    {
        launcher::ResourceModule module{fs::path(LAUNCHER_RESOURCE_FIXTURE)};
        const auto borrowed = module.FetchBin(std::string("SVG"), std::string("ARTWORK"));
        owned.assign(borrowed.begin(), borrowed.end());
    }
    REQUIRE(owned == asset(L"artwork.svg"));
    auto missing = fs::path(LAUNCHER_RESOURCE_FIXTURE);
    missing += L".missing";
    REQUIRE_THROWS_AS(launcher::ResourceModule{missing}, std::system_error);
    // The owning convenience API must retain bytes after its local module dies.
    const auto extracted = launcher::read_pe_resource(source(std::string("SVG"), std::string("ARTWORK")));
    REQUIRE(extracted == owned);
    auto relative = fs::path(LAUNCHER_RESOURCE_FIXTURE).filename();
    REQUIRE_NOTHROW(launcher::resolve_icon_path(relative, fs::path(LAUNCHER_RESOURCE_FIXTURE).parent_path()));
    REQUIRE(relative == fs::path(LAUNCHER_RESOURCE_FIXTURE));
}

TEST_CASE("Explicit icon paths preserve normalized literal percent and dollar text", "[icons][pe-resource][percent]")
{
    ResourceFixture fixture;
    const auto parent = fs::weakly_canonical(fs::temp_directory_path());
    const auto directory = parent / (L"LauncherPercentIcons-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    REQUIRE(fs::create_directory(directory));
    struct Cleanup {
        fs::path directory, parent;
        ~Cleanup() {
            std::error_code error;
            const auto resolved = fs::weakly_canonical(directory, error);
            if (!error && resolved == directory && resolved.parent_path() == parent) { fs::remove_all(directory, error); }
        }
    } cleanup{directory, parent};
    const auto png = directory / L"图片 %WINDIR% $NAME.png";
    const auto dll = directory / L"资源 %WINDIR% $NAME.dll";
    REQUIRE(fs::copy_file(fs::path(LAUNCHER_RESOURCE_ASSETS) / L"artwork.png", png));
    REQUIRE(fs::copy_file(fs::path(LAUNCHER_RESOURCE_FIXTURE), dll));
    Microsoft::WRL::ComPtr<IWICBitmapSource> image, expected;
    fixture.images.load(launcher::FileIcon{fs::path(LAUNCHER_RESOURCE_ASSETS) / L"artwork.png"}, 48, expected);
    fixture.images.load(launcher::FileIcon{png}, 48, image);
    REQUIRE(pixels(image.Get()) == pixels(expected.Get()));
    auto resource = source(std::string("SVG"), std::string("ARTWORK"));
    fixture.images.load(resource, 48, expected);
    resource.path = dll;
    fixture.images.load(resource, 48, image);
    REQUIRE(pixels(image.Get()) == pixels(expected.Get()));
    auto literal = fs::path(L"%WINDIR% $NAME.ico");
    launcher::resolve_icon_path(literal, directory);
    REQUIRE(literal == directory / L"%WINDIR% $NAME.ico");
}

TEST_CASE("PE resources preserve exact bytes across names IDs types and languages", "[icons][pe-resource]")
{
    struct Case { launcher::PeResourceIcon icon; const wchar_t* expected; };
    for (const auto& test : std::array{
            Case{source(std::string("SVG"), std::string("ARTWORK"), std::uint16_t{0}), L"artwork.svg"},
            Case{source(std::string("OTHER"), std::string("ARTWORK")), L"alternate.svg"},
            Case{source(std::uint16_t(301), std::uint16_t(7)), L"artwork.png"},
            Case{source(std::uint16_t(10), std::string("BLOB")), L"binary.bin"},
            Case{source(std::uint16_t(10), std::string("LOCALIZED"), std::uint16_t{0}), L"artwork.svg"},
            Case{source(std::uint16_t(10), std::string("LOCALIZED"), std::uint16_t{1033}), L"alternate.svg"},
            Case{source(std::uint16_t(10), std::string("ENGLISH_ONLY")), L"alternate.svg"},
            Case{source(std::string("图片"), std::string("图标")), L"artwork.png"}}) {
        INFO(fs::path(test.expected).string());
        const auto bytes = launcher::read_pe_resource(test.icon);
        // The module has already been released here. Bytes own their storage.
        REQUIRE(bytes == asset(test.expected));
    }
    // Explicit language zero requires an actual neutral entry, rather than
    // silently accepting the English resource selected by Windows fallback.
    REQUIRE_THROWS_AS(launcher::read_pe_resource(source(std::uint16_t(10), std::string("LOCALIZED"), std::uint16_t{1041})), std::system_error);
    REQUIRE_THROWS_AS(launcher::read_pe_resource(source(std::uint16_t(10), std::string("ENGLISH_ONLY"), std::uint16_t{0})), std::system_error);
}

TEST_CASE("PE SVG and PNG payloads match file decoding and preserve visible pixels", "[icons][pe-resource]")
{
    ResourceFixture fixture;
    struct Case { launcher::PeResourceIcon icon; const wchar_t* file; };
    for (const auto& test : std::array{
            Case{source(std::string("SVG"), std::string("ARTWORK")), L"artwork.svg"},
            Case{source(std::uint16_t(10), std::string("COMMENTS")), L"commented.svg"},
            Case{source(std::uint16_t(301), std::uint16_t(7)), L"artwork.png"},
            Case{source(std::string("SVG"), std::string("PNG_IN_SVG")), L"artwork.png"}}) {
        ComPtr<IWICBitmapSource> resource;
        ComPtr<IWICBitmapSource> file;
        INFO(fs::path(test.file).string());
        REQUIRE_NOTHROW(fixture.images.load(test.icon, 48, resource));
        REQUIRE_NOTHROW(fixture.images.load(launcher::FileIcon{fs::path(LAUNCHER_RESOURCE_ASSETS) / test.file}, 48, file));
        REQUIRE(pixels(resource.Get()) == pixels(file.Get()));
        const auto bytes = pixels(resource.Get());
        const auto offset = [](UINT x, UINT y) { return (y * 48 + x) * 4; };
        REQUIRE(bytes[offset(12, 12) + 2] == 255); // Red interior.
        REQUIRE(bytes[offset(36, 36)] == 255);     // Blue interior.
        REQUIRE(bytes[offset(0, 0) + 3] == 0);    // Transparent corner.
        REQUIRE(std::abs(int(bytes[offset(36, 12) + 3]) - 128) <= 1);
    }
}

TEST_CASE("PE cache identity includes type name and language and resolves relative paths", "[icons][pe-resource]")
{
    ResourceFixture fixture;
    const auto first = source(std::string("SVG"), std::string("ARTWORK"), std::uint16_t{0});
    auto other_type = first;
    other_type.type = std::string("OTHER");
    auto localized = source(std::uint16_t(10), std::string("LOCALIZED"), std::uint16_t{0});
    auto english = localized;
    english.language = std::uint16_t{1033};
    ComPtr<IWICBitmapSource> red;
    ComPtr<IWICBitmapSource> yellow;
    ComPtr<IWICBitmapSource> neutral;
    ComPtr<IWICBitmapSource> translated;
    ComPtr<IWICBitmapSource> repeated;
    REQUIRE_NOTHROW(fixture.images.load(first, 48, red));
    REQUIRE_NOTHROW(fixture.images.load(other_type, 48, yellow));
    REQUIRE_NOTHROW(fixture.images.load(localized, 48, neutral));
    REQUIRE_NOTHROW(fixture.images.load(english, 48, translated));
    REQUIRE(pixels(red.Get()) != pixels(yellow.Get()));
    REQUIRE(pixels(neutral.Get()) != pixels(translated.Get()));
    REQUIRE_NOTHROW(fixture.images.load(first, 48, repeated));
    REQUIRE(red.Get() == repeated.Get());
    auto relative = first;
    relative.path = relative.path.filename();
    REQUIRE_NOTHROW(fixture.images.load(relative, 48, repeated, fs::path(LAUNCHER_RESOURCE_FIXTURE).parent_path()));
    REQUIRE(red.Get() == repeated.Get());
    auto other_name = first;
    other_name.name = std::string("PNG_IN_SVG");
    REQUIRE_NOTHROW(fixture.images.load(other_name, 48, repeated));
    REQUIRE(red.Get() != repeated.Get());
}

TEST_CASE("PE missing selectors and non-image data fail without substituting artwork", "[icons][pe-resource]")
{
    ResourceFixture fixture;
    for (auto invalid : std::array{source(std::string("MISSING"), std::string("ARTWORK")),
            source(std::string("SVG"), std::string("MISSING")),
            source(std::uint16_t(10), std::string("LOCALIZED"), std::uint16_t{1041}),
            source(std::uint16_t(10), std::string("BLOB")),
            source(std::uint16_t(10), std::string("BROKEN"))}) {
        ComPtr<IWICBitmapSource> image;
        REQUIRE_THROWS_AS(fixture.images.load(invalid, 48, image), std::system_error);
        REQUIRE_FALSE(image);
    }
    auto missing = source(std::string("SVG"), std::string("ARTWORK"));
    missing.path += L".missing";
    REQUIRE_THROWS_AS(launcher::read_pe_resource(missing), launcher::ConfigError);
}

TEST_CASE("SVG sniffing recognizes the root beyond the closing-tag shortcut", "[icons][pe-resource][svg]")
{
    ResourceFixture fixture;
    for (const auto name : {L"artwork.svg", L"commented.svg", L"alternate.svg"}) {
        REQUIRE(launcher::is_svg_image(asset(name)));
    }
    for (const auto name : {L"artwork.png", L"binary.bin"}) { REQUIRE_FALSE(launcher::is_svg_image(asset(name))); }
    for (const auto xml : {"<svg xmlns='http://www.w3.org/2000/svg'/> ",
            "<?xml version='1.0'?><!--before--><s:svg xmlns:s='http://www.w3.org/2000/svg'></s:svg><!--after-->",
            "<svg xmlns='http://www.w3.org/2000/svg'></svg >"}) {
        REQUIRE(launcher::is_svg_image(std::span(reinterpret_cast<const BYTE*>(xml), std::char_traits<char>::length(xml))));
    }
    for (const auto xml : {"<html><svg/></html>", "<svg xmlns='urn:other'/>"}) {
        REQUIRE_FALSE(launcher::is_svg_image(std::span(reinterpret_cast<const BYTE*>(xml), std::char_traits<char>::length(xml))));
    }
}
