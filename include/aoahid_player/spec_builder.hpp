// SPDX-License-Identifier: MIT
#pragma once

#include <aoahid.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "context.hpp"
#include "device.hpp"

// Turns the chosen profile settings into the immutable aoahid_spec objects a
// connection uses. Specs are built once when connecting and reused for every
// Node on every device; nothing here runs inside the playback loop.

namespace aoap {

struct TouchSetup {
    bool enabled{};
    int32_t width{};
    int32_t height{};
    uint32_t max_contacts{};
};

struct MouseSetup {
    bool enabled{};
    uint32_t buttons{};
};

struct KeySetup {
    bool enabled{};
    uint16_t usage_minimum{0x04};
    uint16_t usage_maximum{0x65};
};

struct GamepadSetup {
    bool enabled{};
    uint32_t buttons{};
    uint32_t axis_bits{};
    aoahid_dpad_representation dpad{AOAHID_DPAD_NONE};
    std::vector<aoahid_axis_role> axes;
};

struct PenSetup {
    bool enabled{};
    aoahid_pen_mode mode{AOAHID_PEN_DIRECT_SCREEN};
    int32_t width{};
    int32_t height{};
    int32_t pressure_maximum{4095};
};

struct ProfileSetup {
    TouchSetup touch;
    MouseSetup mouse;
    KeySetup key;
    GamepadSetup gamepad;
    PenSetup pen;
};

namespace spec_detail {

// Smallest width that represents 0..maximum, or a signed -maximum..maximum.
inline uint32_t unsigned_bits(const int64_t maximum) noexcept {
    uint32_t bits = 1;
    while (bits < 32 && (int64_t{1} << bits) - 1 < maximum)
        ++bits;
    return bits;
}

inline aoahid_integer_field field(const int32_t minimum, const int32_t maximum,
                                  const uint32_t bits) noexcept {
    aoahid_integer_field value{};
    value.logical_minimum = minimum;
    value.logical_maximum = maximum;
    value.bit_width = bits;
    return value;
}

struct AxisIdentity {
    std::string_view name;
    aoahid_axis_role role;
    uint16_t usage_page;
    uint16_t usage;
    const char* linux_code;
    const char* android_axis;
};

// Usage Page/Usage are the audited HUT 1.7 identities libaoahid re-checks. The
// Linux codes follow hidinput_configure_usage's Generic Desktop `usage & 0xf`
// and Simulation Controls mappings; the Android names are their MotionEvent
// counterparts. libaoahid stores both as required target evidence.
inline constexpr std::array<AxisIdentity, 14> axis_table{{
    {"x", AOAHID_AXIS_X, 0x01, 0x30, "ABS_X", "AXIS_X"},
    {"y", AOAHID_AXIS_Y, 0x01, 0x31, "ABS_Y", "AXIS_Y"},
    {"z", AOAHID_AXIS_Z, 0x01, 0x32, "ABS_Z", "AXIS_Z"},
    {"rx", AOAHID_AXIS_RX, 0x01, 0x33, "ABS_RX", "AXIS_RX"},
    {"ry", AOAHID_AXIS_RY, 0x01, 0x34, "ABS_RY", "AXIS_RY"},
    {"rz", AOAHID_AXIS_RZ, 0x01, 0x35, "ABS_RZ", "AXIS_RZ"},
    {"slider", AOAHID_AXIS_SLIDER, 0x01, 0x36, "ABS_THROTTLE", "AXIS_THROTTLE"},
    {"dial", AOAHID_AXIS_DIAL, 0x01, 0x37, "ABS_RUDDER", "AXIS_RUDDER"},
    {"wheel", AOAHID_AXIS_WHEEL, 0x01, 0x38, "ABS_WHEEL", "AXIS_WHEEL"},
    {"rudder", AOAHID_AXIS_SIMULATION_RUDDER, 0x02, 0xBA, "ABS_RUDDER", "AXIS_RUDDER"},
    {"throttle", AOAHID_AXIS_SIMULATION_THROTTLE, 0x02, 0xBB, "ABS_THROTTLE", "AXIS_THROTTLE"},
    {"accelerator", AOAHID_AXIS_SIMULATION_ACCELERATOR, 0x02, 0xC4, "ABS_GAS", "AXIS_GAS"},
    {"brake", AOAHID_AXIS_SIMULATION_BRAKE, 0x02, 0xC5, "ABS_BRAKE", "AXIS_BRAKE"},
    {"steering", AOAHID_AXIS_SIMULATION_STEERING, 0x02, 0xC8, "ABS_WHEEL", "AXIS_WHEEL"},
}};

inline const AxisIdentity* find_axis(const std::string_view name) noexcept {
    for (const AxisIdentity& entry : axis_table) {
        if (entry.name == name)
            return &entry;
    }
    return nullptr;
}

inline const AxisIdentity* find_axis(const aoahid_axis_role role) noexcept {
    for (const AxisIdentity& entry : axis_table) {
        if (entry.role == role)
            return &entry;
    }
    return nullptr;
}

// Accelerator/Brake/Throttle need 0..max with neutral 0; Steering/Rudder need
// a signed interval around zero. Every other role keeps the signed stick range.
inline bool axis_is_unipolar(const aoahid_axis_role role) noexcept {
    return role == AOAHID_AXIS_SIMULATION_ACCELERATOR || role == AOAHID_AXIS_SIMULATION_BRAKE ||
           role == AOAHID_AXIS_SIMULATION_THROTTLE;
}

} // namespace spec_detail

inline uint32_t enabled_profiles(const ProfileSetup& setup) noexcept {
    uint32_t mask = 0;
    if (setup.touch.enabled)
        mask |= profile_bit(Profile::touch);
    if (setup.mouse.enabled)
        mask |= profile_bit(Profile::mouse);
    if (setup.key.enabled)
        mask |= profile_bit(Profile::key);
    if (setup.gamepad.enabled)
        mask |= profile_bit(Profile::gamepad);
    if (setup.pen.enabled)
        mask |= profile_bit(Profile::pen);
    return mask;
}

// The CSV pen row has no resolution column, so the pen shares the touch
// surface when one is set and otherwise uses a unitless 0..32767 square the
// target maps onto its own screen.
inline void resolve_pen_surface(ProfileSetup& setup) noexcept {
    const bool touch = setup.touch.enabled && setup.touch.width > 0 && setup.touch.height > 0;
    setup.pen.width = touch ? setup.touch.width : 32768;
    setup.pen.height = touch ? setup.touch.height : 32768;
}

// Checks every enabled profile before any USB work. Returns false with a
// message a user can act on; libaoahid still performs its own full checks.
inline bool validate_setup(const ProfileSetup& setup, std::string& error) {
    if (enabled_profiles(setup) == 0U) {
        error = "Choose at least one profile to connect.";
        return false;
    }
    const auto size_ok = [](const int32_t value) { return value > 0 && value <= 65536; };
    if (setup.touch.enabled) {
        if (!size_ok(setup.touch.width) || !size_ok(setup.touch.height)) {
            error = "Set the touchscreen resolution (width and height, 1-65536).";
            return false;
        }
        if (setup.touch.max_contacts < 1 || setup.touch.max_contacts > 16) {
            error = "Touchscreen contacts must be between 1 and 16.";
            return false;
        }
    }
    if (setup.mouse.enabled && (setup.mouse.buttons < 1 || setup.mouse.buttons > 65535)) {
        error = "Mouse buttons must be between 1 and 65535.";
        return false;
    }
    if (setup.key.enabled &&
        (setup.key.usage_minimum < 0x04 || setup.key.usage_maximum < setup.key.usage_minimum ||
         setup.key.usage_maximum >= 0xE0)) {
        error = "The keyboard usage range must be ordered, start at 0x04 or above and end "
                "below 0xE0 (0xE0-0xE7 are the modifier keys).";
        return false;
    }
    if (setup.gamepad.enabled) {
        const GamepadSetup& pad = setup.gamepad;
        if (pad.buttons < 1 || pad.buttons > 65535) {
            error = "Gamepad buttons must be between 1 and 65535.";
            return false;
        }
        if (pad.axis_bits < 2 || pad.axis_bits > 32) {
            error = "Gamepad axis resolution must be between 2 and 32 bits.";
            return false;
        }
        bool has_x = false;
        bool has_y = false;
        for (size_t index = 0; index < pad.axes.size(); ++index) {
            has_x = has_x || pad.axes[index] == AOAHID_AXIS_X;
            has_y = has_y || pad.axes[index] == AOAHID_AXIS_Y;
            for (size_t other = 0; other < index; ++other) {
                if (pad.axes[other] == pad.axes[index]) {
                    error = "Each gamepad axis can only be listed once.";
                    return false;
                }
            }
        }
        if (!has_x || !has_y) {
            error = "The gamepad needs at least the X and Y axes.";
            return false;
        }
    }
    if (setup.pen.enabled && (!size_ok(setup.pen.width) || !size_ok(setup.pen.height))) {
        error = "The pen surface size is not set.";
        return false;
    }
    return true;
}

// Owns one immutable Spec per enabled profile for a whole connection.
class SpecSet {
  public:
    SpecSet() noexcept = default;
    ~SpecSet() { release(); }

