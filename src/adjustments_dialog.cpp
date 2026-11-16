#include "launcher/windows_pch.hpp"

#if LAUNCHER_USE_STD_MODULE && !defined(__INTELLISENSE__)
#include "launcher/std_header_macros.hh"
import std;
#include "launcher/seenthemall.hh"
#endif

#include "launcher/adjustments_dialog.hpp"
#include "adjustments_resource.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string_view>
#include <vector>

namespace launcher {
namespace {

struct Field {
    int edit;
    int spin;
    double minimum;
    double maximum;
    const wchar_t* label;
    bool percentage;
    double step = 1.0;
};

constexpr std::array<Field, 4> fields{{
    {IDC_TEXT_SCALE, IDC_TEXT_SPIN, 25.0, 400.0, L"Text scale", true},
    {IDC_ICON_SCALE, IDC_ICON_SPIN, 25.0, 400.0, L"Icon/panel scale", true},
    {IDC_ICON_SPACING, IDC_SPACING_SPIN, 0.0,
        static_cast<double>(max_icon_spacing_px), L"Icon spacing", false},
    {IDC_THICKNESS, IDC_THICKNESS_SPIN, 0.0, max_thickness_dip, L"Thickness", false, 0.1}
}};

enum class PresetAction { none, store, recall };

struct DialogContext {
    RuntimeAdjustments committed;
    RuntimeAdjustments preview;
    const AdjustmentCallbacks& callbacks;
    HWND owner{};
    bool initializing = true;
    bool restored = false;
    PresetAction preset_action = PresetAction::none;
    std::vector<AdjustmentPreset> presets;
};

void position_dialog(HWND dialog, HWND owner, bool only_if_overlap = false);

std::wstring number_text(double value)
{
    std::wostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(10) << value;
    return text.str();
}

void status(HWND dialog, const std::wstring& message)
{
    SetDlgItemTextW(dialog, IDC_ADJUSTMENT_STATUS, message.c_str());
}

void report_error(HWND dialog, const std::wstring& message)
{
    status(dialog, message);
    if (IsWindow(dialog)) {
        MessageBoxW(dialog, message.c_str(), L"Launcher adjustments", MB_OK | MB_ICONERROR);
    }
}

std::wstring wide_text(std::string_view text)
{
    if (text.empty()) { return {}; }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count == 0) { throw ConfigError("Could not display the preset name"); }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::array<double, 4> displayed_values(const RuntimeAdjustments& values)
{
    return {values.text_scale * 100.0, values.icon_scale * 100.0,
        static_cast<double>(values.icon_spacing_px.value_or(8)), values.thickness_dip.value_or(2)};
}

bool read_number(HWND dialog, const Field& field, double& value)
{
    std::array<wchar_t, 64> text{};
    const int length = GetDlgItemTextW(dialog, field.edit, text.data(),
        static_cast<int>(text.size()));
    if (length <= 0 || length >= static_cast<int>(text.size()) - 1) {
        return false;
    }
    wchar_t* end = nullptr;
    value = std::wcstod(text.data(), &end);
    if (end == text.data()) {
        return false;
    }
    while (*end != L'\0' && std::iswspace(*end)) {
        ++end;
    }
    return *end == L'\0' && std::isfinite(value) &&
        value >= field.minimum && value <= field.maximum &&
        (field.edit != IDC_ICON_SPACING || std::floor(value) == value);
}

bool read_values(HWND dialog, DialogContext& context, RuntimeAdjustments& values)
{
    values = context.preview;
    std::array<double, 4> numbers{};
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (!read_number(dialog, fields[index], numbers[index])) {
            status(dialog, std::wstring(fields[index].label) + L" must be " +
                std::to_wstring(static_cast<int>(fields[index].minimum)) + L"–" +
                std::to_wstring(static_cast<int>(fields[index].maximum)) +
                (fields[index].percentage ? L"%." : fields[index].edit == IDC_THICKNESS
                    ? L" DIPs (decimals allowed)." : L" px (whole pixels)."));
            return false;
        }
    }
    values.text_scale = numbers[0] / 100.0;
    values.icon_scale = numbers[1] / 100.0;
    values.spacing_scale = 1.0;
    values.icon_spacing_px = static_cast<std::uint32_t>(numbers[2]);
    values.thickness_dip = numbers[3];
    return true;
}

void update_description(HWND dialog, DialogContext& context)
{
    if (context.callbacks.describe) {
        const auto description = context.callbacks.describe();
        SetDlgItemTextW(dialog, IDC_EFFECTIVE_SIZES, description.c_str());
    }
}

void show_values(HWND dialog, DialogContext& context, const RuntimeAdjustments& values)
{
    context.initializing = true;
    const auto numbers = displayed_values(values);
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto text = number_text(numbers[index]);
        SetDlgItemTextW(dialog, fields[index].edit, text.c_str());
        SendDlgItemMessageW(dialog, fields[index].spin, UDM_SETPOS32, 0,
            static_cast<LPARAM>(std::lround(numbers[index] / fields[index].step)));
    }
    context.initializing = false;
}

