#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/config.hpp"
#include "launcher/icon_images.hpp"
#include "launcher/pe_resources.hpp"
#include "launcher/svg_images.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

// A small integration executable, linked to the production parser and images
// library. It has no Shell fallback: every configured resource must decode.
int wmain(int count, wchar_t** arguments)
{
    if (count != 3) {
        std::cerr << "Usage: LauncherResourceProbe configuration.toml output-directory\n";
        return 2;
    }
    const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(apartment)) { return 1; }
    int result = 0;
    try {
        namespace fs = std::filesystem;
        const fs::path configuration_path = fs::absolute(arguments[1]);
        const fs::path directory = arguments[2];
        fs::create_directories(directory);
        const auto configuration = launcher::load_configuration(configuration_path);
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf())))) {
            throw std::runtime_error("Cannot initialize WIC");
        }
        launcher::IconImages images;
        images.initialize(factory.Get());
        unsigned index = 0;
        for (const auto& tab : configuration.tabs) {
            for (const auto& item : tab.items) {
                auto resource = std::get<launcher::PeResourceIcon>(item.icon_source);
                launcher::resolve_icon_path(resource.path, configuration_path.parent_path());
                const auto bytes = launcher::read_pe_resource(resource);
                std::ofstream output(directory / (std::to_string(index) + ".bin"), std::ios::binary);
                output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!output.good()) { throw std::runtime_error("Cannot save extracted bytes"); }
                Microsoft::WRL::ComPtr<IWICBitmapSource> image;
                images.load(resource, 48, image);
                UINT width = 0;
                UINT height = 0;
                if (FAILED(image->GetSize(&width, &height)) || width != 48 || height != 48) { throw std::runtime_error("Wrong decoded dimensions"); }
                std::vector<BYTE> pixels(width * height * 4);
                if (FAILED(image->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()))) { throw std::runtime_error("Cannot read decoded pixels"); }
                bool visible = false;
                for (std::size_t alpha = 3; alpha < pixels.size(); alpha += 4) { visible |= pixels[alpha] != 0; }
                if (!visible) { throw std::runtime_error("Resource decoded to entirely transparent pixels"); }
                std::cout << index++ << ": " << item.label << ", " << bytes.size() << " bytes, "
                    << (launcher::is_svg_image(bytes) ? "SVG/LunaSVG" : "WIC") << ", 48x48 visible pixels\n";
            }
        }
        if (index == 0) { throw std::runtime_error("No resource examples were exercised"); }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    CoUninitialize();
    return result;
}
