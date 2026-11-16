#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/icon_images.hpp"
#include "launcher/config_error.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

void number(std::vector<BYTE>& bytes, std::uint32_t value, unsigned count)
{
    for (unsigned index = 0; index < count; ++index) { bytes.push_back(static_cast<BYTE>(value >> (index * 8))); }
}

std::vector<BYTE> image(UINT size, BYTE blue, BYTE green, BYTE red)
{
    std::vector<BYTE> bytes;
    number(bytes, 40, 4);
    number(bytes, size, 4);
    number(bytes, size * 2, 4);
    number(bytes, 1, 2);
    number(bytes, 32, 2);
    bytes.resize(40, 0);
    for (UINT pixel = 0; pixel < size * size; ++pixel) { bytes.insert(bytes.end(), {blue, green, red, 255}); }
    bytes.resize(bytes.size() + ((size + 31) / 32) * 4 * size, 0);
    return bytes;
}

class Fixture final {
public:
    Fixture()
    {
        apartment_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        REQUIRE(SUCCEEDED(apartment_));
        REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf()))));
        cache.initialize(factory.Get());
        parent_ = fs::weakly_canonical(fs::temp_directory_path());
        root = parent_ / (L"LauncherImages-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        REQUIRE(fs::create_directory(root));
    }
    ~Fixture()
    {
        cache.initialize(nullptr);
        factory.Reset();
        std::error_code error;
        const auto resolved = fs::weakly_canonical(root, error);
        if (!error && resolved == root && resolved.parent_path() == parent_) { fs::remove_all(root, error); }
        if (SUCCEEDED(apartment_)) { CoUninitialize(); }
    }
    fs::path ico(std::wstring_view name = L"colors.ico")
    {
        const auto path = root / name;
        std::vector<BYTE> bytes;
        number(bytes, 0, 2);
        number(bytes, 1, 2);
        number(bytes, 3, 2);
        const std::array images{image(16, 0, 0, 255), image(32, 0, 255, 0), image(64, 255, 0, 0)};
        UINT offset = 6 + 3 * 16;
        for (unsigned index = 0; index < images.size(); ++index) {
            const UINT size = 16U << index;
            bytes.insert(bytes.end(), {static_cast<BYTE>(size), static_cast<BYTE>(size), 0, 0});
            number(bytes, 1, 2);
            number(bytes, 32, 2);
            number(bytes, static_cast<UINT>(images[index].size()), 4);
            number(bytes, offset, 4);
            offset += static_cast<UINT>(images[index].size());
        }
        for (const auto& pixels : images) { bytes.insert(bytes.end(), pixels.begin(), pixels.end()); }
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(output.good());
        return path;
    }
    fs::path svg(std::string_view content, std::wstring_view name = L"Artwork 资料.svg")
    {
        const auto path = root / name;
        std::ofstream output(path, std::ios::binary);
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        REQUIRE(output.good());
        return path;
    }
    fs::path executable()
    {
        std::wstring original(32768, L'\0');
        const DWORD count = GetModuleFileNameW(nullptr, original.data(), static_cast<DWORD>(original.size()));
        REQUIRE(count > 0);
        original.resize(count);
        const auto path = root / L"Native colors 资料.exe";
        REQUIRE(fs::copy_file(original, path));
        HANDLE update = BeginUpdateResourceW(path.c_str(), TRUE);
        REQUIRE(update != nullptr);
        std::vector<BYTE> group;
        number(group, 0, 2);
        number(group, 1, 2);
        number(group, 3, 2);
        const std::array images{image(16, 0, 0, 255), image(32, 0, 255, 0), image(64, 255, 0, 0)};
        bool success = true;
        for (unsigned index = 0; index < images.size(); ++index) {
            const UINT size = 16U << index;
            group.insert(group.end(), {static_cast<BYTE>(size), static_cast<BYTE>(size), 0, 0});
            number(group, 1, 2);
            number(group, 32, 2);
            number(group, static_cast<UINT>(images[index].size()), 4);
            number(group, index + 1, 2);
            success &= UpdateResourceW(update, RT_ICON, MAKEINTRESOURCEW(index + 1), 0,
                const_cast<BYTE*>(images[index].data()), static_cast<DWORD>(images[index].size())) != FALSE;
        }
        success &= UpdateResourceW(update, RT_GROUP_ICON, MAKEINTRESOURCEW(101), 0,
            group.data(), static_cast<DWORD>(group.size())) != FALSE;
        // A second group contains just the red 16px frame. Group ID, group
        // index and individual RT_ICON frame ID deliberately differ.
        std::vector<BYTE> second(group.begin(), group.begin() + 20);
        second[4] = 1;
        success &= UpdateResourceW(update, RT_GROUP_ICON, MAKEINTRESOURCEW(202), 0,
            second.data(), static_cast<DWORD>(second.size())) != FALSE;
        success &= EndUpdateResourceW(update, !success) != FALSE;
        REQUIRE(success);
        return path;
    }

    ComPtr<IWICImagingFactory> factory;
    launcher::IconImages cache;
    fs::path root;
