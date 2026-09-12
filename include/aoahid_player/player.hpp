// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "device_group.hpp"
#include "event_script.hpp"
#include "events.hpp"
#include "input_state.hpp"
#include "timing.hpp"

namespace aoap {

enum class PlaybackState : uint8_t { stopped, playing, paused };

// A point on the script timeline, in script time (speed 1.0).
struct PlaybackPosition {
    Segment segment{Segment::intro};
    int64_t time_ns{};
};

struct PlaybackStatus {
    PlaybackState state{PlaybackState::stopped};
    PlaybackPosition position;
    uint64_t loops{}; // completed loop iterations
    uint64_t reports{};
};

// Told about every row the player sends (script rows and the releases of
// stop, pause, and seek) on the playback thread. Must be quick and must not
// block; the live preview uses it.
class PlaybackObserver {
  public:
    virtual ~PlaybackObserver() = default;
    virtual void sent(const EventPayload& payload) noexcept = 0;
};

// Drives an EventScript against a DeviceGroup.
//   - The intro (uppercase rows) runs before loop iteration 1; the loop rows
//     then repeat until the loop limit, stop(), or the loss of every device.
//   - Every row runs at an absolute deadline measured from one anchor, so a
//     slow send never makes the script drift.
//   - Rows with wait_ms 0 are batched into the next report. A row that would
//     overwrite an unreported edge of the same control flushes first, since
//     libaoahid rejects that overwrite and dropping a press would change
//     what the script means.
//   - Pause, seek, speed, loop limit, and the live offset can be changed from
//     any thread at any time. They wake the playback thread at once; nothing
//     polls.
//   - Whatever the player pressed is released on stop and pause. A seek
//     rebuilds the input state at the target (see InputState), so held keys
//     and contacts are exactly what uninterrupted playback would have left.
//
// run() and load() belong to the thread that owns the DeviceGroup.
class Player {
  public:
    explicit Player(DeviceGroup& group, EventSink* sink = nullptr);

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;
    Player(Player&&) = delete;
    Player& operator=(Player&&) = delete;

    // Only while stopped.
    void load(std::shared_ptr<const EventScript> script);
    void set_observer(PlaybackObserver* observer) noexcept { observer_ = observer; }
    [[nodiscard]] bool loaded() const noexcept { return script_ != nullptr; }

    // Plays from `start` and returns once playback has stopped and every
    // control it pressed has been released.
    void run(PlaybackPosition start = {});

    // Controls, callable from any thread. stop() is also async-signal-safe.
    void stop() noexcept;
    void pause() noexcept;
    void resume() noexcept;
    // Moves playback (or the paused position) to `target`. No effect while
    // stopped; pass the start position to run() instead.
    void seek(PlaybackPosition target) noexcept;
    void set_speed(double speed) noexcept; // 2.0 plays twice as fast
    void set_loop_limit(int64_t loops) noexcept; // 0 = repeat until stopped
    // Shifts every later deadline; positive delays. Returns the new total.
    int64_t adjust_offset_ns(int64_t delta) noexcept;
    void set_offset_ns(int64_t offset) noexcept;
    // Stops playback once Timing::now_ns() reaches `stop_at_ns`, playing or
    // paused; 0 removes the limit. Any thread.
    void set_stop_at(int64_t stop_at_ns) noexcept;
    // True when the last run() ended because of set_stop_at().
    [[nodiscard]] bool stopped_by_time() const noexcept {
        return timed_out_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] double speed() const noexcept { return speed_.load(std::memory_order_relaxed); }
    [[nodiscard]] int64_t loop_limit() const noexcept {
        return loop_limit_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int64_t offset_ns() const noexcept {
        return offset_ns_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] PlaybackStatus status() const noexcept;

    static constexpr double min_speed = 0.01;
    static constexpr double max_speed = 1000.0;

  private:
    enum Request : uint32_t {
        request_stop = 1U << 0,
        request_pause = 1U << 1, // want_paused_ changed
        request_seek = 1U << 2,
        request_retime = 1U << 3, // speed changed
    };
    enum class Outcome { proceed, jumped, stop };

    void post(uint32_t request) noexcept;
    Outcome service();
    Outcome hold_paused();
    Outcome wait_to(int64_t script_time);
    bool finish_segment();

    void execute(const EventRecord& record);
    aoahid_result send(const EventPayload& payload);
    void flush_now();
    void settle(const InputState& target);
    void jump(PlaybackPosition target);
    void retime(int64_t now);

    [[nodiscard]] int64_t deadline_for(int64_t script_time) const noexcept;
    [[nodiscard]] int64_t script_time_at(int64_t now) const noexcept;
    [[nodiscard]] PlaybackPosition clamp(PlaybackPosition position) const noexcept;
    [[nodiscard]] bool conflicts(uint64_t key) const noexcept;
    void report_skip(aoahid_result result, const EventPayload& payload);
    void publish(PlaybackState state, PlaybackPosition position) noexcept;

    DeviceGroup& group_;
    EventSink* sink_;
    PlaybackObserver* observer_{};

    // Playback thread only.
    std::shared_ptr<const EventScript> script_;
    Timeline timeline_;
    InputState live_;   // what the devices currently hold
    InputState target_; // scratch for seeks
    std::vector<EventPayload> releases_;
    std::vector<EventPayload> presses_;
    std::vector<uint64_t> staged_keys_;
    Segment segment_{Segment::intro};
    size_t index_{};
    int64_t cursor_{}; // script time of the batch being staged
    uint64_t loops_{};
    int64_t anchor_real_{};
    int64_t anchor_script_{};
    int64_t offset_anchor_{};
    double anchor_speed_{1.0};
    bool paused_{};
    PlaybackPosition pause_position_{};
    uint32_t warned_profiles_{};
    bool rejection_reported_{};

    // Shared with the control threads.
    WakeSignal wake_;
    std::atomic<uint32_t> requests_{};
    std::atomic<bool> want_paused_{};
    std::atomic<uint64_t> seek_word_{};
    std::atomic<double> speed_{1.0};
    std::atomic<int64_t> offset_ns_{};
    std::atomic<int64_t> loop_limit_{};
    std::atomic<uint64_t> reports_{};
    std::atomic<int64_t> stop_at_ns_{};
    std::atomic<bool> timed_out_{};

    // Seqlock-published timeline anchor, so status() can interpolate the
    // position without the playback thread writing anything per row.
    std::atomic<uint32_t> seq_{};
    std::atomic<uint8_t> pub_state_{};
    std::atomic<uint8_t> pub_segment_{};
    std::atomic<int64_t> pub_anchor_real_{};
    std::atomic<int64_t> pub_anchor_script_{};
    std::atomic<int64_t> pub_offset_anchor_{};
    std::atomic<int64_t> pub_duration_{};
    std::atomic<double> pub_speed_{1.0};
    std::atomic<uint64_t> pub_loops_{};
};

} // namespace aoap
