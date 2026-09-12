// SPDX-License-Identifier: MIT
#include "aoahid_player/event_script.hpp"

#include "aoahid_player/device_group.hpp"
#include "aoahid_player/paths.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <string_view>

namespace aoap {
namespace {

constexpr int64_t ns_per_ms = 1'000'000;

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r' || text.back() == '\n'))
        text.remove_suffix(1);
    return text;
}

// Splits on ',' into at most `capacity` trimmed fields. Returns the count, or
// capacity + 1 when there are extra columns so the caller can reject the row.
size_t split_fields(std::string_view line, std::string_view* fields, const size_t capacity) {
    size_t count = 0;
    while (true) {
        const size_t comma = line.find(',');
        std::string_view field = comma == std::string_view::npos ? line : line.substr(0, comma);
        if (count < capacity)
            fields[count] = trim(field);
        ++count;
        if (comma == std::string_view::npos)
            break;
        line.remove_prefix(comma + 1);
    }
    return count;
}

bool parse_int64(std::string_view text, int64_t& out) noexcept {
    if (text.empty())
        return false;
    int base = 10;
    bool negative = false;
    if (text.front() == '+' || text.front() == '-') {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty())
        return false;
    uint64_t magnitude = 0;
    const char* first = text.data();
    const char* last = first + text.size();
    const std::from_chars_result result = std::from_chars(first, last, magnitude, base);
    if (result.ec != std::errc{} || result.ptr != last)
        return false;
    const uint64_t limit = negative ? uint64_t{1} << 63 : (uint64_t{1} << 63) - 1U;
    if (magnitude > limit)
        return false;
    out = negative ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude);
    return true;
}

bool parse_bounded(std::string_view text, const int64_t minimum, const int64_t maximum,
                   int64_t& out) noexcept {
    return parse_int64(text, out) && out >= minimum && out <= maximum;
}

bool parse_flag(std::string_view text, bool& out) noexcept {
    int64_t value = 0;
    if (!parse_bounded(text, 0, 1, value))
        return false;
    out = value == 1;
    return true;
}

// wait_ms accepts a decimal value, matching the original aoa_touch.c script
// format. The single floating-point multiply happens here, at parse time.
bool parse_wait_ns(std::string_view text, int64_t& out) noexcept {
    if (text.empty())
        return false;
    const std::string buffer(text);
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(buffer.c_str(), &end);
    if (end == buffer.c_str() || *end != '\0' || errno == ERANGE)
        return false;
    if (!(value >= 0.0) || value > 1.0e9)
        return false;
    out = static_cast<int64_t>(std::llround(value * static_cast<double>(ns_per_ms)));
    return true;
}

bool parse_row(const char prefix, std::string_view* field, const size_t count,
               EventPayload& payload, std::string& reason) {
    int64_t values[6]{};
    switch (prefix) {
    case 't': {
        if (count != 5) {
            reason = "touch needs finger_id,state,x,y,wait_ms";
            return false;
        }
        bool state = false;
        if (!parse_bounded(field[0], 0, 65535, values[0]) || !parse_flag(field[1], state) ||
            !parse_bounded(field[2], INT32_MIN, INT32_MAX, values[2]) ||
            !parse_bounded(field[3], INT32_MIN, INT32_MAX, values[3])) {
            reason = "touch column is not a valid number";
            return false;
        }
        payload = TouchEvent{static_cast<int>(values[0]), state,
                             static_cast<int32_t>(values[2]), static_cast<int32_t>(values[3])};
        return true;
    }
    case 'm': {
        if (count != 3) {
            reason = "mouse move needs dx,dy,wait_ms";
            return false;
        }
        if (!parse_bounded(field[0], INT32_MIN, INT32_MAX, values[0]) ||
            !parse_bounded(field[1], INT32_MIN, INT32_MAX, values[1])) {
            reason = "mouse delta is not a valid number";
            return false;
        }
        payload = MouseMove{static_cast<int32_t>(values[0]), static_cast<int32_t>(values[1])};
        return true;
    }
    case 'b': {
        if (count != 3) {
            reason = "mouse button needs button_no,pressed,wait_ms";
            return false;
        }
        bool pressed = false;
        if (!parse_bounded(field[0], 1, 65535, values[0]) || !parse_flag(field[1], pressed)) {
            reason = "mouse button number must be 1-based and pressed must be 0 or 1";
            return false;
        }
        payload = MouseButton{static_cast<uint32_t>(values[0]), pressed};
        return true;
    }
    case 'k': {
        if (count != 3) {
            reason = "key needs usage,down,wait_ms";
            return false;
        }
        bool down = false;
        if (!parse_bounded(field[0], 0, 0xFFFF, values[0]) || !parse_flag(field[1], down)) {
            reason = "key usage must be 0..0xffff and down must be 0 or 1";
            return false;
        }
        payload = KeyEvent{static_cast<uint16_t>(values[0]), down};
        return true;
    }
    case 'g': {
        if (count != 3) {
            reason = "gamepad button needs button_no,pressed,wait_ms";
            return false;
        }
        bool pressed = false;
        if (!parse_bounded(field[0], 1, 65535, values[0]) || !parse_flag(field[1], pressed)) {
            reason = "gamepad button number must be 1-based and pressed must be 0 or 1";
            return false;
        }
        payload = GamepadButton{static_cast<uint32_t>(values[0]), pressed};
        return true;
    }
    case 'a': {
        if (count != 3) {
            reason = "gamepad axis needs axis_index,value,wait_ms";
            return false;
        }
        if (!parse_bounded(field[0], 0, 65535, values[0]) ||
            !parse_bounded(field[1], INT32_MIN, INT32_MAX, values[1])) {
            reason = "gamepad axis index or value is not a valid number";
            return false;
        }
        payload = GamepadAxis{static_cast<size_t>(values[0]), static_cast<int32_t>(values[1])};
        return true;
    }
    case 'h': {
        if (count != 5) {
            reason = "dpad needs up,down,right,left,wait_ms";
            return false;
        }
        bool direction[4]{};
        for (size_t index = 0; index < 4; ++index) {
            if (!parse_flag(field[index], direction[index])) {
                reason = "each dpad direction must be 0 or 1";
                return false;
            }
        }
        payload = GamepadDpad{direction[0], direction[1], direction[2], direction[3]};
        return true;
    }
    case 'p': {
        if (count != 6) {
            reason = "pen needs in_range,tip,x,y,pressure,wait_ms";
            return false;
        }
        bool in_range = false;
        bool tip = false;
        if (!parse_flag(field[0], in_range) || !parse_flag(field[1], tip) ||
            !parse_bounded(field[2], INT32_MIN, INT32_MAX, values[2]) ||
            !parse_bounded(field[3], INT32_MIN, INT32_MAX, values[3]) ||
            !parse_bounded(field[4], INT32_MIN, INT32_MAX, values[4])) {
            reason = "pen column is not a valid number";
            return false;
        }
        if (tip && !in_range) {
            reason = "pen tip=1 requires in_range=1";
            return false;
        }
        payload = PenSample{in_range, tip, static_cast<int32_t>(values[2]),
                            static_cast<int32_t>(values[3]), static_cast<int32_t>(values[4])};
        return true;
    }
    default:
        reason = "unknown row prefix";
        return false;
    }
}

} // namespace

