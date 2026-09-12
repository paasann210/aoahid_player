// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace aoap {

// A counter one thread sleeps on and others bump to wake it immediately, so
// a control change never waits out a scripted delay and an idle wait costs
// no periodic wakeups.
//
// Linux: a private futex on the counter itself. Windows: an auto-reset event
// waited on together with the high-resolution timer.
class WakeSignal {
  public:
    WakeSignal() noexcept;
    ~WakeSignal();

    WakeSignal(const WakeSignal&) = delete;
    WakeSignal& operator=(const WakeSignal&) = delete;
    WakeSignal(WakeSignal&&) = delete;
    WakeSignal& operator=(WakeSignal&&) = delete;

    [[nodiscard]] uint32_t epoch() const noexcept;

    // Async-signal-safe on Linux and safe from a Windows console handler.
    void notify() noexcept;

    // Blocks until epoch() differs from `seen`.
    void wait(uint32_t seen) const noexcept;

  private:
    friend class Timing;

    alignas(4) mutable uint32_t word_{}; // only touched through std::atomic_ref
#if defined(_WIN32)
    void* event_{};
#endif
};

// Absolute-deadline high-precision wait, ported from the original
// aoa_touch.c model: sleep until shortly before the deadline, then
// busy-wait the remainder for sub-millisecond accuracy.
//
// Linux: an absolute CLOCK_MONOTONIC futex wait (or clock_nanosleep without
// a WakeSignal) for the coarse part. Windows: CreateWaitableTimerExW with
// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+), which avoids
// changing the global timer period through timeBeginPeriod.
class Timing {
  public:
    // Sleep until this many nanoseconds before the deadline, then spin.
    static constexpr int64_t spin_threshold_ns = 100'000; // 100 us

    static int64_t now_ns() noexcept;

    // Returns true once `deadline_ns` is reached, or false as soon as
    // `wake->epoch()` differs from `seen`.
    static bool wait_until(int64_t deadline_ns, const WakeSignal* wake = nullptr,
                           uint32_t seen = 0) noexcept;

    // Cold-path helper for setup code; never used inside the playback loop.
    static void sleep_ms(int32_t milliseconds) noexcept;
};

} // namespace aoap
