#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/pe_resources.hpp"
#include "launcher/resource_module.hpp"
#include "launcher/file_lookup.hpp"

#include <system_error>

namespace launcher {

void resolve_icon_path(std::filesystem::path& path, const std::filesystem::path& directory)
{
    if (path.is_relative() && !directory.empty()) { path = directory / path; }
    path = path.lexically_normal();
}

std::vector<BYTE> read_pe_resource(const PeResourceIcon& source)
{
    const FileLookup files({}, {});
    const auto path = files.require(source.path, FileFallback::exact_file);
    auto module = files.open_exact(path, [](const auto& filename) { return ResourceModule(filename); });
    const auto bytes = module.FetchBin(source.type, source.name, source.language);
    if (bytes.size() > 32 * 1024 * 1024) { throw std::system_error(ERROR_FILE_TOO_LARGE, std::system_category(), "PE icon resource"); }
    return {bytes.begin(), bytes.end()};
}

} // namespace launcher
