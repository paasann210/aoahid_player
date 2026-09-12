// SPDX-License-Identifier: MIT
#include "aoahid_player/getevent_parser.hpp"

#include <charconv>
#include <cstdlib>

namespace aoap::record {
namespace {

constexpr int64_t ns_per_sec = 1'000'000'000;

struct KeyMapping {
    std::string_view name;
    uint16_t linux_code;
    uint16_t hid_usage;
};

// Linux input-event-codes.h keycodes to HID Usage Table 1.7 section 10
// Keyboard/Keypad usages. Only keys the keyboard profile can express are
// listed; Consumer-page keys such as KEY_VOLUMEUP have no Usage here.
constexpr std::array<KeyMapping, 113> key_table{{
    {"KEY_ESC", 1, 0x29},          {"KEY_1", 2, 0x1E},
    {"KEY_2", 3, 0x1F},            {"KEY_3", 4, 0x20},
    {"KEY_4", 5, 0x21},            {"KEY_5", 6, 0x22},
    {"KEY_6", 7, 0x23},            {"KEY_7", 8, 0x24},
    {"KEY_8", 9, 0x25},            {"KEY_9", 10, 0x26},
    {"KEY_0", 11, 0x27},           {"KEY_MINUS", 12, 0x2D},
    {"KEY_EQUAL", 13, 0x2E},       {"KEY_BACKSPACE", 14, 0x2A},
    {"KEY_TAB", 15, 0x2B},         {"KEY_Q", 16, 0x14},
    {"KEY_W", 17, 0x1A},           {"KEY_E", 18, 0x08},
    {"KEY_R", 19, 0x15},           {"KEY_T", 20, 0x17},
    {"KEY_Y", 21, 0x1C},           {"KEY_U", 22, 0x18},
    {"KEY_I", 23, 0x0C},           {"KEY_O", 24, 0x12},
    {"KEY_P", 25, 0x13},           {"KEY_LEFTBRACE", 26, 0x2F},
    {"KEY_RIGHTBRACE", 27, 0x30},  {"KEY_ENTER", 28, 0x28},
    {"KEY_LEFTCTRL", 29, 0xE0},    {"KEY_A", 30, 0x04},
    {"KEY_S", 31, 0x16},           {"KEY_D", 32, 0x07},
    {"KEY_F", 33, 0x09},           {"KEY_G", 34, 0x0A},
    {"KEY_H", 35, 0x0B},           {"KEY_J", 36, 0x0D},
    {"KEY_K", 37, 0x0E},           {"KEY_L", 38, 0x0F},
    {"KEY_SEMICOLON", 39, 0x33},   {"KEY_APOSTROPHE", 40, 0x34},
    {"KEY_GRAVE", 41, 0x35},       {"KEY_LEFTSHIFT", 42, 0xE1},
    {"KEY_BACKSLASH", 43, 0x31},   {"KEY_Z", 44, 0x1D},
    {"KEY_X", 45, 0x1B},           {"KEY_C", 46, 0x06},
    {"KEY_V", 47, 0x19},           {"KEY_B", 48, 0x05},
    {"KEY_N", 49, 0x11},           {"KEY_M", 50, 0x10},
    {"KEY_COMMA", 51, 0x36},       {"KEY_DOT", 52, 0x37},
    {"KEY_SLASH", 53, 0x38},       {"KEY_RIGHTSHIFT", 54, 0xE5},
    {"KEY_KPASTERISK", 55, 0x55},  {"KEY_LEFTALT", 56, 0xE2},
    {"KEY_SPACE", 57, 0x2C},       {"KEY_CAPSLOCK", 58, 0x39},
    {"KEY_F1", 59, 0x3A},          {"KEY_F2", 60, 0x3B},
    {"KEY_F3", 61, 0x3C},          {"KEY_F4", 62, 0x3D},
    {"KEY_F5", 63, 0x3E},          {"KEY_F6", 64, 0x3F},
    {"KEY_F7", 65, 0x40},          {"KEY_F8", 66, 0x41},
    {"KEY_F9", 67, 0x42},          {"KEY_F10", 68, 0x43},
    {"KEY_NUMLOCK", 69, 0x53},     {"KEY_SCROLLLOCK", 70, 0x47},
    {"KEY_KP7", 71, 0x5F},         {"KEY_KP8", 72, 0x60},
    {"KEY_KP9", 73, 0x61},         {"KEY_KPMINUS", 74, 0x56},
    {"KEY_KP4", 75, 0x5C},         {"KEY_KP5", 76, 0x5D},
    {"KEY_KP6", 77, 0x5E},         {"KEY_KPPLUS", 78, 0x57},
    {"KEY_KP1", 79, 0x59},         {"KEY_KP2", 80, 0x5A},
    {"KEY_KP3", 81, 0x5B},         {"KEY_KP0", 82, 0x62},
    {"KEY_KPDOT", 83, 0x63},       {"KEY_F11", 87, 0x44},
    {"KEY_F12", 88, 0x45},         {"KEY_KPENTER", 96, 0x58},
    {"KEY_RIGHTCTRL", 97, 0xE4},   {"KEY_KPSLASH", 98, 0x54},
    {"KEY_SYSRQ", 99, 0x46},       {"KEY_RIGHTALT", 100, 0xE6},
    {"KEY_HOME", 102, 0x4A},       {"KEY_UP", 103, 0x52},
    {"KEY_PAGEUP", 104, 0x4B},     {"KEY_LEFT", 105, 0x50},
    {"KEY_RIGHT", 106, 0x4F},      {"KEY_END", 107, 0x4D},
    {"KEY_DOWN", 108, 0x51},       {"KEY_PAGEDOWN", 109, 0x4E},
    {"KEY_INSERT", 110, 0x49},     {"KEY_DELETE", 111, 0x4C},
    {"KEY_PAUSE", 119, 0x48},      {"KEY_LEFTMETA", 125, 0xE3},
    {"KEY_RIGHTMETA", 126, 0xE7},  {"KEY_COMPOSE", 127, 0x65},
    {"KEY_F13", 183, 0x68},        {"KEY_F14", 184, 0x69},
    {"KEY_F15", 185, 0x6A},        {"KEY_F16", 186, 0x6B},
    {"KEY_F17", 187, 0x6C},        {"KEY_F18", 188, 0x6D},
    {"KEY_F19", 189, 0x6E},        {"KEY_F20", 190, 0x6F},
    {"KEY_F21", 191, 0x70},
}};

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
                             text.back() == '\n'))
        text.remove_suffix(1);
    return text;
}