bool preview(HWND dialog, DialogContext& context)
{
    RuntimeAdjustments values;
    if (!read_values(dialog, context, values)) {
        return false;
    }
    std::wstring error;
    if (!context.callbacks.preview(values, error)) {
        status(dialog, error.empty() ? L"These sizes cannot be previewed." : error);
        return false;
    }
    context.preview = values;
    update_description(dialog, context);
    position_dialog(dialog, context.owner, true);
    status(dialog, L"Previewing. Apply or OK saves these adjustments.");
    return true;
}

void dismiss_presets(HWND dialog, DialogContext& context)
{
    const int button = context.preset_action == PresetAction::store ? IDC_STORE_PRESET : IDC_RECALL_PRESET;
    context.preset_action = PresetAction::none;
    ShowWindow(GetDlgItem(dialog, IDC_PRESET_LIST), SW_HIDE);
    ShowWindow(GetDlgItem(dialog, IDC_EFFECTIVE_SIZES), SW_SHOW);
    SetFocus(GetDlgItem(dialog, button));
}

void choose_preset(HWND dialog, DialogContext& context)
{
    const auto index = SendDlgItemMessageW(dialog, IDC_PRESET_LIST, LB_GETCURSEL, 0, 0);
    if (index == LB_ERR || static_cast<std::size_t>(index) >= context.presets.size()) {
        status(dialog, L"Choose a preset name first. Esc returns to the notes.");
        return;
    }
    const auto& preset = context.presets[static_cast<std::size_t>(index)];
    const auto name = wide_text(preset.name);
    std::wstring error;
    if (context.preset_action == PresetAction::store) {
        if (!preview(dialog, context)) { return; }
        if (!context.callbacks.store_preset(preset.name, context.preview, error)) {
            report_error(dialog, error.empty() ? L"The preset has not been stored." : error);
            return;
        }
        dismiss_presets(dialog, context);
        status(dialog, L"Stored '" + name + L"'. Apply or OK saves active adjustments.");
    } else {
        if (!preset.values) {
            status(dialog, L"This slot is empty. Store adjustments in it before recalling.");
            return;
        }
        if (!context.callbacks.preview(*preset.values, error)) {
            status(dialog, error.empty() ? L"This preset cannot be previewed." : error);
            return;
        }
        context.preview = *preset.values;
        show_values(dialog, context, context.preview);
        update_description(dialog, context);
        position_dialog(dialog, context.owner, true);
        dismiss_presets(dialog, context);
        status(dialog, L"Recalled '" + name + L"'. Apply or OK saves these adjustments.");
    }
}