uint64_t batch_key(const EventPayload& payload) noexcept {
    // High byte selects the control family, low bits the instance within it.
    return std::visit(
        [](const auto& value) noexcept -> uint64_t {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, TouchEvent>)
                return (uint64_t{1} << 56) | static_cast<uint64_t>(value.finger_id);
            else if constexpr (std::is_same_v<T, MouseMove>)
                return 0U; // relative deltas accumulate; batching is lossless
            else if constexpr (std::is_same_v<T, MouseButton>)
                return (uint64_t{2} << 56) | value.button;
            else if constexpr (std::is_same_v<T, KeyEvent>)
                return (uint64_t{3} << 56) | value.usage;
            else if constexpr (std::is_same_v<T, GamepadButton>)
                return (uint64_t{4} << 56) | value.button;
            else if constexpr (std::is_same_v<T, GamepadAxis>)
                return (uint64_t{5} << 56) | value.axis_index;
            else if constexpr (std::is_same_v<T, GamepadDpad>)
                return uint64_t{6} << 56;
            else
                return uint64_t{7} << 56;
        },
        payload);
}

namespace {

// "# screen 1080x2400" -> 1080, 2400. Anything else is an ordinary comment.
bool parse_screen_directive(std::string_view comment, int32_t& width, int32_t& height) {
    comment = trim(comment);
    constexpr std::string_view keyword = "screen";
    if (comment.size() <= keyword.size())
        return false;
    for (size_t index = 0; index < keyword.size(); ++index) {
        if ((comment[index] | 0x20) != keyword[index])
            return false;
    }
    if (comment[keyword.size()] != ' ' && comment[keyword.size()] != '\t')
        return false;
    comment = trim(comment.substr(keyword.size()));
    const size_t cross = comment.find_first_of("xX");
    if (cross == std::string_view::npos)
        return false;
    int32_t w = 0;
    int32_t h = 0;
    const std::string_view left = trim(comment.substr(0, cross));
    const std::string_view right = trim(comment.substr(cross + 1));
    const auto [end_w, error_w] = std::from_chars(left.data(), left.data() + left.size(), w);
    const auto [end_h, error_h] = std::from_chars(right.data(), right.data() + right.size(), h);
    if (error_w != std::errc{} || error_h != std::errc{} || end_w != left.data() + left.size() ||
        end_h != right.data() + right.size())
        return false;
    if (w < 1 || h < 1 || w > 65536 || h > 65536)
        return false;
    width = w;
    height = h;
    return true;
}

int32_t scale_coordinate(const int32_t value, const int32_t from, const int32_t to) {
    const int64_t scaled = (static_cast<int64_t>(value) * to + from / 2) / from;
    return static_cast<int32_t>(std::clamp<int64_t>(scaled, 0, to - 1));
}

} // namespace