    SpecSet(const SpecSet&) = delete;
    SpecSet& operator=(const SpecSet&) = delete;
    SpecSet(SpecSet&& other) noexcept : specs_(other.specs_) { other.specs_.fill(nullptr); }
    SpecSet& operator=(SpecSet&& other) noexcept {
        if (this != &other) {
            release();
            specs_ = other.specs_;
            other.specs_.fill(nullptr);
        }
        return *this;
    }

    [[nodiscard]] aoahid_spec* get(const Profile profile) const noexcept {
        return specs_[static_cast<size_t>(profile)];
    }
    [[nodiscard]] uint32_t mask() const noexcept {
        uint32_t value = 0;
        for (size_t index = 0; index < profile_count; ++index) {
            if (specs_[index] != nullptr)
                value |= uint32_t{1} << index;
        }
        return value;
    }

    void release() noexcept {
        for (aoahid_spec*& spec : specs_) {
            aoahid_spec_release(spec);
            spec = nullptr;
        }
    }

    // Builds every enabled profile. Returns false and fills `error` on the
    // first factory rejection, leaving already-built specs owned by this set.
    bool build(const ProfileSetup& setup, std::string& error) {
        release();
        if (setup.touch.enabled && !build_touch(setup.touch, error))
            return false;
        if (setup.mouse.enabled && !build_mouse(setup.mouse, error))
            return false;
        if (setup.key.enabled && !build_key(setup.key, error))
            return false;
        if (setup.gamepad.enabled && !build_gamepad(setup.gamepad, error))
            return false;
        if (setup.pen.enabled && !build_pen(setup.pen, error))
            return false;
        return true;
    }