std::string_view next_token(std::string_view& text) noexcept {
    text = trim(text);
    const size_t end = text.find_first_of(" \t");
    const std::string_view token = end == std::string_view::npos ? text : text.substr(0, end);
    text.remove_prefix(token.size());
    return token;
}

bool parse_hex32(const std::string_view text, uint32_t& out) noexcept {
    if (text.empty())
        return false;
    const char* last = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(text.data(), last, out, 16);
    return result.ec == std::errc{} && result.ptr == last;
}

// "[   85881.911639] " when getevent ran with -t. Returns -1 when absent.
int64_t parse_timestamp(std::string_view& text) noexcept {
    text = trim(text);
    if (text.empty() || text.front() != '[')
        return -1;
    const size_t close = text.find(']');
    if (close == std::string_view::npos)
        return -1;
    const std::string stamp(trim(text.substr(1, close - 1)));
    text.remove_prefix(close + 1);
    char* end = nullptr;
    const double seconds = std::strtod(stamp.c_str(), &end);
    if (end == stamp.c_str() || seconds < 0.0)
        return -1;
    return static_cast<int64_t>(seconds * static_cast<double>(ns_per_sec));
}

} // namespace

bool GeteventParser::key_usage(const std::string_view token, uint16_t& usage) noexcept {
    for (const KeyMapping& entry : key_table) {
        if (entry.name == token) {
            usage = entry.hid_usage;
            return true;
        }
    }
    // getevent prints the raw hex code when it has no label for it.
    uint32_t code = 0;
    if (!parse_hex32(token, code))
        return false;
    for (const KeyMapping& entry : key_table) {
        if (entry.linux_code == code) {
            usage = entry.hid_usage;
            return true;
        }
    }
    return false;
}

GeteventParser::DeviceState& GeteventParser::state_for(const std::string& device) {
    return devices_[device];
}

void GeteventParser::stage(const EventPayload& payload) {
    frame_rows_.push_back(EventRecord{payload, false, 0});
}

void GeteventParser::release_pending(const int64_t timestamp_ns, std::vector<EventRecord>& out) {
    if (pending_.empty())
        return;
    int64_t wait_ns = 0;
    if (pending_time_ns_ >= 0 && timestamp_ns >= pending_time_ns_)
        wait_ns = timestamp_ns - pending_time_ns_;
    // Only the last row of a frame waits; the rest are batched into it, which
    // is what a wait_ms of zero means to aoa_touch.
    pending_.back().wait_ns = wait_ns;
    out.insert(out.end(), pending_.begin(), pending_.end());
    pending_.clear();
}

void GeteventParser::close_frame(DeviceState& state, const int64_t timestamp_ns,
                                 std::vector<EventRecord>& out) {
    if (state.frame_dirty) {
        state.frame_dirty = false;
        for (size_t index = 0; index < max_slots; ++index) {
            Slot& slot = state.slots[index];
            if (!slot.changed)
                continue;
            slot.changed = false;
            if (slot.lifting) {
                slot.lifting = false;
                slot.active = false;
                if (slot.placed) {
                    slot.placed = false;
                    stage(TouchEvent{static_cast<int>(index), false, slot.x, slot.y});
                }
                continue;
            }
            if (slot.active) {
                slot.placed = true;
                stage(TouchEvent{static_cast<int>(index), true, slot.x, slot.y});
            }
        }
    }
    if (frame_rows_.empty())
        return;
    // The previous frame's duration is now known, so it can be written out.
    release_pending(timestamp_ns, out);
    pending_.swap(frame_rows_);
    frame_rows_.clear();
    pending_time_ns_ = timestamp_ns;
}