void show_presets(HWND dialog, DialogContext& context, PresetAction action)
{
    auto presets = context.callbacks.load_presets();
    if (presets.empty()) {
        status(dialog, L"Add [[preset]] entries with a name in the runtime file first.");
        return;
    }
    SendDlgItemMessageW(dialog, IDC_PRESET_LIST, LB_RESETCONTENT, 0, 0);
    for (const auto& preset : presets) {
        const auto label = wide_text(preset.name) + (preset.values ? L"" : L" (empty)");
        const auto result = SendDlgItemMessageW(dialog, IDC_PRESET_LIST, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (result == LB_ERR || result == LB_ERRSPACE) { throw ConfigError("Could not display the preset list"); }
    }
    context.presets = std::move(presets);
    context.preset_action = action;
    ShowWindow(GetDlgItem(dialog, IDC_EFFECTIVE_SIZES), SW_HIDE);
    ShowWindow(GetDlgItem(dialog, IDC_PRESET_LIST), SW_SHOW);
    SetFocus(GetDlgItem(dialog, IDC_PRESET_LIST));
    status(dialog, action == PresetAction::store
        ? L"Choose a name, then Store or Enter. Esc returns to the notes."
        : L"Choose a name, then Recall or Enter. Esc returns to the notes.");
}

bool commit(HWND dialog, DialogContext& context)
{
    if (!preview(dialog, context)) {
        return false;
    }
    std::wstring error;
    if (!context.callbacks.commit(context.preview, error)) {
        status(dialog, L"The adjustments have not been saved.");
        if (IsWindow(dialog) && IsWindow(context.owner)) {
            MessageBoxW(dialog, error.empty() ? L"Could not save the adjustments." : error.c_str(),
                L"Launcher adjustments", MB_OK | MB_ICONERROR);
        }
        return false;
    }
    context.committed = context.preview;
    status(dialog, L"Saved.");
    return true;
}

bool restore(HWND dialog, DialogContext& context)
{
    std::wstring error;
    if (!context.callbacks.preview(context.committed, error)) {
        status(dialog, error.empty() ? L"Could not restore the saved sizes. Try Cancel again." : error);
        return false;
    }
    context.preview = context.committed;
    context.restored = true;
    return true;
}

void position_dialog(HWND dialog, HWND owner, bool only_if_overlap)
{
    RECT bounds{};
    RECT parent{};
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetWindowRect(dialog, &bounds) || !GetWindowRect(owner, &parent) ||
        !GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor)) {
        return;
    }
    RECT intersection{};
    if (only_if_overlap && !IntersectRect(&intersection, &bounds, &parent)) {
        return;
    }
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    const std::array<LONG, 4> distances{parent.top - monitor.rcWork.top,
        monitor.rcWork.right - parent.right, monitor.rcWork.bottom - parent.bottom,
        parent.left - monitor.rcWork.left};
    const auto edge = static_cast<std::size_t>(std::min_element(distances.begin(), distances.end()) - distances.begin());
    int preferred_x = static_cast<int>((parent.left + parent.right - width) / 2);
    int preferred_y = static_cast<int>((parent.top + parent.bottom - height) / 2);
    const int gap = MulDiv(10, static_cast<int>(GetDpiForWindow(owner)), 96);
    switch (edge) {
    case 0: preferred_y = static_cast<int>(parent.bottom) + gap; break;
    case 1: preferred_x = static_cast<int>(parent.left) - width - gap; break;
    case 2: preferred_y = static_cast<int>(parent.top) - height - gap; break;
    default: preferred_x = static_cast<int>(parent.right) + gap; break;
    }
    const int x = std::clamp(preferred_x,
        static_cast<int>(monitor.rcWork.left),
        std::max(static_cast<int>(monitor.rcWork.left), static_cast<int>(monitor.rcWork.right) - width));
    const int y = std::clamp(preferred_y,
        static_cast<int>(monitor.rcWork.top),
        std::max(static_cast<int>(monitor.rcWork.top), static_cast<int>(monitor.rcWork.bottom) - height));
    SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