  private:
    std::array<aoahid_spec*, profile_count> specs_{};

    bool store(const Profile profile, const aoahid_result result, aoahid_spec* spec,
               const char* what, std::string& error) {
        if (result != AOAHID_OK) {
            error = std::string("The ") + what + " settings were rejected: " +
                    explain_error(result);
            return false;
        }
        specs_[static_cast<size_t>(profile)] = spec;
        return true;
    }

    bool build_touch(const TouchSetup& setup, std::string& error) {
        using namespace spec_detail;
        aoahid_touch_options options{};
        options.struct_size = static_cast<uint32_t>(sizeof(options));
        options.maximum_contacts = setup.max_contacts;
        // One report carries every declared slot, so a frame is never split
        // across control transfers.
        options.contacts_per_report = setup.max_contacts;
        // Four bits cover the 0..15 Contact Identifier space that
        // maximum_contacts is itself capped at.
        options.contact_identifier = field(0, 15, 4);
        options.x = field(0, setup.width - 1, unsigned_bits(setup.width - 1));
        options.y = field(0, setup.height - 1, unsigned_bits(setup.height - 1));
        options.contact_count = field(0, static_cast<int32_t>(setup.max_contacts),
                                      unsigned_bits(static_cast<int64_t>(setup.max_contacts)));
        // The CSV touch row carries no pressure, geometry, or scan time, so
        // those optional fields stay out of the descriptor entirely.
        aoahid_spec* spec = nullptr;
        const aoahid_result result = aoahid_spec_create_touchscreen(&options, &spec);
        return store(Profile::touch, result, spec, "touchscreen", error);
    }

