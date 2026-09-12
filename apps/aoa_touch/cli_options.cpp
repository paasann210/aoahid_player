// SPDX-License-Identifier: MIT
#include "cli_options.hpp"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace aoap::cli {
namespace {

bool parse_size_t(const std::string_view text, size_t& out) noexcept {
    if (text.empty())
        return false;
    const char* last = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(text.data(), last, out);
    return result.ec == std::errc{} && result.ptr == last;
}

bool parse_uint32(const std::string_view text, uint32_t& out) noexcept {
    size_t value = 0;
    if (!parse_size_t(text, value) || value > UINT32_MAX)
        return false;
    out = static_cast<uint32_t>(value);
    return true;
}

bool parse_resolution(const std::string_view text, int32_t& width, int32_t& height) noexcept {
    const size_t separator = text.find_first_of("xX");
    if (separator == std::string_view::npos)
        return false;
    size_t parsed_width = 0;
    size_t parsed_height = 0;
    if (!parse_size_t(text.substr(0, separator), parsed_width) ||
        !parse_size_t(text.substr(separator + 1), parsed_height))
        return false;
    if (parsed_width == 0 || parsed_height == 0 || parsed_width > 65536 || parsed_height > 65536)
        return false;
    width = static_cast<int32_t>(parsed_width);
    height = static_cast<int32_t>(parsed_height);
    return true;
}

void split(const std::string_view text, const char separator, std::vector<std::string>& out) {
    out.clear();
    size_t start = 0;
    while (start <= text.size()) {
        const size_t position = text.find(separator, start);
        const size_t end = position == std::string_view::npos ? text.size() : position;
        std::string_view piece = text.substr(start, end - start);
        while (!piece.empty() && (piece.front() == ' ' || piece.front() == '\t'))
            piece.remove_prefix(1);
        while (!piece.empty() && (piece.back() == ' ' || piece.back() == '\t'))
            piece.remove_suffix(1);
        out.emplace_back(piece);
        if (position == std::string_view::npos)
            break;
        start = position + 1;
    }
}

bool needs_value(const int index, const int argc, const char* flag) {
    if (index + 1 < argc)
        return true;
    std::fprintf(stderr, "[ERROR] %s needs a value\n", flag);
    return false;
}

// A profile is requested when any of its flags appear; every flag that
// profile needs is then mandatory, because README.md states there are no
// presets and libaoahid supplies no default for a descriptor field.
bool require(const bool present, const char* flag, const char* profile) {
    if (present)
        return true;
    std::fprintf(stderr, "[ERROR] the %s profile also needs %s\n", profile, flag);
    return false;
}

} // namespace

void print_usage(const char* program) {
    std::printf(
        "usage: %s [options] [script.csv]\n"
        "\n"
        "Device selection:\n"
        "  --devices <list>        Comma-separated device numbers from the discovery\n"
        "                           list (e.g. \"1,3\"), or \"all\". If omitted, an\n"
        "                           interactive numbered list is shown.\n"
        "\n"
        "Profile setup (all explicit; there are no presets):\n"
        "  --touch-res WxH          Touch surface resolution, e.g. 1080x1920\n"
        "  --touch-max-contacts N   Max simultaneous touch contacts (1-16)\n"
        "  --gamepad-buttons N      Number of gamepad buttons\n"
        "  --gamepad-axes LIST      Comma-separated axis roles, e.g. x,y,rx,ry\n"
        "                           (x y z rx ry rz slider dial wheel rudder\n"
        "                            throttle accelerator brake steering;\n"
        "                            x and y are required)\n"
        "  --gamepad-axis-bits N    Bit width per axis, 2-32\n"
        "  --gamepad-dpad MODE      none|hat|buttons\n"
        "  --key-usage-range LO,HI  Keyboard HID usage range (default 0x04,0x65)\n"
        "  --mouse-buttons N        Number of mouse buttons\n"
        "  --pen-mode MODE          direct|indirect\n"
        "\n"
        "Playback:\n"
        "  -A                       Auto-detect touch resolution via `adb shell wm size`\n"
        "  --speed FACTOR           Playback speed multiplier (default 1.0)\n"
        "  --loop N                 Stop after N loop iterations (default: infinite)\n"
        "  --no-prompt              Do not start the live \"-> \" offset prompt\n"
        "  -h, --help               Show this text\n"
        "\n"
        "With no script path, the csv/ folder next to the executable is listed\n"
        "for interactive selection.\n"
        "\n"
        "While playing, type a number of milliseconds at the \"-> \" prompt to\n"
        "shift every later event (negative is earlier); Ctrl+C stops.\n",
        program);
}