INT_PTR CALLBACK dialog_proc(HWND dialog, UINT message, WPARAM word, LPARAM parameter)
{
    auto* context = reinterpret_cast<DialogContext*>(GetWindowLongPtrW(dialog, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            context = reinterpret_cast<DialogContext*>(parameter);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(context));
            if (context->callbacks.dialog_window) {
                context->callbacks.dialog_window(dialog);
            }
            for (const auto& field : fields) {
                SendDlgItemMessageW(dialog, field.spin, UDM_SETBUDDY,
                    reinterpret_cast<WPARAM>(GetDlgItem(dialog, field.edit)), 0);
                SendDlgItemMessageW(dialog, field.spin, UDM_SETRANGE32,
                    static_cast<WPARAM>(std::lround(field.minimum / field.step)),
                    static_cast<LPARAM>(std::lround(field.maximum / field.step)));
                const std::array<UDACCEL, 2> acceleration{{{0, 1}, {2, 5}}};
                SendDlgItemMessageW(dialog, field.spin, UDM_SETACCEL, acceleration.size(),
                    reinterpret_cast<LPARAM>(acceleration.data()));
            }
            show_values(dialog, *context, context->preview);
            ShowWindow(GetDlgItem(dialog, IDC_PRESET_LIST), SW_HIDE);
            EnableWindow(GetDlgItem(dialog, IDC_STORE_PRESET), context->callbacks.load_presets && context->callbacks.store_preset);
            EnableWindow(GetDlgItem(dialog, IDC_RECALL_PRESET), static_cast<bool>(context->callbacks.load_presets));
            update_description(dialog, *context);
            status(dialog, L"Adjust the numbers to preview changes.");
            position_dialog(dialog, context->owner);
            SendDlgItemMessageW(dialog, IDC_TEXT_SCALE, EM_SETSEL, 0, -1);
            return TRUE;
        }
        if (context == nullptr) {
            return FALSE;
        }
        switch (message) {
        case WM_COMMAND: {
            const int command = LOWORD(word);
            if (command == IDC_STORE_PRESET || command == IDC_RECALL_PRESET) {
                const auto action = command == IDC_STORE_PRESET ? PresetAction::store : PresetAction::recall;
                if (context->preset_action == action) { choose_preset(dialog, *context); }
                else { show_presets(dialog, *context, action); }
                return TRUE;
            }
            if ((command == IDOK || (command == IDC_PRESET_LIST && HIWORD(word) == LBN_DBLCLK)) &&
                context->preset_action != PresetAction::none) {
                choose_preset(dialog, *context);
                return TRUE;
            }
            if (command == IDCANCEL && context->preset_action != PresetAction::none) {
                dismiss_presets(dialog, *context);
                status(dialog, L"Preset selection canceled. Continue adjusting the numbers.");
                return TRUE;
            }
            if (!context->initializing && HIWORD(word) == EN_CHANGE &&
                std::any_of(fields.begin(), fields.end(), [command](const Field& field) {
                    return field.edit == command;
                })) {
                preview(dialog, *context);
                return TRUE;
            }
            if (command == IDC_RESET_SIZES) {
                if (context->preset_action != PresetAction::none) { dismiss_presets(dialog, *context); }
                auto reset = context->preview;
                reset.text_scale = 1.0;
                reset.icon_scale = 1.0;
                reset.spacing_scale = 1.0;
                reset.icon_spacing_px = context->callbacks.default_icon_spacing_px
                    ? context->callbacks.default_icon_spacing_px() : 8;
                reset.thickness_dip = context->callbacks.default_thickness_dip
                    ? context->callbacks.default_thickness_dip() : 2;
                show_values(dialog, *context, reset);
                preview(dialog, *context);
                return TRUE;
            }
            if (command == IDOK || command == IDC_APPLY_ADJUSTMENTS) {
                if (context->preset_action != PresetAction::none) { dismiss_presets(dialog, *context); }
                if (commit(dialog, *context) && command == IDOK) {
                    EndDialog(dialog, IDOK);
                }
                return TRUE;
            }
            if (command == IDCANCEL) {
                if (restore(dialog, *context)) {
                    EndDialog(dialog, IDCANCEL);
                }
                return TRUE;
            }
            break;
        }
        case WM_NOTIFY: {
            const auto* notification = reinterpret_cast<NMUPDOWN*>(parameter);
            if (notification != nullptr && notification->hdr.code == UDN_DELTAPOS) {
                const auto field = std::find_if(fields.begin(), fields.end(), [notification](const Field& item) {
                    return static_cast<UINT_PTR>(item.spin) == notification->hdr.idFrom;
                });
                if (field != fields.end()) {
                    double current = 100.0;
                    if (!read_number(dialog, *field, current)) {
                        const auto index = static_cast<std::size_t>(field - fields.begin());
                        current = displayed_values(context->preview)[index];
                    }
                    const double next = std::clamp(current + notification->iDelta * field->step, field->minimum, field->maximum);
                    const auto text = number_text(next);
                    SetDlgItemTextW(dialog, field->edit, text.c_str());
                    SendDlgItemMessageW(dialog, field->spin, UDM_SETPOS32, 0,
                        static_cast<LPARAM>(std::lround(next / field->step)));
                    SetWindowLongPtrW(dialog, DWLP_MSGRESULT, TRUE);
                    return TRUE;
                }
            }
            break;
        }
        case WM_CLOSE:
            if (restore(dialog, *context)) {
                EndDialog(dialog, IDCANCEL);
            }
            return TRUE;
        case WM_NCDESTROY:
            if (context->callbacks.dialog_window) {
                context->callbacks.dialog_window(nullptr);
            }
            return FALSE;
        default:
            break;
        }
    } catch (const std::exception& failure) {
        report_error(dialog, wide_text(failure.what()));
        return TRUE;
    } catch (...) {
        report_error(dialog, L"Could not apply this adjustment.");
        return TRUE;
    }
    return FALSE;
}

} // namespace

INT_PTR show_adjustments_dialog(HINSTANCE instance, HWND owner,
    const RuntimeAdjustments& initial, const AdjustmentCallbacks& callbacks)
{
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_UPDOWN_CLASS};
    if (!InitCommonControlsEx(&controls)) {
        return -1;
    }
    DialogContext context{initial, initial, callbacks, owner};
    const INT_PTR result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_ADJUSTMENTS),
        owner, dialog_proc, reinterpret_cast<LPARAM>(&context));
    if (callbacks.dialog_window) {
        callbacks.dialog_window(nullptr);
    }
    if (result != IDOK && !context.restored && IsWindow(owner)) {
        std::wstring error;
        if (!callbacks.preview(context.committed, error)) {
            MessageBoxW(owner, error.empty() ? L"Could not restore the saved sizes." : error.c_str(),
                L"Launcher adjustments", MB_OK | MB_ICONERROR);
            return -1;
        }
    }
    return result;
}

} // namespace launcher