bool scale_script(const EventScript& script, const int32_t touch_width,
                  const int32_t touch_height, const int32_t pen_width, const int32_t pen_height,
                  EventScript& out) {
    const int32_t from_w = script.screen_width;
    const int32_t from_h = script.screen_height;
    if (from_w <= 0 || from_h <= 0)
        return false;
    const bool touch = touch_width > 0 && touch_height > 0 &&
                       (touch_width != from_w || touch_height != from_h);
    const bool pen =
        pen_width > 0 && pen_height > 0 && (pen_width != from_w || pen_height != from_h);
    if (!touch && !pen)
        return false;

    out = script;
    const auto convert = [&](std::vector<EventRecord>& rows) {
        for (EventRecord& record : rows) {
            if (auto* contact = std::get_if<TouchEvent>(&record.payload); contact && touch) {
                contact->x = scale_coordinate(contact->x, from_w, touch_width);
                contact->y = scale_coordinate(contact->y, from_h, touch_height);
            } else if (auto* sample = std::get_if<PenSample>(&record.payload); sample && pen) {
                sample->x = scale_coordinate(sample->x, from_w, pen_width);
                sample->y = scale_coordinate(sample->y, from_h, pen_height);
            }
        }
    };
    convert(out.once_rows);
    convert(out.loop_rows);
    if (touch) {
        out.screen_width = touch_width;
        out.screen_height = touch_height;
    }
    return true;
}

bool EventScript::load(const std::string& path, std::string& error) {
    once_rows.clear();
    loop_rows.clear();
    screen_width = 0;
    screen_height = 0;

    const std::string name = path_utf8(utf8_path(path).filename());
    std::ifstream input(utf8_path(path), std::ios::binary);
    if (!input) {
        error = name + ": cannot open the file";
        return false;
    }

    std::string line;
    size_t number = 0;
    bool first_line = true;
    while (std::getline(input, line)) {
        ++number;
        std::string_view view(line);
        if (first_line && view.size() >= 3 && static_cast<unsigned char>(view[0]) == 0xEF &&
            static_cast<unsigned char>(view[1]) == 0xBB &&
            static_cast<unsigned char>(view[2]) == 0xBF) {
            view.remove_prefix(3);
        }
        first_line = false;

        const size_t comment = view.find('#');
        if (comment != std::string_view::npos) {
            if (screen_width == 0 && trim(view.substr(0, comment)).empty())
                parse_screen_directive(view.substr(comment + 1), screen_width, screen_height);
            view = view.substr(0, comment);
        }
        view = trim(view);
        if (view.empty())
            continue;

        const char prefix = view.front();
        const char lowered = static_cast<char>(prefix | 0x20);
        if (lowered < 'a' || lowered > 'z') {
            error = name + ':' + std::to_string(number) +
                    ": row must start with a profile letter";
            return false;
        }
        const bool once = prefix >= 'A' && prefix <= 'Z';
        view.remove_prefix(1);
        view = trim(view);
        if (view.empty() || view.front() != ',') {
            error = name + ':' + std::to_string(number) + ": expected ',' after the row prefix";
            return false;
        }
        view.remove_prefix(1);

        std::string_view fields[6];
        const size_t count = split_fields(view, fields, 6);
        if (count > 6) {
            error = name + ':' + std::to_string(number) + ": too many columns";
            return false;
        }

        EventPayload payload{};
        std::string reason;
        if (!parse_row(lowered, fields, count, payload, reason)) {
            error = name + ':' + std::to_string(number) + ": " + reason;
            return false;
        }

        int64_t wait_ns = 0;
        if (!parse_wait_ns(fields[count - 1], wait_ns)) {
            error = name + ':' + std::to_string(number) +
                    ": wait_ms must be zero or a positive number of milliseconds";
            return false;
        }

        EventRecord record{payload, once, wait_ns};
        (once ? once_rows : loop_rows).push_back(record);
    }

    if (once_rows.empty() && loop_rows.empty()) {
        error = name + ": contains no event rows";
        return false;
    }
    return true;
}

uint32_t EventScript::required_profiles() const noexcept {
    uint32_t mask = 0;
    for (const EventRecord& record : once_rows)
        mask |= profile_bit(profile_of(record.payload));
    for (const EventRecord& record : loop_rows)
        mask |= profile_bit(profile_of(record.payload));
    return mask;
}

size_t Timeline::row_at(const Segment segment, const int64_t time_ns) const noexcept {
    const std::vector<int64_t>& list = starts(segment);
    // The trailing entry is the segment length, not a row.
    const auto last = list.end() - 1;
    return static_cast<size_t>(std::lower_bound(list.begin(), last, time_ns) - list.begin());
}

Timeline build_timeline(const EventScript& script) {
    Timeline timeline;
    const auto fill = [](const std::vector<EventRecord>& rows, std::vector<int64_t>& starts) {
        starts.clear();
        starts.reserve(rows.size() + 1);
        int64_t time = 0;
        for (const EventRecord& record : rows) {
            starts.push_back(time);
            time += record.wait_ns;
        }
        starts.push_back(time);
    };
    fill(script.once_rows, timeline.intro);
    fill(script.loop_rows, timeline.loop);
    return timeline;
}

} // namespace aoap