std::optional<Options> parse(const int argc, char** argv) {
    Options options;
    std::vector<std::string> pieces;

    bool have_touch_res = false;
    bool have_touch_contacts = false;
    bool have_gamepad_buttons = false;
    bool have_gamepad_axes = false;
    bool have_gamepad_bits = false;
    bool have_gamepad_dpad = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);

        if (argument == "-h" || argument == "--help") {
            print_usage(argv[0]);
            options.help = true;
            return options;
        }
        if (argument == "--devices") {
            if (!needs_value(index, argc, "--devices"))
                return std::nullopt;
            const std::string_view value(argv[++index]);
            options.have_devices = true;
            if (value == "all") {
                options.all_devices = true;
                continue;
            }
            split(value, ',', pieces);
            for (const std::string& piece : pieces) {
                size_t number = 0;
                if (!parse_size_t(piece, number) || number == 0) {
                    std::fprintf(stderr, "[ERROR] --devices takes 1-based numbers or \"all\"\n");
                    return std::nullopt;
                }
                options.devices.push_back(number);
            }
            if (options.devices.empty()) {
                std::fprintf(stderr, "[ERROR] --devices is empty\n");
                return std::nullopt;
            }
            continue;
        }
        if (argument == "--touch-res") {
            if (!needs_value(index, argc, "--touch-res"))
                return std::nullopt;
            if (!parse_resolution(argv[++index], options.profiles.touch.width,
                                  options.profiles.touch.height)) {
                std::fprintf(stderr, "[ERROR] --touch-res format is WxH, e.g. 1080x1920\n");
                return std::nullopt;
            }
            have_touch_res = true;
            continue;
        }
        if (argument == "--touch-max-contacts") {
            if (!needs_value(index, argc, "--touch-max-contacts"))
                return std::nullopt;
            uint32_t value = 0;
            if (!parse_uint32(argv[++index], value) || value == 0 || value > 16) {
                std::fprintf(stderr, "[ERROR] --touch-max-contacts must be 1..16\n");
                return std::nullopt;
            }
            options.profiles.touch.max_contacts = value;
            have_touch_contacts = true;
            continue;
        }
        if (argument == "--mouse-buttons") {
            if (!needs_value(index, argc, "--mouse-buttons"))
                return std::nullopt;
            uint32_t value = 0;
            if (!parse_uint32(argv[++index], value) || value == 0 || value > 65535) {
                std::fprintf(stderr, "[ERROR] --mouse-buttons must be 1..65535\n");
                return std::nullopt;
            }
            options.profiles.mouse.buttons = value;
            options.profiles.mouse.enabled = true;
            continue;
        }
        if (argument == "--key-usage-range") {
            if (!needs_value(index, argc, "--key-usage-range"))
                return std::nullopt;
            split(argv[++index], ',', pieces);
            if (pieces.size() != 2) {
                std::fprintf(stderr, "[ERROR] --key-usage-range format is LO,HI\n");
                return std::nullopt;
            }
            unsigned long low = std::strtoul(pieces[0].c_str(), nullptr, 0);
            unsigned long high = std::strtoul(pieces[1].c_str(), nullptr, 0);
            // The Array rollover this profile uses reserves 0xE0..0xE7 for
            // modifiers, which aoahid_kbd routes separately.
            if (low < 0x04 || high < low || high >= 0xE0) {
                std::fprintf(stderr, "[ERROR] --key-usage-range must be ordered, start at 0x04 "
                                     "or above, and end below 0xe0\n");
                return std::nullopt;
            }
            options.profiles.key.usage_minimum = static_cast<uint16_t>(low);
            options.profiles.key.usage_maximum = static_cast<uint16_t>(high);
            options.profiles.key.enabled = true;
            continue;
        }
        if (argument == "--gamepad-buttons") {
            if (!needs_value(index, argc, "--gamepad-buttons"))
                return std::nullopt;
            uint32_t value = 0;
            if (!parse_uint32(argv[++index], value) || value == 0 || value > 65535) {
                std::fprintf(stderr, "[ERROR] --gamepad-buttons must be 1..65535\n");
                return std::nullopt;
            }
            options.profiles.gamepad.buttons = value;
            have_gamepad_buttons = true;
            continue;
        }
        if (argument == "--gamepad-axes") {
            if (!needs_value(index, argc, "--gamepad-axes"))
                return std::nullopt;
            split(argv[++index], ',', pieces);
            options.profiles.gamepad.axes.clear();
            for (const std::string& piece : pieces) {
                const spec_detail::AxisIdentity* identity = spec_detail::find_axis(piece);
                if (identity == nullptr) {
                    std::fprintf(stderr, "[ERROR] unknown gamepad axis role \"%s\"\n",
                                 piece.c_str());
                    return std::nullopt;
                }
                options.profiles.gamepad.axes.push_back(identity->role);
            }
            have_gamepad_axes = true;
            continue;
        }
        if (argument == "--gamepad-axis-bits") {
            if (!needs_value(index, argc, "--gamepad-axis-bits"))
                return std::nullopt;
            uint32_t value = 0;
            if (!parse_uint32(argv[++index], value) || value < 2 || value > 32) {
                std::fprintf(stderr, "[ERROR] --gamepad-axis-bits must be 2..32\n");
                return std::nullopt;
            }
            options.profiles.gamepad.axis_bits = value;
            have_gamepad_bits = true;
            continue;
        }
        if (argument == "--gamepad-dpad") {
            if (!needs_value(index, argc, "--gamepad-dpad"))
                return std::nullopt;
            const std::string_view mode(argv[++index]);
            if (mode == "none")
                options.profiles.gamepad.dpad = AOAHID_DPAD_NONE;
            else if (mode == "hat")
                options.profiles.gamepad.dpad = AOAHID_DPAD_HAT;
            else if (mode == "buttons")
                options.profiles.gamepad.dpad = AOAHID_DPAD_BUTTONS;
            else {
                std::fprintf(stderr, "[ERROR] --gamepad-dpad must be none, hat, or buttons\n");
                return std::nullopt;
            }
            have_gamepad_dpad = true;
            continue;
        }
        if (argument == "--pen-mode") {
            if (!needs_value(index, argc, "--pen-mode"))
                return std::nullopt;
            const std::string_view mode(argv[++index]);
            if (mode == "direct")
                options.profiles.pen.mode = AOAHID_PEN_DIRECT_SCREEN;
            else if (mode == "indirect")
                options.profiles.pen.mode = AOAHID_PEN_INDIRECT_TABLET;
            else {
                std::fprintf(stderr, "[ERROR] --pen-mode must be direct or indirect\n");
                return std::nullopt;
            }
            options.profiles.pen.enabled = true;
            continue;
        }
        if (argument == "-A") {
            options.auto_resolution = true;
            continue;
        }
        if (argument == "--speed") {
            if (!needs_value(index, argc, "--speed"))
                return std::nullopt;
            char* end = nullptr;
            const double value = std::strtod(argv[++index], &end);
            if (end == argv[index] || *end != '\0' || !(value > 0.0) || value > 1000.0) {
                std::fprintf(stderr, "[ERROR] --speed must be a positive multiplier\n");
                return std::nullopt;
            }
            options.speed = value;
            continue;
        }
        if (argument == "--loop") {
            if (!needs_value(index, argc, "--loop"))
                return std::nullopt;
            size_t value = 0;
            if (!parse_size_t(argv[++index], value)) {
                std::fprintf(stderr, "[ERROR] --loop must be a non-negative number\n");
                return std::nullopt;
            }
            options.loop_count = static_cast<int64_t>(value);
            continue;
        }
        if (argument == "--no-prompt") {
            options.no_prompt = true;
            continue;
        }
        if (!argument.empty() && argument.front() == '-') {
            std::fprintf(stderr, "[ERROR] unknown option \"%.*s\"\n",
                         static_cast<int>(argument.size()), argument.data());
            return std::nullopt;
        }
        if (!options.script_path.empty()) {
            std::fprintf(stderr, "[ERROR] more than one script path was given\n");
            return std::nullopt;
        }
        options.script_path.assign(argument);
    }

    // -A supplies the resolution, so it also requests the touch profile.
    if (have_touch_res || have_touch_contacts || options.auto_resolution) {
        options.profiles.touch.enabled = true;
        if (!require(have_touch_res || options.auto_resolution, "--touch-res or -A", "touch") ||
            !require(have_touch_contacts, "--touch-max-contacts", "touch"))
            return std::nullopt;
    }
    if (have_gamepad_buttons || have_gamepad_axes || have_gamepad_bits || have_gamepad_dpad) {
        options.profiles.gamepad.enabled = true;
        if (!require(have_gamepad_buttons, "--gamepad-buttons", "gamepad") ||
            !require(have_gamepad_axes, "--gamepad-axes", "gamepad") ||
            !require(have_gamepad_bits, "--gamepad-axis-bits", "gamepad") ||
            !require(have_gamepad_dpad, "--gamepad-dpad", "gamepad"))
            return std::nullopt;
        if (options.profiles.gamepad.axes.size() < 2) {
            std::fprintf(stderr, "[ERROR] --gamepad-axes needs at least x and y\n");
            return std::nullopt;
        }
    }
    if (options.profiles.pen.enabled)
        resolve_pen_surface(options.profiles);

    if (!options.profiles.touch.enabled && !options.profiles.mouse.enabled &&
        !options.profiles.key.enabled && !options.profiles.gamepad.enabled &&
        !options.profiles.pen.enabled) {
        std::fprintf(stderr, "[ERROR] no profile was configured; see --help\n");
        return std::nullopt;
    }
    return options;
}

} // namespace aoap::cli
