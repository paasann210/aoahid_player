// SPDX-License-Identifier: MIT
#pragma once

#include <aoahid.h>
#include <aoahid.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "context.hpp"

namespace aoap {

// The profile families this player drives. The CSV prefixes in README.md map
// one-to-one onto these; libaoahid's toggle/battery/raw profiles are out of
// scope for this project.
enum class Profile : size_t { touch = 0, mouse, key, gamepad, pen, count };

constexpr size_t profile_count = static_cast<size_t>(Profile::count);
constexpr uint32_t profile_bit(Profile profile) noexcept {
    return uint32_t{1} << static_cast<size_t>(profile);
}
const char* profile_name(Profile profile) noexcept;  // "touch", "key", ... (CLI flags)
const char* profile_title(Profile profile) noexcept; // "Touchscreen", "Keyboard", ...

// "touchscreen, keyboard and pen" for the profiles set in `mask`.
std::string describe_profiles(uint32_t mask);

// RAII wrapper around aoahid_device. Owns Nodes opened against it for the
// profiles the connection was configured with (touch/mouse/key/gamepad/pen),
// each built from a Spec constructed once when connecting.
class Device {
  public:
    Device() noexcept = default;
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;

    // `selected` is borrowed for the call only; identity strings are copied.
    aoahid_result open(const Context& context, const aoahid_device_info* selected);
    void close() noexcept;

    // Opens one Node for `spec` and binds the matching typed node_ref. The
    // Spec is retained by the Node, so the caller keeps its own reference.
    aoahid_result open_node(Profile profile, aoahid_spec* spec) noexcept;

    [[nodiscard]] bool has(Profile profile) const noexcept {
        return nodes_[static_cast<size_t>(profile)] != nullptr;
    }
    [[nodiscard]] uint32_t profile_mask() const noexcept { return profile_mask_; }
    [[nodiscard]] aoahid_node* node(Profile profile) const noexcept {
        return nodes_[static_cast<size_t>(profile)];
    }

    [[nodiscard]] const aoa::touchscreen_node_ref& touch() const noexcept { return touch_; }
    [[nodiscard]] const aoa::mouse_node_ref& mouse() const noexcept { return mouse_; }
    [[nodiscard]] const aoa::keyboard_node_ref& key() const noexcept { return key_; }
    [[nodiscard]] const aoa::gamepad_node_ref& gamepad() const noexcept { return gamepad_; }
    [[nodiscard]] const aoa::pen_node_ref& pen() const noexcept { return pen_; }

    [[nodiscard]] aoahid_device* native_handle() const noexcept { return handle_; }
    [[nodiscard]] const std::string& label() const noexcept { return label_; }

  private:
    aoahid_device* handle_{};
    std::array<aoahid_node*, profile_count> nodes_{};
    uint32_t profile_mask_{};

    aoa::touchscreen_node_ref touch_{};
    aoa::mouse_node_ref mouse_{};
    aoa::keyboard_node_ref key_{};
    aoa::gamepad_node_ref gamepad_{};
    aoa::pen_node_ref pen_{};

    std::string label_;
};

// Human-readable "product (vid:pid) serial" used by the picker and the error
// summary. Safe against null strings in aoahid_device_info.
std::string device_label(const aoahid_device_info* info);

} // namespace aoap