private:
    fs::path parent_;
    HRESULT apartment_ = E_FAIL;
};

std::array<BYTE, 4> pixel(IWICBitmapSource* bitmap, UINT x, UINT y)
{
    std::array<BYTE, 4> bytes{};
    WICRect rectangle{static_cast<INT>(x), static_cast<INT>(y), 1, 1};
    REQUIRE(SUCCEEDED(bitmap->CopyPixels(&rectangle, 4, static_cast<UINT>(bytes.size()), bytes.data())));
    return {bytes[0], bytes[1], bytes[2], bytes[3]};
}

void verify_sizes(Fixture& fixture, const launcher::IconSource& source)
{
    struct Case { UINT size; std::array<BYTE, 4> color; };
    const std::array cases{Case{16, {0, 0, 255, 255}}, Case{24, {0, 255, 0, 255}},
        Case{32, {0, 255, 0, 255}}, Case{48, {255, 0, 0, 255}}, Case{96, {255, 0, 0, 255}}};
    for (const auto& test : cases) {
        INFO("requested pixels: " << test.size);
        INFO("expected BGRA: " << static_cast<unsigned>(test.color[0]) << ',' << static_cast<unsigned>(test.color[1]) << ','
            << static_cast<unsigned>(test.color[2]) << ',' << static_cast<unsigned>(test.color[3]));
        ComPtr<IWICBitmapSource> bitmap;
        REQUIRE_NOTHROW(fixture.cache.load(source, test.size, bitmap));
        UINT width = 0;
        UINT height = 0;
        REQUIRE(SUCCEEDED(bitmap->GetSize(&width, &height)));
        REQUIRE(width == test.size);
        REQUIRE(height == test.size);
        const auto center = pixel(bitmap.Get(), test.size / 2, test.size / 2);
        const auto first = pixel(bitmap.Get(), 1, 1);
        const auto last = pixel(bitmap.Get(), test.size - 2, test.size - 2);
        INFO("actual BGRA: " << static_cast<unsigned>(center[0]) << ',' << static_cast<unsigned>(center[1]) << ','
            << static_cast<unsigned>(center[2]) << ',' << static_cast<unsigned>(center[3]));
        // WIC's HICON conversion and Fant resampling can round channels slightly.
        for (unsigned channel = 0; channel < 4; ++channel) {
            REQUIRE(std::abs(static_cast<int>(center[channel]) - test.color[channel]) <= 3);
            REQUIRE(std::abs(static_cast<int>(first[channel]) - test.color[channel]) <= 3);
            REQUIRE(std::abs(static_cast<int>(last[channel]) - test.color[channel]) <= 3);
        }
    }
}
} // namespace

TEST_CASE("ICO images select exact then next larger then largest smaller", "[icons][native]")
{
    Fixture fixture;
    verify_sizes(fixture, launcher::FileIcon{fixture.ico()});
}

TEST_CASE("An unreadable pinned icon reports its source instead of returning a cached image", "[icons][native]")
{
    Fixture fixture;
    const auto path = fixture.ico();
    ComPtr<IWICBitmapSource> bitmap;
    REQUIRE_NOTHROW(fixture.cache.load(launcher::FileIcon{path}, 48, bitmap));
    REQUIRE(bitmap);
    struct LockedFile final {
        HANDLE handle;
        ~LockedFile() { if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); } }
    } locked{CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    REQUIRE(locked.handle != INVALID_HANDLE_VALUE);
    REQUIRE_THROWS_AS(fixture.cache.load(launcher::FileIcon{path}, 48, bitmap), launcher::ConfigError);
    REQUIRE_FALSE(bitmap);
}

