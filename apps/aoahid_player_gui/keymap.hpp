// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace gui {

// HID Keyboard/Keypad usage for a GLFW key, 0 when the key has none. GLFW
// names keys by their position on a US layout, which is also how HID usages
// work, so the phone applies its own layout to them.
uint16_t hid_usage_from_glfw(int key) noexcept;

// HID usage for a key JIS (Japanese) keyboards have and US layouts do not —
// Henkan, Muhenkan, Kana, Zenkaku/Hankaku, and the extra Ro key — identified
// from the raw platform scancode GLFW hands the key callback, since GLFW has
// no GLFW_KEY_* constant for any of them (they decode to GLFW_KEY_UNKNOWN on
// every backend). Returns 0 for a scancode that is not one of these keys.
// Linux only (X11 and Wayland); on Windows and macOS 0 is always returned,
// so this never changes behaviour there.
uint16_t hid_usage_from_scancode(int scancode) noexcept;

// The key that hands keyboard and pointer back to the computer; it is never
// forwarded. Configurable — see Settings::live_release_key.
bool is_release_key(int key, int configured_key) noexcept;

// Short label for a usage, such as "A", "Enter", or "Shift"; "0x87" style
// for anything without a name.
const char* hid_usage_name(uint16_t usage) noexcept;

// Short label for a GLFW key, for naming the configurable release key in the
// UI — "Escape", "Right Ctrl", "F9". Falls back to the HID usage's own name
// for anything hid_usage_from_glfw() understands, and to "Key <code>" for
// the rest (a key with no printable name, such as a media key GLFW reports
// with no GLFW_KEY_* of its own).
const char* glfw_key_name(int key) noexcept;

// HID usage (and whether Left Shift must be held) for one ASCII character of
// pasted clipboard text: letters, digits, the printable symbols a US layout
// types unshifted or shifted, space, tab, and newline (mapped to Enter).
// Returns false for anything else, including every non-ASCII character, so
// the caller can skip it.
bool hid_usage_from_ascii(char c, uint16_t& usage, bool& shift) noexcept;

} // namespace gui
