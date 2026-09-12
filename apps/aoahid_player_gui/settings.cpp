// SPDX-License-Identifier: MIT
#include "settings.hpp"

#include "aoahid_player/paths.hpp"
#include "aoahid_player/spec_builder.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

namespace gui {
namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

// Accepts decimal or 0x-prefixed hex; anything else leaves `out` alone.
void read_int(const std::string& text, const int minimum, const int maximum, int& out) {
    if (text.empty())
        return;
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 0);
    if (errno != 0 || end == text.c_str() || *end != '\0')
        return;
    if (value < minimum || value > maximum)
        return;
    out = static_cast<int>(value);
}

void read_bool(const std::string& text, bool& out) {
    if (text == "1" || text == "true" || text == "on")
        out = true;
    else if (text == "0" || text == "false" || text == "off")
        out = false;
}

void read_axes(const std::string& text, std::vector<aoahid_axis_role>& out) {
    std::vector<aoahid_axis_role> axes;
    std::stringstream stream(text);
    std::string piece;
    while (std::getline(stream, piece, ',')) {
        const aoap::spec_detail::AxisIdentity* identity =
            aoap::spec_detail::find_axis(trim(piece));
        if (identity == nullptr)
            return;
        if (std::find(axes.begin(), axes.end(), identity->role) != axes.end())
            return;
        axes.push_back(identity->role);
    }
    const bool has_x = std::find(axes.begin(), axes.end(), AOAHID_AXIS_X) != axes.end();
    const bool has_y = std::find(axes.begin(), axes.end(), AOAHID_AXIS_Y) != axes.end();
    if (has_x && has_y)
        out = std::move(axes);
}

std::string axes_text(const std::vector<aoahid_axis_role>& axes) {
    std::string text;
    for (const aoahid_axis_role role : axes) {
        const aoap::spec_detail::AxisIdentity* identity = aoap::spec_detail::find_axis(role);
        if (identity == nullptr)
            continue;
        if (!text.empty())
            text += ',';
        text += identity->name;
    }
    return text;
}

} // namespace

std::filesystem::path settings_path() {
    return aoap::utf8_path(aoap::executable_directory()) / "aoahid_player_gui.ini";
}

Settings load_settings(const std::filesystem::path& path) {
    Settings settings;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return settings;

    std::string line;
    while (std::getline(file, line)) {
        const std::string_view view = trim(line);
        if (view.empty() || view.front() == '#')
            continue;
        const size_t equals = view.find('=');
        if (equals == std::string_view::npos)
            continue;
        const std::string key(trim(view.substr(0, equals)));
        const std::string value(trim(view.substr(equals + 1)));

        if (key == "touch")
            read_bool(value, settings.use_touch);
        else if (key == "touch.width")
            read_int(value, 1, 65536, settings.touch_width);
        else if (key == "touch.height")
            read_int(value, 1, 65536, settings.touch_height);
        else if (key == "touch.contacts")
            read_int(value, 1, 16, settings.touch_contacts);
        else if (key == "mouse")
            read_bool(value, settings.use_mouse);
        else if (key == "mouse.buttons")
            read_int(value, 1, 16, settings.mouse_buttons);
        else if (key == "keyboard")
            read_bool(value, settings.use_key);
        else if (key == "keyboard.usage_min")
            read_int(value, 0x04, 0xDF, settings.key_min);
        else if (key == "keyboard.usage_max")
            read_int(value, 0x04, 0xDF, settings.key_max);
        else if (key == "gamepad")
            read_bool(value, settings.use_gamepad);
        else if (key == "gamepad.buttons")
            read_int(value, 1, 32, settings.pad_buttons);
        else if (key == "gamepad.axis_bits")
            read_int(value, 2, 32, settings.pad_bits);
        else if (key == "gamepad.dpad") {
            if (value == "none")
                settings.pad_dpad = 0;
            else if (value == "hat")
                settings.pad_dpad = 1;
            else if (value == "buttons")
                settings.pad_dpad = 2;
        }
        else if (key == "gamepad.axes")
            read_axes(value, settings.pad_axes);
        else if (key == "pen")
            read_bool(value, settings.use_pen);
        else if (key == "pen.mode") {
            if (value == "direct")
                settings.pen_mode = 0;
            else if (value == "indirect")
                settings.pen_mode = 1;
        }
        else if (key == "live.release_key")
            read_int(value, 0, 348, settings.live_release_key);
    }
    if (settings.key_max < settings.key_min)
        settings.key_max = settings.key_min;
    return settings;
}

bool save_settings(const std::filesystem::path& path, const Settings& settings,
                   std::string& error) {
    std::ostringstream out;
    char hex[8];
    out << "# AOA HID Player settings, saved automatically. Delete this file to reset.\n";
    out << "touch = " << (settings.use_touch ? 1 : 0) << '\n';
    out << "touch.width = " << settings.touch_width << '\n';
    out << "touch.height = " << settings.touch_height << '\n';
    out << "touch.contacts = " << settings.touch_contacts << '\n';
    out << "mouse = " << (settings.use_mouse ? 1 : 0) << '\n';
    out << "mouse.buttons = " << settings.mouse_buttons << '\n';
    out << "keyboard = " << (settings.use_key ? 1 : 0) << '\n';
    std::snprintf(hex, sizeof hex, "0x%02X", static_cast<unsigned>(settings.key_min));
    out << "keyboard.usage_min = " << hex << '\n';
    std::snprintf(hex, sizeof hex, "0x%02X", static_cast<unsigned>(settings.key_max));
    out << "keyboard.usage_max = " << hex << '\n';
    out << "gamepad = " << (settings.use_gamepad ? 1 : 0) << '\n';
    out << "gamepad.buttons = " << settings.pad_buttons << '\n';
    out << "gamepad.axis_bits = " << settings.pad_bits << '\n';
    out << "gamepad.dpad = "
        << (settings.pad_dpad == 0 ? "none" : settings.pad_dpad == 2 ? "buttons" : "hat") << '\n';
    out << "gamepad.axes = " << axes_text(settings.pad_axes) << '\n';
    out << "pen = " << (settings.use_pen ? 1 : 0) << '\n';
    out << "pen.mode = " << (settings.pen_mode == 1 ? "indirect" : "direct") << '\n';
    out << "live.release_key = " << settings.live_release_key << '\n';

    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "cannot write " + aoap::path_utf8(temporary);
            return false;
        }
        const std::string text = out.str();
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.flush();
        if (!file) {
            error = "cannot write " + aoap::path_utf8(temporary);
            return false;
        }
    }
    std::error_code code;
    std::filesystem::rename(temporary, path, code);
    if (code) {
        std::filesystem::remove(temporary, code);
        error = "cannot replace " + aoap::path_utf8(path);
        return false;
    }
    return true;
}

} // namespace gui