TEST_CASE("SVG icons render at the requested size with centered aspect ratio", "[icons][svg][native]")
{
    Fixture fixture;
    for (const auto view_box : {"", " viewBox='0 0 20 10'"}) {
        const auto path = fixture.svg(std::string("<svg xmlns='http://www.w3.org/2000/svg' width='20' height='10'") +
            view_box + "><rect width='20' height='10' fill='#ff0000'/></svg>");
        fixture.cache.clear();
        for (const UINT size : {60U, 240U}) {
            ComPtr<IWICBitmapSource> bitmap;
            REQUIRE_NOTHROW(fixture.cache.load(launcher::FileIcon{path}, size, bitmap));
            UINT width = 0;
            UINT height = 0;
            REQUIRE(SUCCEEDED(bitmap->GetSize(&width, &height)));
            REQUIRE(width == size);
            REQUIRE(height == size);
            const auto center = pixel(bitmap.Get(), size / 2, size / 2);
            const auto padding = pixel(bitmap.Get(), size / 2, size / 8);
            const auto left = pixel(bitmap.Get(), 2, size / 2);
            REQUIRE(center[2] == 255);
            REQUIRE(center[3] == 255);
            REQUIRE(padding[3] == 0);
            REQUIRE(left[2] == 255);
            ComPtr<IWICBitmapSource> cached;
            REQUIRE_NOTHROW(fixture.cache.load(launcher::FileIcon{path}, size, cached));
            REQUIRE(cached.Get() == bitmap.Get());
        }
    }
}

TEST_CASE("SVG paths gradients clipping transforms and opacity retain premultiplied alpha", "[icons][svg][native]")
{
    Fixture fixture;
    const auto path = fixture.svg(R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <linearGradient id="shade"><stop stop-color="#000000"/><stop offset="1" stop-color="#ffffff"/></linearGradient>
        <clipPath id="clip"><rect x="10" y="10" width="80" height="80"/></clipPath>
      </defs>
      <g transform="translate(0 5)" opacity="0.5" clip-path="url(#clip)">
        <path d="M0 0 H100 V100 H0 Z" fill="url(#shade)"/>
      </g>
    </svg>)svg");
    ComPtr<IWICBitmapSource> bitmap;
    REQUIRE_NOTHROW(fixture.cache.load(launcher::FileIcon{path}, 100, bitmap));
    const auto left = pixel(bitmap.Get(), 25, 50);
    const auto right = pixel(bitmap.Get(), 75, 50);
    const auto above = pixel(bitmap.Get(), 50, 12);
    INFO("left BGRA=" << static_cast<unsigned>(left[0]) << ',' << static_cast<unsigned>(left[1]) << ','
        << static_cast<unsigned>(left[2]) << ',' << static_cast<unsigned>(left[3]));
    INFO("right alpha=" << static_cast<unsigned>(right[3]) << "; padding alpha=" << static_cast<unsigned>(above[3]));
    REQUIRE(left[0] < right[0]);
    REQUIRE(left[3] >= 127);
    REQUIRE(left[3] <= 128);
    REQUIRE(right[0] <= right[3]);
    REQUIRE(above[3] == 0);
    for (const auto attributes : {"opacity='0.5'", "fill-opacity='0.5'", "style='opacity:0.5'"}) {
        const auto probe = fixture.svg(std::string("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><g ") +
            attributes + "><rect width='100' height='100' fill='red'/></g></svg>", L"Opacity.svg");
        fixture.cache.clear();
        REQUIRE_NOTHROW(fixture.cache.load(launcher::FileIcon{probe}, 100, bitmap));
        const auto sample = pixel(bitmap.Get(), 50, 50);
        INFO(attributes << " alpha=" << static_cast<unsigned>(sample[3]));
        CHECK(sample[3] <= 128);
    }
}

TEST_CASE("Invalid or empty SVG artwork fails without poisoning the image renderer", "[icons][svg][native]")
{
    Fixture fixture;
    ComPtr<IWICBitmapSource> bitmap;
    for (const auto content : {"<svg><broken", "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 10 10'/>"}) {
        const auto path = fixture.svg(content);
        REQUIRE_THROWS_AS(fixture.cache.load(launcher::FileIcon{path}, 48, bitmap), std::system_error);
        REQUIRE(!bitmap);
    }
    const auto valid = fixture.svg("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 10 10'><circle cx='5' cy='5' r='4'/></svg>");
    REQUIRE_NOTHROW(fixture.cache.load(launcher::FileIcon{valid}, 48, bitmap));
}

