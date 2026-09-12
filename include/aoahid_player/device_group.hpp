// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "device.hpp"
#include "event_script.hpp"
#include "events.hpp"

namespace aoap {

// Which profile a script row drives.
Profile profile_of(const EventPayload& payload) noexcept;

// Point-in-time copy of one device's state, safe to take from any thread.
struct DeviceStatus {
    std::string label;
    uint64_t reports{};
    uint64_t errors{};
    bool active{};
    std::string last_error;
};

// Holds every connected Device and broadcasts one script event to all of
// them at once. Per device:
//   1. mutate in-memory state (aoahid_touch / aoahid_kbd / ... - cheap)
//   2. submit (non-blocking)
// Every device is submitted before any completion is awaited, so the total
// wait is bounded by the slowest device rather than the sum of all of them.
// The submissions stay on one thread because libaoahid requires the caller
// to serialize every call touching one Context.
//
// A device that fails (unplugged, timed out, ...) is closed and skipped from
// then on instead of aborting playback; the sink is told once, in plain words.
//
// Threading: add_device / reset / apply / flush belong to the one thread that
// owns the Context. snapshot() and the counters may be read from any thread.
class DeviceGroup {
  public:
    // Failure deadline for one report's completion drain. It bounds a stuck
    // transfer; it is not a delay added to a successful send.
    static constexpr uint32_t drain_deadline_ms = 500U;

    explicit DeviceGroup(EventSink* sink = nullptr) noexcept : sink_(sink) {}
    ~DeviceGroup();

    DeviceGroup(const DeviceGroup&) = delete;
    DeviceGroup& operator=(const DeviceGroup&) = delete;
    DeviceGroup(DeviceGroup&&) = delete;
    DeviceGroup& operator=(DeviceGroup&&) = delete;

    void add_device(Device&& device);

    // Closes and forgets every device.
    void reset() noexcept;

    [[nodiscard]] bool empty() const noexcept { return slots_.empty(); }
    [[nodiscard]] size_t size() const noexcept { return slots_.size(); }
    [[nodiscard]] size_t active_count() const noexcept {
        return active_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint32_t profile_mask() const noexcept {
        return profile_mask_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool dirty() const noexcept { return dirty_mask_ != 0U; }

    // Applies one script row to every active device.
    //   AOAHID_ERR_BUSY        the row would overwrite an edge that has not
    //                          reached a report yet and nothing was changed;
    //                          the caller flushes and retries.
    //   AOAHID_ERR_UNSUPPORTED no device has a Node for that profile.
    //   AOAHID_ERR_PARAM       the row is outside the declared Spec. That is a
    //                          script or settings problem, so the row is
    //                          counted and the devices are kept.
    // Any other failure drops just that device and is not reported here.
    aoahid_result apply(const EventPayload& payload);

    // Adds wheel detents on every active mouse; results as for apply().
    aoahid_result scroll(int32_t wheel);

    [[nodiscard]] uint64_t rejected_rows() const noexcept { return rejected_rows_; }
    [[nodiscard]] const std::string& rejection_detail() const noexcept {
        return rejection_detail_;
    }

    // Submits every dirty node on every active device, then drains them.
    // Returns true when anything was sent.
    bool flush();

    [[nodiscard]] std::vector<DeviceStatus> snapshot() const;

  private:
    struct Slot {
        Device device;
        std::string label;
        std::string last_error; // guarded by mutex_
        std::atomic<uint64_t> reports{};
        std::atomic<uint64_t> errors{};
        std::atomic<bool> active{true};
    };

    // Counters have one writer, so a plain load/store pair avoids a locked
    // read-modify-write on the send path.
    static void bump(std::atomic<uint64_t>& counter) noexcept {
        counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    void fail(Slot& slot, aoahid_result result);

    std::vector<std::unique_ptr<Slot>> slots_;
    mutable std::mutex mutex_; // guards slots_ layout and last_error for snapshot()
    EventSink* sink_;
    std::atomic<size_t> active_count_{};
    std::atomic<uint32_t> profile_mask_{};
    uint32_t dirty_mask_{};
    uint64_t rejected_rows_{};
    std::string rejection_detail_;
};

} // namespace aoap
