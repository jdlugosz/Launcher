#include <launcher/windows_pch.hpp>
#include <launcher/std_header_macros.hh>
import std;
#include <launcher/seenthemall.hh>
#include <launcher/dock.hpp>
#include <toml++/toml.hpp>

#ifndef CHAR_BIT
#error The real macro prelude must retain CHAR_BIT.
#endif
#ifndef assert
#error The real macro prelude must retain assert.
#endif
static_assert(CHAR_BIT == 8);
static_assert(__cpp_lib_char8_t >= 201907L);
static_assert(__cpp_lib_expected >= 202202L);
static_assert(std::ranges::random_access_range<std::vector<int>>);

int main()
{
    launcher::Configuration configuration;
    configuration.tabs.push_back(launcher::Tab{"General", "Gen", 0xf2a5a5, {}});
    const auto parsed = toml::parse("values = [11, 13, 17]\nfinite = 1.5\n");
    std::vector<int> values;
    for (const auto& value : *parsed["values"].as_array()) {
        values.push_back(static_cast<int>(value.value<std::int64_t>().value()));
    }
    std::expected<int, std::string> total = std::accumulate(values.begin(), values.end(), 0);
    if (!total || *total != 41 || !std::isfinite(parsed["finite"].value<double>().value())) { return 1; }
    const auto utf8 = toml::parse(std::u8string_view{u8"title = '你好'\n"});
    if (utf8["title"].value<std::string>().value() != "\xe4\xbd\xa0\xe5\xa5\xbd") { return 2; }
    std::ostringstream serialized;
    serialized << parsed;
    if (toml::parse(serialized.str())["values"][2].value<int>() != 17) { return 3; }
    bool rejected = false;
    try { [[maybe_unused]] const auto invalid = toml::parse("x = [\n"); }
    catch (const toml::parse_error&) { rejected = true; }
    if (!rejected) { return 4; }
    Microsoft::WRL::ComPtr<IUnknown> pointer;
    if (pointer || configuration.tabs.size() != 1) { return 5; }
    const auto path = std::filesystem::path("module-probe") / "configuration.toml";
    std::cout << std::format("std/guards/SDK/toml++ OK: sum={}, tab={}, path={}\n",
        *total, configuration.tabs.front().name, path.string());
    return 0;
}