void GeteventParser::feed(std::string_view line, const int64_t host_now_ns,
                          std::vector<EventRecord>& out) {
    line = trim(line);
    if (line.empty())
        return;

    int64_t timestamp = parse_timestamp(line);
    if (timestamp < 0)
        timestamp = host_now_ns;

    line = trim(line);
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos)
        return;
    const std::string device(trim(line.substr(0, colon)));
    if (device.empty() || device.front() != '/')
        return;
    if (!filter_.empty() && device != filter_)
        return;
    line.remove_prefix(colon + 1);

    const std::string_view type = next_token(line);
    const std::string_view code = next_token(line);
    const std::string_view value = next_token(line);
    if (type.empty() || code.empty())
        return;

    DeviceState& state = state_for(device);

    if (type == "EV_SYN") {
        if (code == "SYN_REPORT")
            close_frame(state, timestamp, out);
        return;
    }

    if (type == "EV_ABS") {
        uint32_t raw = 0;
        if (!parse_hex32(value, raw))
            return;
        if (code == "ABS_MT_SLOT") {
            if (raw < max_slots)
                state.slot = raw;
            return;
        }
        if (state.slot >= max_slots)
            return;
        Slot& slot = state.slots[state.slot];
        if (code == "ABS_MT_TRACKING_ID") {
            if (raw == 0xFFFFFFFFU) {
                if (slot.active) {
                    slot.lifting = true;
                    slot.changed = true;
                    state.frame_dirty = true;
                }
            } else if (!slot.active) {
                slot.active = true;
                slot.lifting = false;
                slot.changed = true;
                state.frame_dirty = true;
            }
            return;
        }
        if (code == "ABS_MT_POSITION_X" || code == "ABS_X") {
            slot.x = static_cast<int32_t>(raw);
        } else if (code == "ABS_MT_POSITION_Y" || code == "ABS_Y") {
            slot.y = static_cast<int32_t>(raw);
        } else {
            return;
        }
        if (slot.active) {
            slot.changed = true;
            state.frame_dirty = true;
        }
        return;
    }

    if (type == "EV_KEY") {
        bool down = false;
        if (value == "DOWN") {
            down = true;
        } else if (value == "UP") {
            down = false;
        } else {
            uint32_t raw = 0;
            if (!parse_hex32(value, raw))
                return;
            if (raw > 1)
                return; // key repeat (2) is not an edge
            down = raw == 1;
        }
        uint16_t usage = 0;
        if (!key_usage(code, usage)) {
            ++unmapped_keys_;
            if (first_unmapped_key_.empty())
                first_unmapped_key_.assign(code);
            return;
        }
        // The row joins the frame that the next SYN_REPORT closes.
        stage(KeyEvent{usage, down});
        return;
    }
}

void GeteventParser::finish(std::vector<EventRecord>& out) {
    out.insert(out.end(), pending_.begin(), pending_.end());
    pending_.clear();
    out.insert(out.end(), frame_rows_.begin(), frame_rows_.end());
    frame_rows_.clear();
}

namespace {

// The integer after `key` ("min " or "max ") in one getevent -p axis line.
bool field_after(const std::string_view line, const std::string_view key, long& value) {
    const size_t at = line.find(key);
    if (at == std::string_view::npos)
        return false;
    const char* begin = line.data() + at + key.size();
    const char* end = line.data() + line.size();
    while (begin < end && *begin == ' ')
        ++begin;
    const auto [stop, error] = std::from_chars(begin, end, value);
    return error == std::errc{} && stop != begin;
}

} // namespace

bool parse_touch_range(const std::string_view text, const std::string_view device, int32_t& width,
                       int32_t& height) {
    std::string_view current;
    long max_x = -1;
    long max_y = -1;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        const std::string_view line = text.substr(start, end - start);
        start = end + 1;

        constexpr std::string_view added = "add device ";
        const size_t add = line.find(added);
        if (add != std::string_view::npos) {
            const size_t colon = line.find(':', add);
            current = colon == std::string_view::npos ? std::string_view() : line.substr(colon + 1);
            while (!current.empty() && (current.front() == ' ' || current.front() == '\t'))
                current.remove_prefix(1);
            while (!current.empty() && (current.back() == '\r' || current.back() == ' '))
                current.remove_suffix(1);
            max_x = -1;
            max_y = -1;
            continue;
        }
        if (!device.empty() && current != device)
            continue;
        const bool is_x = line.find("ABS_MT_POSITION_X") != std::string_view::npos;
        const bool is_y = line.find("ABS_MT_POSITION_Y") != std::string_view::npos;
        if (!is_x && !is_y)
            continue;
        long minimum = 0;
        long maximum = 0;
        if (!field_after(line, "min ", minimum) || !field_after(line, "max ", maximum))
            continue;
        if (minimum != 0 || maximum < 1 || maximum > 65535)
            continue;
        (is_x ? max_x : max_y) = maximum;
        if (max_x > 0 && max_y > 0) {
            width = static_cast<int32_t>(max_x + 1);
            height = static_cast<int32_t>(max_y + 1);
            return true;
        }
    }
    return false;
}

} // namespace aoap::record