TEST_CASE("SVG embedded styles masks and installed fonts render", "[icons][svg][native]")
{
    Fixture fixture;
    const auto styled = fixture.svg(R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <style>.art { fill: #ff0000; }</style>
      <defs><mask id="half"><rect width="50" height="100" fill="white"/></mask></defs>
      <rect class="art" width="100" height="100" mask="url(#half)"/>
    </svg>)svg");
    ComPtr<IWICBitmapSource> bitmap;
    REQUIRE_NOTHROW(fixture.cache.load(launcher::FileIcon{styled}, 100, bitmap));
    const auto visible = pixel(bitmap.Get(), 25, 50);
    const auto masked = pixel(bitmap.Get(), 75, 50);
    REQUIRE(visible[2] == 255);
    REQUIRE(visible[3] == 255);
    REQUIRE(masked[3] == 0);
    const auto text = fixture.svg("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'>"
        "<text x='10' y='80' font-family='Arial' font-size='80'>A</text></svg>", L"Text.svg");
    REQUIRE_NOTHROW(fixture.cache.load(launcher::FileIcon{text}, 100, bitmap));
}

TEST_CASE("Executable icon resources are resized without Shell padding", "[icons][native]")
{
    Fixture fixture;
    verify_sizes(fixture, launcher::ShellItemIcon{fixture.executable()});
}

TEST_CASE("Explicit icon groups select by index or ID and keep distinct cache entries", "[icons][icon-group][native]")
{
    Fixture fixture;
    const auto path = fixture.root / L"Native colors %WINDIR% $NAME.exe";
    REQUIRE(fs::copy_file(fixture.executable(), path));
    const launcher::IconGroup first{path, launcher::IconIndex{0}};
    const launcher::IconGroup second{path, launcher::IconIndex{1}};
    const launcher::IconGroup identified{path, launcher::IconResourceId{202}};
    ComPtr<IWICBitmapSource> blue;
    ComPtr<IWICBitmapSource> red;
    ComPtr<IWICBitmapSource> by_id;
    ComPtr<IWICBitmapSource> repeated;
    REQUIRE_NOTHROW(fixture.cache.load(first, 48, blue));
    REQUIRE_NOTHROW(fixture.cache.load(second, 48, red));
    REQUIRE_NOTHROW(fixture.cache.load(identified, 48, by_id));
    REQUIRE(pixel(blue.Get(), 24, 24) == std::array<BYTE, 4>{255, 0, 0, 255});
    REQUIRE(pixel(red.Get(), 24, 24) == std::array<BYTE, 4>{0, 0, 255, 255});
    REQUIRE(pixel(by_id.Get(), 24, 24) == pixel(red.Get(), 24, 24));
    REQUIRE(blue.Get() != red.Get());
    REQUIRE_NOTHROW(fixture.cache.load(first, 48, repeated));
    REQUIRE(blue.Get() == repeated.Get());
    const launcher::IconGroup relative{path.filename(), launcher::IconResourceId{101}};
    REQUIRE_NOTHROW(fixture.cache.load(relative, 48, repeated, path.parent_path()));
    REQUIRE(pixel(repeated.Get(), 24, 24) == pixel(blue.Get(), 24, 24));
    for (const launcher::IconGroup invalid : {launcher::IconGroup{path, launcher::IconIndex{101}},
            launcher::IconGroup{path, launcher::IconResourceId{1}}}) {
        REQUIRE_THROWS_AS(fixture.cache.load(invalid, 48, repeated), std::system_error);
        REQUIRE_FALSE(repeated);
    }
}

TEST_CASE("Shell32 icon groups load through resolved paths by index and ID", "[icons][icon-group][native]")
{
    Fixture fixture;
    ComPtr<IWICBitmapSource> indexed;
    ComPtr<IWICBitmapSource> identified;
    std::wstring system_directory(32768, L'\0');
    const UINT length = GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
    REQUIRE(length > 0);
    REQUIRE(length < system_directory.size());
    system_directory.resize(length);
    const fs::path module = fs::path(system_directory) / L"SHELL32.dll";
    REQUIRE_NOTHROW(fixture.cache.load(launcher::IconGroup{module, launcher::IconIndex{0}}, 96, indexed));
    REQUIRE_NOTHROW(fixture.cache.load(launcher::IconGroup{module, launcher::IconResourceId{3}}, 96, identified));
    UINT width = 0;
    UINT height = 0;
    REQUIRE(SUCCEEDED(identified->GetSize(&width, &height)));
    REQUIRE(width == 96);
    REQUIRE(height == 96);
    ComPtr<IWICBitmapSource> stock;
    REQUIRE_NOTHROW(fixture.cache.load(launcher::StockIcon::folder, 96, stock));
}

TEST_CASE("Bitmap enlargement preserves monotone edges without contrast ringing", "[icons][native]")
{
    Fixture fixture;
    const auto path = fixture.root / L"Gray step.bmp";
    std::vector<BYTE> bytes;
    number(bytes, 0x4d42, 2);
    number(bytes, 54 + 48 * 48 * 4, 4);
    number(bytes, 0, 4);
    number(bytes, 54, 4);
    number(bytes, 40, 4);
    number(bytes, 48, 4);
    number(bytes, 48, 4);
    number(bytes, 1, 2);
    number(bytes, 32, 2);
    bytes.resize(54, 0);
    for (unsigned y = 0; y < 48; ++y) {
        for (unsigned x = 0; x < 48; ++x) {
            const BYTE value = x < 24 ? 64 : 192;
            bytes.insert(bytes.end(), {value, value, value, 255});
        }
    }
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(output.good());
    }
    for (const UINT size : {60U, 72U, 96U, 240U}) {
        INFO("enlarging 48 pixels to " << size);
        ComPtr<IWICBitmapSource> bitmap;
        REQUIRE_NOTHROW(fixture.cache.load(launcher::FileIcon{path}, size, bitmap));
        std::vector<BYTE> row(size * 4);
        WICRect rectangle{0, static_cast<INT>(size / 2), static_cast<INT>(size), 1};
        REQUIRE(SUCCEEDED(bitmap->CopyPixels(&rectangle, size * 4, static_cast<UINT>(row.size()), row.data())));
        BYTE previous = 64;
        for (UINT x = 0; x < size; ++x) {
            const BYTE value = row[x * 4];
            REQUIRE(value >= 64);
            REQUIRE(value <= 192);
            REQUIRE(value >= previous);
            REQUIRE(row[x * 4 + 3] == 255);
            previous = value;
        }
    }
}

