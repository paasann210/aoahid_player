// SPDX-License-Identifier: MIT
#include "keymap.hpp"

#include <GLFW/glfw3.h>

#include <cstdio>

#if defined(__linux__)
// linux/input-event-codes.h defines these, but pulling in the kernel header
// just for five constants (and risking a name clash with GLFW's own key
// macros) is not worth it; they have not changed since the 2.6 days.
namespace linux_evdev {
constexpr int key_zenkaku_hankaku = 85;
constexpr int key_ro = 89;
constexpr int key_henkan = 92;
constexpr int key_katakanahiragana = 93;
constexpr int key_muhenkan = 94;
constexpr int key_yen = 124;
} // namespace linux_evdev
#endif

namespace gui {

uint16_t hid_usage_from_glfw(const int key) noexcept {
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z)
        return static_cast<uint16_t>(0x04 + (key - GLFW_KEY_A));
    if (key >= GLFW_KEY_1 && key <= GLFW_KEY_9)
        return static_cast<uint16_t>(0x1E + (key - GLFW_KEY_1));
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F12)
        return static_cast<uint16_t>(0x3A + (key - GLFW_KEY_F1));
    if (key >= GLFW_KEY_F13 && key <= GLFW_KEY_F24)
        return static_cast<uint16_t>(0x68 + (key - GLFW_KEY_F13));
    if (key >= GLFW_KEY_KP_1 && key <= GLFW_KEY_KP_9)
        return static_cast<uint16_t>(0x59 + (key - GLFW_KEY_KP_1));
    switch (key) {
    case GLFW_KEY_0: return 0x27;
    case GLFW_KEY_ENTER: return 0x28;
    case GLFW_KEY_ESCAPE: return 0x29;
    case GLFW_KEY_BACKSPACE: return 0x2A;
    case GLFW_KEY_TAB: return 0x2B;
    case GLFW_KEY_SPACE: return 0x2C;
    case GLFW_KEY_MINUS: return 0x2D;
    case GLFW_KEY_EQUAL: return 0x2E;
    case GLFW_KEY_LEFT_BRACKET: return 0x2F;
    case GLFW_KEY_RIGHT_BRACKET: return 0x30;
    case GLFW_KEY_BACKSLASH: return 0x31;
    case GLFW_KEY_SEMICOLON: return 0x33;
    case GLFW_KEY_APOSTROPHE: return 0x34;
    case GLFW_KEY_GRAVE_ACCENT: return 0x35;
    case GLFW_KEY_COMMA: return 0x36;
    case GLFW_KEY_PERIOD: return 0x37;
    case GLFW_KEY_SLASH: return 0x38;
    case GLFW_KEY_CAPS_LOCK: return 0x39;
    case GLFW_KEY_PRINT_SCREEN: return 0x46;
    case GLFW_KEY_SCROLL_LOCK: return 0x47;
    case GLFW_KEY_PAUSE: return 0x48;
    case GLFW_KEY_INSERT: return 0x49;
    case GLFW_KEY_HOME: return 0x4A;
    case GLFW_KEY_PAGE_UP: return 0x4B;
    case GLFW_KEY_DELETE: return 0x4C;
    case GLFW_KEY_END: return 0x4D;
    case GLFW_KEY_PAGE_DOWN: return 0x4E;
    case GLFW_KEY_RIGHT: return 0x4F;
    case GLFW_KEY_LEFT: return 0x50;
    case GLFW_KEY_DOWN: return 0x51;
    case GLFW_KEY_UP: return 0x52;
    case GLFW_KEY_NUM_LOCK: return 0x53;
    case GLFW_KEY_KP_DIVIDE: return 0x54;
    case GLFW_KEY_KP_MULTIPLY: return 0x55;
    case GLFW_KEY_KP_SUBTRACT: return 0x56;
    case GLFW_KEY_KP_ADD: return 0x57;
    case GLFW_KEY_KP_ENTER: return 0x58;
    case GLFW_KEY_KP_0: return 0x62;
    case GLFW_KEY_KP_DECIMAL: return 0x63;
    case GLFW_KEY_WORLD_2: return 0x64; // the extra key beside Shift on ISO boards
    case GLFW_KEY_MENU: return 0x65;
    case GLFW_KEY_KP_EQUAL: return 0x67;
    case GLFW_KEY_LEFT_CONTROL: return 0xE0;
    case GLFW_KEY_LEFT_SHIFT: return 0xE1;
    case GLFW_KEY_LEFT_ALT: return 0xE2;
    case GLFW_KEY_LEFT_SUPER: return 0xE3;
    case GLFW_KEY_RIGHT_SHIFT: return 0xE5;
    case GLFW_KEY_RIGHT_ALT: return 0xE6;
    case GLFW_KEY_RIGHT_SUPER: return 0xE7;
    default: return 0;
    }
}

