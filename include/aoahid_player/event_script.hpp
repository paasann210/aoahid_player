// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <string>
#include <variant>
#include <vector>

// This header is deliberately free of <aoahid.h>: aoa_record shares the CSV
// row format with aoa_touch but never links libaoahid.

namespace aoap {

// One parsed CSV line. `once` is true for an uppercase prefix (runs only on
// the first loop iteration); false for lowercase (runs every iteration).
// See README.md's CSV format table for the exact column layout per prefix.
struct TouchEvent   { int finger_id; bool state; int32_t x, y; };
struct MouseMove    { int32_t dx, dy; };
struct MouseButton  { uint32_t button; bool pressed; };
struct KeyEvent     { uint16_t usage; bool down; };
struct GamepadButton{ uint32_t button; bool pressed; };
struct GamepadAxis  { size_t axis_index; int32_t value; };
struct GamepadDpad  { bool up, down, right, left; };
struct PenSample    { bool in_range, tip; int32_t x, y, pressure; };

using EventPayload = std::variant<TouchEvent, MouseMove, MouseButton, KeyEvent,
                                   GamepadButton, GamepadAxis, GamepadDpad, PenSample>;

struct EventRecord {
    EventPayload payload;
    bool once;          // true = uppercase prefix, first loop iteration only
    int64_t wait_ns;    // 0 = batch with next row, no flush/wait
};

// Identifies what a row touches, so the player can tell a batchable row (two
// fingers in one frame) from one that must flush first (press then release of
// the same control, which libaoahid rejects as an unreported opposite edge).
// Zero means "accumulates, never conflicts".
uint64_t batch_key(const EventPayload& payload) noexcept;

// Parses a single CSV file into once-only rows and every-loop rows.
class EventScript {
  public:
    std::vector<EventRecord> once_rows;
    std::vector<EventRecord> loop_rows;

    // The coordinate space the touch and pen rows were written in, from a
    // "# screen WxH" comment line (aoa_record writes one); 0 when the file
    // does not say, in which case coordinates are used as they are.
    int32_t screen_width{};
    int32_t screen_height{};

    // `path` is UTF-8. Returns false and fills `error` with
    // "<file>:<line>: <reason>" on the first malformed row. A '#' comment, a
    // blank line, and a UTF-8 BOM are skipped; nothing else is dropped.
    bool load(const std::string& path, std::string& error);

    [[nodiscard]] size_t size() const noexcept { return once_rows.size() + loop_rows.size(); }

    // Union of profile_bit() values the script needs; declared as a plain mask
    // so this header stays independent of device.hpp.
    [[nodiscard]] uint32_t required_profiles() const noexcept;
};

// Maps a script written for its screen_width x screen_height space onto the
// connected surfaces: touch rows onto touch_width x touch_height, pen rows
// onto pen_width x pen_height (0 leaves that profile alone). Returns false,
// leaving `out` untouched, when the script declares no space or every size
// already matches. Scaling happens once here, never per row while playing.
bool scale_script(const EventScript& script, int32_t touch_width, int32_t touch_height,
                  int32_t pen_width, int32_t pen_height, EventScript& out);

// The once-only rows run as an intro before the first loop iteration; the
// loop rows repeat.
enum class Segment : uint8_t { intro, loop };

// Script-time start of every row at speed 1.0, per segment. Each vector has
// one entry past the last row, which holds the segment's length.
struct Timeline {
    std::vector<int64_t> intro{0};
    std::vector<int64_t> loop{0};

    [[nodiscard]] const std::vector<int64_t>& starts(const Segment segment) const noexcept {
        return segment == Segment::intro ? intro : loop;
    }
    [[nodiscard]] size_t rows(const Segment segment) const noexcept {
        return starts(segment).size() - 1;
    }
    [[nodiscard]] int64_t duration(const Segment segment) const noexcept {
        return starts(segment).back();
    }
    // First row that starts at or after `time_ns`; rows() when none does.
    [[nodiscard]] size_t row_at(Segment segment, int64_t time_ns) const noexcept;
};

Timeline build_timeline(const EventScript& script);

// --- Row formatting, shared with the recorder ----------------------------

inline void append_touch_row(std::string& out, const int finger_id, const bool state,
                             const int32_t x, const int32_t y, const double wait_ms) {
    char line[96];
    const int written = std::snprintf(line, sizeof line, "t,%d,%d,%d,%d,%.3f\n", finger_id,
                                      state ? 1 : 0, x, y, wait_ms);
    if (written > 0)
        out.append(line, static_cast<size_t>(written));
}

inline void append_key_row(std::string& out, const uint16_t usage, const bool down,
                           const double wait_ms) {
    char line[64];
    const int written = std::snprintf(line, sizeof line, "k,0x%02x,%d,%.3f\n",
                                      static_cast<unsigned>(usage), down ? 1 : 0, wait_ms);
    if (written > 0)
        out.append(line, static_cast<size_t>(written));
}

} // namespace aoap