TEST_CASE("Shortcut icons retain their configured artwork", "[icons][native]")
{
    Fixture fixture;
    const auto icon = fixture.ico();
    const auto executable = fixture.executable();
    const auto shortcut = fixture.root / L"Custom shortcut.lnk";
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> persistent;
    REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.GetAddressOf()))));
    REQUIRE(SUCCEEDED(link->SetPath(executable.c_str())));
    REQUIRE(SUCCEEDED(link->SetIconLocation(icon.c_str(), 0)));
    REQUIRE(SUCCEEDED(link.As(&persistent)));
    REQUIRE(SUCCEEDED(persistent->Save(shortcut.c_str(), TRUE)));
    verify_sizes(fixture, launcher::ShellItemIcon{shortcut});
    REQUIRE(SUCCEEDED(link->SetIconLocation(executable.c_str(), -101)));
    REQUIRE(SUCCEEDED(persistent->Save(shortcut.c_str(), TRUE)));
    fixture.cache.clear();
    verify_sizes(fixture, launcher::ShellItemIcon{shortcut});
}

TEST_CASE("Icon cache reuses sizes and releases entries independently of callers", "[icons][native]")
{
    Fixture fixture;
    const auto path = fixture.ico();
    ComPtr<IWICBitmapSource> first;
    ComPtr<IWICBitmapSource> repeated;
    REQUIRE_NOTHROW(fixture.cache.load(launcher::FileIcon{path}, 96, first));
    REQUIRE_NOTHROW(fixture.cache.load(launcher::FileIcon{path}, 96, repeated));
    REQUIRE(first.Get() == repeated.Get());
    for (UINT size = 1; size <= 130; ++size) {
        ComPtr<IWICBitmapSource> bitmap;
        REQUIRE_NOTHROW(fixture.cache.load(launcher::FileIcon{path}, size, bitmap));
    }
    fixture.cache.clear();
    const auto center = pixel(first.Get(), 48, 48);
    REQUIRE(center == std::array<BYTE, 4>{255, 0, 0, 255});
    REQUIRE_NOTHROW(fixture.cache.load(launcher::FileIcon{path}, 96, repeated));
    REQUIRE(first.Get() != repeated.Get());
    auto before_change = repeated;
    const auto changed = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(changed != INVALID_HANDLE_VALUE);
    FILETIME stamp{};
    const bool read_stamp = GetFileTime(changed, nullptr, nullptr, &stamp) != FALSE;
    if (++stamp.dwLowDateTime == 0) { ++stamp.dwHighDateTime; }
    const bool set_stamp = SetFileTime(changed, nullptr, nullptr, &stamp) != FALSE;
    CloseHandle(changed);
    REQUIRE(read_stamp);
    REQUIRE(set_stamp);
    REQUIRE_NOTHROW(fixture.cache.load(launcher::FileIcon{path}, 96, repeated));
    REQUIRE(before_change.Get() != repeated.Get());
    REQUIRE_THROWS_AS(fixture.cache.load(launcher::FileIcon{(fixture.root / L"missing.ico")}, 96, repeated), launcher::ConfigError);
    REQUIRE(!repeated);
}