uint16_t hid_usage_from_scancode(const int scancode) noexcept {
#if defined(__linux__)
    if (scancode <= 0)
        return 0;
    // X11 keycodes are evdev codes offset by 8 (XKB's minimum keycode);
    // Wayland hands GLFW the evdev code directly. GLFW_PLATFORM_X11/WAYLAND
    // exist on every backend GLFW builds for on Linux, so this is safe to
    // call unconditionally there.
    const int evdev = glfwGetPlatform() == GLFW_PLATFORM_X11 ? scancode - 8 : scancode;
    switch (evdev) {
    case linux_evdev::key_zenkaku_hankaku: return 0x94; // Zenkaku/Hankaku
    case linux_evdev::key_ro: return 0x87;              // International1 (Ro, \ _)
    case linux_evdev::key_henkan: return 0x8A;          // International4 (Henkan)
    case linux_evdev::key_katakanahiragana: return 0x88; // International2 (Katakana/Hiragana)
    case linux_evdev::key_muhenkan: return 0x8B;         // International5 (Muhenkan)
    case linux_evdev::key_yen: return 0x89;              // International3 (Yen)
    default: return 0;
    }
#else
    (void)scancode;
    return 0;
#endif
}

bool is_release_key(const int key, const int configured_key) noexcept {
    // 0 means "not set yet"; Escape is the default so live control never
    // starts without a way out.
    return key == (configured_key != 0 ? configured_key : GLFW_KEY_ESCAPE);
}

const char* hid_usage_name(const uint16_t usage) noexcept {
    static const char* const letters[26] = {"A", "B", "C", "D", "E", "F", "G", "H", "I",
                                            "J", "K", "L", "M", "N", "O", "P", "Q", "R",
                                            "S", "T", "U", "V", "W", "X", "Y", "Z"};
    static const char* const digits[10] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
    static const char* const functions[12] = {"F1", "F2", "F3", "F4",  "F5",  "F6",
                                              "F7", "F8", "F9", "F10", "F11", "F12"};
    if (usage >= 0x04 && usage <= 0x1D)
        return letters[usage - 0x04];
    if (usage >= 0x1E && usage <= 0x27)
        return digits[usage - 0x1E];
    if (usage >= 0x3A && usage <= 0x45)
        return functions[usage - 0x3A];
    switch (usage) {
    case 0x28: return "Enter";
    case 0x29: return "Esc";
    case 0x2A: return "Backspace";
    case 0x2B: return "Tab";
    case 0x2C: return "Space";
    case 0x2D: return "-";
    case 0x2E: return "=";
    case 0x2F: return "[";
    case 0x30: return "]";
    case 0x31: return "\\";
    case 0x33: return ";";
    case 0x34: return "'";
    case 0x35: return "`";
    case 0x36: return ",";
    case 0x37: return ".";
    case 0x38: return "/";
    case 0x39: return "Caps Lock";
    case 0x49: return "Insert";
    case 0x4A: return "Home";
    case 0x4B: return "Page Up";
    case 0x4C: return "Delete";
    case 0x4D: return "End";
    case 0x4E: return "Page Down";
    case 0x4F: return "Right";
    case 0x50: return "Left";
    case 0x51: return "Down";
    case 0x52: return "Up";
    case 0x58: return "Keypad Enter";
    case 0x65: return "Menu";
    case 0x87: return "Ro";
    case 0x88: return "Katakana/Hiragana";
    case 0x89: return "Yen";
    case 0x8A: return "Henkan";
    case 0x8B: return "Muhenkan";
    case 0x94: return "Zenkaku/Hankaku";
    case 0xE0: return "Ctrl";
    case 0xE1: return "Shift";
    case 0xE2: return "Alt";
    case 0xE3: return "Meta";
    case 0xE4: return "Right Ctrl";
    case 0xE5: return "Right Shift";
    case 0xE6: return "Right Alt";
    case 0xE7: return "Right Meta";
    default: break;
    }
    // Everything else by number; one buffer is enough for the log's use.
    static thread_local char text[8];
    std::snprintf(text, sizeof text, "0x%02X", static_cast<unsigned>(usage));
    return text;
}

const char* glfw_key_name(const int key) noexcept {
    switch (key) {
    case GLFW_KEY_ESCAPE: return "Escape";
    case GLFW_KEY_LEFT_CONTROL: return "Left Ctrl";
    case GLFW_KEY_RIGHT_CONTROL: return "Right Ctrl";
    case GLFW_KEY_LEFT_SHIFT: return "Left Shift";
    case GLFW_KEY_RIGHT_SHIFT: return "Right Shift";
    case GLFW_KEY_LEFT_ALT: return "Left Alt";
    case GLFW_KEY_RIGHT_ALT: return "Right Alt";
    case GLFW_KEY_LEFT_SUPER: return "Left Super";
    case GLFW_KEY_RIGHT_SUPER: return "Right Super";
    case GLFW_KEY_TAB: return "Tab";
    case GLFW_KEY_CAPS_LOCK: return "Caps Lock";
    case GLFW_KEY_MENU: return "Menu";
    case GLFW_KEY_PAUSE: return "Pause";
    case GLFW_KEY_SCROLL_LOCK: return "Scroll Lock";
    case GLFW_KEY_PRINT_SCREEN: return "Print Screen";
    case GLFW_KEY_INSERT: return "Insert";
    default: break;
    }
    const uint16_t usage = hid_usage_from_glfw(key);
    if (usage != 0)
        return hid_usage_name(usage);
    static thread_local char text[16];
    std::snprintf(text, sizeof text, "Key %d", key);
    return text;
}

} // namespace gui
