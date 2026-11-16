#pragma once

#include "launcher/windows_pch.hpp"
#include "launcher/icon_source.hpp"

#include <vector>

namespace launcher {

// Configuration paths have already completed substitutions. Resolve only their
// filesystem location, also used for decoding and extraction probes.
void resolve_icon_path(std::filesystem::path& path, const std::filesystem::path& directory = {});

// Reads data without executing the module. The output owns its bytes after the
// module is released; no decoder or cache borrows mapped resource storage.
// Explicit languages must exist, including language-neutral 0.
// The file is selected exactly through FileLookup; missing/unreadable files
// throw ConfigError, and native resource/decoder failures throw system_error.
std::vector<BYTE> read_pe_resource(const PeResourceIcon& source);

} // namespace launcher
