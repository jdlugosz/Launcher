#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/filesystem_error_notice.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using launcher::PerSearchNoticeState;
using launcher::TargetScanDiagnostic;
using launcher::TargetScanOperation;
using launcher::TargetScanStatus;
using launcher::TargetVersionScanResult;

TargetScanDiagnostic native_error(std::wstring path, DWORD code = ERROR_NOT_READY,
        TargetScanOperation operation = TargetScanOperation::enumerate) {
    return {operation, std::move(path), {static_cast<int>(code), std::system_category()}};
}

TargetVersionScanResult failure(std::vector<TargetScanDiagnostic> diagnostics, TargetScanStatus status = TargetScanStatus::failed) {
    TargetVersionScanResult result;
    result.status = status;
    result.diagnostics = std::move(diagnostics);
    return result;
}

} // namespace

TEST_CASE("Filesystem notices suppress unchanged sets regardless of ordering and duplicates", "[filesystem-error-notice]") {
    PerSearchNoticeState state;
    auto result = failure({
        native_error(L"E:\\Versions\\*", ERROR_NOT_READY),
        native_error(L"E:\\Versions\\One\\app.exe", ERROR_READ_FAULT, TargetScanOperation::metadata)});
    REQUIRE(state.observe(result));
    state.mark_shown();
    CHECK_FALSE(state.observe(result));
    std::reverse(result.diagnostics.begin(), result.diagnostics.end());
    result.diagnostics.push_back(result.diagnostics.front());
    result.diagnostics.front().operation = TargetScanOperation::probe;
    CHECK_FALSE(state.observe(result));
    result.status = TargetScanStatus::incomplete;
    CHECK_FALSE(state.observe(result));
}

TEST_CASE("Filesystem notices distinguish changed full paths domains codes and sets", "[filesystem-error-notice]") {
    PerSearchNoticeState state;
    auto result = failure({native_error(L"E:\\资料\\app.exe", ERROR_ACCESS_DENIED)});
    REQUIRE(state.observe(result));
    state.mark_shown();
    result.diagnostics.front().path = L"E:\\资料\\older\\app.exe";
    REQUIRE(state.observe(result));
    state.mark_shown();
    result.diagnostics.front().error = {ERROR_SHARING_VIOLATION, std::system_category()};
    REQUIRE(state.observe(result));
    state.mark_shown();
    result.diagnostics.front().error = {ERROR_SHARING_VIOLATION, std::generic_category()};
    REQUIRE(state.observe(result));
    state.mark_shown();
    result.diagnostics.push_back(native_error(L"\\\\server\\share\\*", ERROR_BAD_NETPATH));
    REQUIRE(state.observe(result));
    state.mark_shown();
    result.diagnostics.erase(result.diagnostics.begin());
    REQUIRE(state.observe(result));
    state.mark_shown();
    CHECK_FALSE(state.observe(result));
}

TEST_CASE("Filesystem notices commit suppression only after successful delivery", "[filesystem-error-notice]") {
    PerSearchNoticeState state;
    const auto first = failure({native_error(L"E:\\Versions\\*")});
    const auto second = failure({native_error(L"F:\\Versions\\*")});
    REQUIRE(state.observe(first));
    CHECK(state.observe(first)); // Native delivery failed, so retry remains due.
    state.mark_shown();
    CHECK_FALSE(state.observe(first));
    REQUIRE(state.observe(second));
    CHECK_FALSE(state.observe(first)); // A new observation invalidates staging.
    state.mark_shown();                // An accidental empty commit does nothing.
    CHECK_FALSE(state.observe(first));
    REQUIRE(state.observe(second));
    state.mark_shown();
    CHECK_FALSE(state.observe(second));
    CHECK(state.observe(first));
}