    bool build_mouse(const MouseSetup& setup, std::string& error) {
        using namespace spec_detail;
        aoahid_mouse_options options{};
        options.struct_size = static_cast<uint32_t>(sizeof(options));
        options.button_count = setup.buttons;
        // Relative deltas; libaoahid keeps a 64-bit pending total and emits
        // in-range fragments, so this range bounds one report, not one script
        // row. The wheel serves live control; AC Pan stays undeclared.
        options.x = field(-32767, 32767, 16);
        options.y = field(-32767, 32767, 16);
        options.enable_wheel = 1U;
        options.wheel = field(-127, 127, 8);
        aoahid_spec* spec = nullptr;
        const aoahid_result result = aoahid_spec_create_mouse(&options, &spec);
        return store(Profile::mouse, result, spec, "mouse", error);
    }

    bool build_key(const KeySetup& setup, std::string& error) {
        aoahid_keyboard_options options{};
        options.struct_size = static_cast<uint32_t>(sizeof(options));
        options.rollover = AOAHID_KEYBOARD_ARRAY;
        options.array_length = 6U;
        options.usage_minimum = setup.usage_minimum;
        options.usage_maximum = setup.usage_maximum;
        options.usage_bit_width = 8U;
        aoahid_spec* spec = nullptr;
        const aoahid_result result = aoahid_spec_create_keyboard(&options, &spec);
        return store(Profile::key, result, spec, "keyboard", error);
    }

    bool build_gamepad(const GamepadSetup& setup, std::string& error) {
        using namespace spec_detail;
        const uint32_t bits = setup.axis_bits;
        const int32_t signed_extent = static_cast<int32_t>((int64_t{1} << (bits - 1)) - 1);
        // A 32-bit unipolar axis would need 2^32-1, which no longer fits the
        // signed logical maximum, so it saturates at INT32_MAX.
        const int64_t unsigned_span = (int64_t{1} << bits) - 1;
        const int32_t unsigned_extent = static_cast<int32_t>(
            unsigned_span > int64_t{INT32_MAX} ? int64_t{INT32_MAX} : unsigned_span);

        std::vector<aoahid_gamepad_axis> axes;
        axes.reserve(setup.axes.size());
        for (const aoahid_axis_role role : setup.axes) {
            const AxisIdentity* identity = find_axis(role);
            if (identity == nullptr) {
                error = "The gamepad settings use an unsupported axis.";
                return false;
            }
            aoahid_gamepad_axis axis{};
            axis.role = identity->role;
            axis.usage_page = identity->usage_page;
            axis.usage = identity->usage;
            axis.value = axis_is_unipolar(role) ? field(0, unsigned_extent, bits)
                                                : field(-signed_extent, signed_extent, bits);
            axis.neutral_value = 0;
            axis.expected_linux_code = identity->linux_code;
            axis.expected_android_axis = identity->android_axis;
            axes.push_back(axis);
        }

        aoahid_gamepad_options options{};
        options.struct_size = static_cast<uint32_t>(sizeof(options));
        options.application = AOAHID_CONTROLLER_GAMEPAD;
        options.axes = axes.data();
        options.axis_count = axes.size();
        options.button_count = setup.buttons;
        options.button_usage_minimum = 1U;
        options.dpad_representation = setup.dpad;
        if (setup.dpad == AOAHID_DPAD_HAT) {
            // The Android portable-candidate Hat is fixed at logical 0..7 in
            // four bits, leaving 15 as the Null State encoding.
            options.hat_logical_minimum = 0;
            options.hat_logical_maximum = 7;
            options.hat_bit_width = 4U;
        }
        aoahid_spec* spec = nullptr;
        const aoahid_result result = aoahid_spec_create_gamepad(&options, &spec);
        return store(Profile::gamepad, result, spec, "gamepad", error);
    }

    bool build_pen(const PenSetup& setup, std::string& error) {
        using namespace spec_detail;
        aoahid_pen_options options{};
        options.struct_size = static_cast<uint32_t>(sizeof(options));
        options.mode = setup.mode;
        options.x = field(0, setup.width - 1, unsigned_bits(setup.width - 1));
        options.y = field(0, setup.height - 1, unsigned_bits(setup.height - 1));
        options.enable_pressure = 1U;
        options.pressure =
            field(0, setup.pressure_maximum, unsigned_bits(setup.pressure_maximum));
        // Hover must be declared, or libaoahid rejects the in_range=1, tip=0
        // sample the CSV pen row is able to express.
        options.enable_hover = 1U;
        aoahid_spec* spec = nullptr;
        const aoahid_result result = aoahid_spec_create_pen(&options, &spec);
        return store(Profile::pen, result, spec, "pen", error);
    }
};

} // namespace aoap
