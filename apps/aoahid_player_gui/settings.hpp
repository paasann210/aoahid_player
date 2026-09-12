// SPDX-License-Identifier: MIT
#pragma once

#include <aoahid.h>

#include <filesystem>
#include <string>
#include <vector>

namespace gui {

// What the GUI remembers between runs: the profile choices and the adb
// option. Defaults are what a first run shows.
struct Settings {
    bool use_touch{true};
    int touch_width{1080};
    int touch_height{2400};
    int touch_contacts{10};
    bool use_mouse{};
    int mouse_buttons{5};
    bool use_key{};
    int key_min{0x04};
    int key_max{0x65};
    bool use_gamepad{};
    int pad_buttons{16};
    int pad_bits{16};
    int pad_dpad{1}; // 0 none, 1 hat, 2 buttons
    std::vector<aoahid_axis_role> pad_axes{AOAHID_AXIS_X, AOAHID_AXIS_Y, AOAHID_AXIS_Z,
                                           AOAHID_AXIS_RZ};
    bool use_pen{};
    int pen_mode{}; // 0 direct screen, 1 indirect tablet
    // The GLFW key that releases the captured pointer in Live control's
    // mouse mode. 0 means "not set", which live control treats as Escape.
    int live_release_key{};

    bool operator==(const Settings&) const = default;
};

// aoahid_player_gui.ini next to the executable, so the folder stays portable.
std::filesystem::path settings_path();

// A missing file gives the defaults. A value that cannot be read or is out
// of range keeps its default, so a hand-edited file never breaks startup.
Settings load_settings(const std::filesystem::path& path);

// Written to a temporary file and renamed over the old one, so a crash never
// leaves a half-written file.
bool save_settings(const std::filesystem::path& path, const Settings& settings,
                   std::string& error);

} // namespace gui
