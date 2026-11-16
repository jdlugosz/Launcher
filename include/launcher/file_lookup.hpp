#pragma once

#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace launcher {

enum class FileFallback { none, embedded_resource, exact_file };
enum class InstallationScope { unspecified, portable, current_user, all_users };

struct FileText {
    std::string text;
    std::string source; // Full filename, or full executable filename plus resource name.
    std::filesystem::path file; // Empty for an embedded resource.
};

// One startup-owned search policy for configuration, palettes and explicit icons.
// Only a successfully compared duplicate is removed; the last occurrence wins.
class FileLookup final {
public:
    FileLookup(std::filesystem::path executable, std::filesystem::path user_directory,
        std::filesystem::path configuration_directory = {}, bool explicitly_named = false);

    const std::filesystem::path& executable() const { return executable_; }
    const std::filesystem::path& user_directory() const { return user_directory_; }
    const std::vector<std::filesystem::path>& directories() const { return directories_; }
    InstallationScope installation_scope() const { return installation_scope_; }
    bool portable() const { return installation_scope_ == InstallationScope::portable; }
    bool explicitly_named() const { return explicitly_named_; }

    // Absolute names bypass the search. Relative subdirectories are retained.
    // These queries verify read access and reject directories and broken links.
    // Only genuine absence returns nullopt; other failures throw ConfigError.
    std::optional<std::filesystem::path> find(const std::filesystem::path& name, FileFallback fallback = FileFallback::none) const;
    std::filesystem::path require(const std::filesystem::path& name, FileFallback fallback = FileFallback::none) const;
    FileText read_text(const std::filesystem::path& name, FileFallback fallback = FileFallback::none) const;
    std::optional<FileText> read_optional_text(const std::filesystem::path& name, FileFallback fallback = FileFallback::none) const;

    // Exact destinations never borrow another root's file, including when the
    // destination does not exist yet. Native adapters retain their own flags.
    std::filesystem::path exact_path(const std::filesystem::path& name) const;
    std::filesystem::path runtime_path(const std::filesystem::path& configuration) const;
    void write_text(const std::filesystem::path& name, std::string_view text) const;
    std::ofstream open_output(const std::filesystem::path& name, bool append = false) const;
    template<class Opener>
    decltype(auto) open_exact(const std::filesystem::path& name, Opener&& opener) const {
        return std::forward<Opener>(opener)(exact_path(name));
    }

    // Formats an already established missing-file result; performs no I/O.
    std::string missing_message(const std::filesystem::path& name, FileFallback fallback = FileFallback::none) const;

    // Also used by TOML preprocessing after a file has already been selected.
    static std::string read_file(const std::filesystem::path& filename, std::size_t maximum_bytes = std::numeric_limits<std::size_t>::max());

private:
    std::filesystem::path executable_;
    std::filesystem::path user_directory_;
    std::vector<std::filesystem::path> directories_;
    InstallationScope installation_scope_ = InstallationScope::unspecified;
    bool explicitly_named_ = false;
};

} // namespace launcher
