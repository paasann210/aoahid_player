// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "aoahid_player/event_script.hpp"

namespace aoap::record {

// Parses one line of `adb shell getevent -l` raw output (format:
// "[timestamp] /dev/input/eventN: EV_ABS ABS_MT_POSITION_X 000001a4" etc.)
// and accumulates it into an in-progress touch/key event. getevent reports
// one axis/key per line and an EV_SYN SYN_REPORT line marks a completed
// frame; only completed frames become an EventRecord (always lowercase /
// `once = false`, since a recording has no "first loop" concept).
//
// A frame usually closes several contacts at once, so completed rows are
// appended to a caller-owned vector rather than returned one at a time. The
// last row of a frame carries the wait to the next frame, which is only known
// once that next frame arrives; feed() therefore emits the previous frame,
// not the one it just closed.
//
// Multi-touch is tracked with the Type B protocol (ABS_MT_SLOT plus
// ABS_MT_TRACKING_ID). Relative mouse motion (EV_REL) is a known gap; see
// README.md's limitations.
class GeteventParser {
  public:
    // Records only this /dev/input/eventN when set; every device otherwise.
    void set_device_filter(std::string path) { filter_ = std::move(path); }

    // `host_now_ns` is used when the line carries no getevent timestamp,
    // which happens when getevent was started without -t.
    void feed(std::string_view line, int64_t host_now_ns, std::vector<EventRecord>& out);

    // Emits the frame still held back, with a zero trailing wait.
    void finish(std::vector<EventRecord>& out);

    [[nodiscard]] uint64_t unmapped_keys() const noexcept { return unmapped_keys_; }
    [[nodiscard]] const std::string& first_unmapped_key() const noexcept {
        return first_unmapped_key_;
    }

    // Exposed for testing: "KEY_A" or a raw Linux keycode to a HID Usage.
    static bool key_usage(std::string_view token, uint16_t& usage) noexcept;

  private:
    static constexpr size_t max_slots = 16;

    struct Slot {
        int32_t x{};
        int32_t y{};
        bool active{};
        bool changed{};
        bool lifting{};
        bool placed{};
    };

    struct DeviceState {
        size_t slot{};
        std::array<Slot, max_slots> slots{};
        bool frame_dirty{};
    };

    DeviceState& state_for(const std::string& device);
    void close_frame(DeviceState& state, int64_t timestamp_ns, std::vector<EventRecord>& out);
    void stage(const EventPayload& payload);
    void release_pending(int64_t timestamp_ns, std::vector<EventRecord>& out);

    std::unordered_map<std::string, DeviceState> devices_;
    std::vector<EventRecord> frame_rows_;
    std::vector<EventRecord> pending_;
    int64_t pending_time_ns_{-1};
    std::string filter_;
    uint64_t unmapped_keys_{};
    std::string first_unmapped_key_;
};

// Reads `adb shell getevent -lp` output and returns the coordinate space of
// the first device reporting both ABS_MT_POSITION_X and _Y (or of `device`
// when it is not empty) as max + 1. Recordings keep the panel's raw values,
// which are not always screen pixels, so this is what they are scaled from.
// False when no device qualifies or an axis does not start at zero.
bool parse_touch_range(std::string_view text, std::string_view device, int32_t& width,
                       int32_t& height);

} // namespace aoap::record