TEST_CASE("Filesystem notices clear only after complete successful recovery", "[filesystem-error-notice]") {
    PerSearchNoticeState state;
    const auto fault = failure({native_error(L"E:\\Versions\\*")});
    REQUIRE(state.observe(fault));
    state.mark_shown();
    auto internal = failure({
        native_error(L"E:\\Versions\\*", ERROR_INVALID_NAME, TargetScanOperation::normalize),
        native_error(L"E:\\Versions\\*", ERROR_NOT_ENOUGH_MEMORY, TargetScanOperation::budget)});
    internal.omitted_diagnostics = 12;
    internal.exhausted_limit = launcher::TargetScanLimit::entries;
    CHECK_FALSE(state.observe(internal));
    CHECK_FALSE(state.observe(fault));
    CHECK_FALSE(state.observe(failure({}, TargetScanStatus::incomplete)));
    CHECK_FALSE(state.observe(fault));
    CHECK_FALSE(state.observe(TargetVersionScanResult{}));
    REQUIRE(state.observe(fault));
    state.mark_shown();
    CHECK_FALSE(state.observe(fault));
}

TEST_CASE("Filesystem notices ignore internal limit custom and successful diagnostic codes", "[filesystem-error-notice]") {
    class InternalCategory final : public std::error_category {
    public:
        const char* name() const noexcept override { return "launcher.internal"; }
        std::string message(int) const override { return "internal test"; }
    } internal_category;
    auto result = failure({
        native_error(L"E:\\normalize", ERROR_ACCESS_DENIED, TargetScanOperation::normalize),
        native_error(L"E:\\budget", ERROR_READ_FAULT, TargetScanOperation::budget),
        {TargetScanOperation::metadata, L"E:\\custom", {ERROR_READ_FAULT, internal_category}},
        {TargetScanOperation::metadata, L"E:\\limit", {ERROR_READ_FAULT, std::system_category()}, launcher::TargetScanLimit::resource_bytes},
        native_error(L"E:\\success", ERROR_SUCCESS)});
    PerSearchNoticeState state;
    CHECK_FALSE(state.observe(result));
    state.mark_shown();
    result.diagnostics.push_back({TargetScanOperation::metadata, L"E:\\generic", std::make_error_code(std::errc::permission_denied)});
    REQUIRE(state.observe(result));
    state.mark_shown();
    CHECK_FALSE(state.observe(result));
}

TEST_CASE("Filesystem notice state belongs to each configured search", "[filesystem-error-notice]") {
    PerSearchNoticeState first_search;
    PerSearchNoticeState second_search;
    const auto fault = failure({native_error(L"E:\\Versions\\*")});
    REQUIRE(first_search.observe(fault));
    first_search.mark_shown();
    CHECK_FALSE(first_search.observe(fault));
    CHECK(second_search.observe(fault));
    CHECK_FALSE(second_search.observe(TargetVersionScanResult{}));
    CHECK_FALSE(first_search.observe(fault));
    CHECK(second_search.observe(fault));
}

TEST_CASE("Filesystem notice memory stays within native diagnostic bounds", "[filesystem-error-notice]") {
    PerSearchNoticeState state;
    auto result = failure({});
    for (int index = 0; index < 64; ++index) {
        result.diagnostics.push_back(native_error(L"E:\\" + std::to_wstring(index) + L"\\app.exe"));
    }
    REQUIRE(state.observe(result));
    state.mark_shown();
    std::reverse(result.diagnostics.begin(), result.diagnostics.end());
    CHECK_FALSE(state.observe(result));
    result.diagnostics.push_back(result.diagnostics.front());
    CHECK_FALSE(state.observe(result)); // Duplicates need no extra retained slot.
    result.diagnostics.push_back(native_error(L"E:\\65th\\app.exe"));
    CHECK_FALSE(state.observe(result)); // An unexpected oversized set is not adopted.
    state.mark_shown();
    result.diagnostics.pop_back();
    CHECK_FALSE(state.observe(result));
    auto full_path = failure({native_error(L"E:\\" + std::wstring(32764, L'资'))});
    REQUIRE(state.observe(full_path));
    state.mark_shown();
    full_path.diagnostics.front().path += L"x";
    CHECK_FALSE(state.observe(full_path));
    state.mark_shown();
    full_path.diagnostics.front().path = L"E:\\" + std::wstring(32764, L'资');
    CHECK_FALSE(state.observe(full_path));
}
