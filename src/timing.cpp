// SPDX-License-Identifier: MIT
#include "aoahid_player/timing.hpp"

#include <atomic>
#include <climits>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <ctime>
#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#endif

namespace aoap {
namespace {

constexpr int64_t ns_per_sec = 1'000'000'000;

static_assert(std::atomic_ref<uint32_t>::is_always_lock_free);

std::atomic_ref<uint32_t> counter(uint32_t& word) noexcept {
    return std::atomic_ref<uint32_t>(word);
}

#if defined(_WIN32)

// QueryPerformanceFrequency is fixed at boot, so it is read once.
int64_t performance_frequency() noexcept {
    static const int64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart > 0 ? static_cast<int64_t>(value.QuadPart) : ns_per_sec;
    }();
    return frequency;
}

// One high-resolution waitable timer per thread. Creating it once keeps the
// wait path free of handle churn, and the HIGH_RESOLUTION flag gives sub-
// millisecond granularity without touching the global system timer period.
HANDLE thread_timer() noexcept {
    static thread_local HANDLE timer = [] {
        HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr,
                                               CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_ALL_ACCESS);
        if (handle == nullptr) {
            // Pre-1803 kernels reject the flag; fall back to an ordinary timer.
            handle = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
        return handle;
    }();
    return timer;
}

#elif defined(__linux__)

// FUTEX_WAIT_BITSET takes an absolute CLOCK_MONOTONIC deadline, the same
// clock and hrtimer path clock_nanosleep(TIMER_ABSTIME) uses.
long futex_wait(uint32_t* word, const uint32_t expected, const timespec* deadline) noexcept {
    return syscall(SYS_futex, word, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG, expected, deadline,
                   nullptr, FUTEX_BITSET_MATCH_ANY);
}

void futex_wake(uint32_t* word) noexcept {
    syscall(SYS_futex, word, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, INT_MAX, nullptr, nullptr, 0);
}

#endif

#if !defined(_WIN32)
timespec to_timespec(const int64_t ns) noexcept {
    timespec value{};
    value.tv_sec = static_cast<time_t>(ns / ns_per_sec);
    value.tv_nsec = static_cast<long>(ns % ns_per_sec);
    return value;
}
#endif

} // namespace

WakeSignal::WakeSignal() noexcept {
#if defined(_WIN32)
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
#endif
}

WakeSignal::~WakeSignal() {
#if defined(_WIN32)
    if (event_ != nullptr)
        CloseHandle(static_cast<HANDLE>(event_));
#endif
}

uint32_t WakeSignal::epoch() const noexcept {
    return counter(word_).load(std::memory_order_acquire);
}

void WakeSignal::notify() noexcept {
    counter(word_).fetch_add(1, std::memory_order_acq_rel);
#if defined(_WIN32)
    if (event_ != nullptr)
        SetEvent(static_cast<HANDLE>(event_));
#elif defined(__linux__)
    futex_wake(&word_);
#endif
}

void WakeSignal::wait(const uint32_t seen) const noexcept {
    while (epoch() == seen) {
#if defined(_WIN32)
        if (event_ == nullptr || WaitForSingleObject(static_cast<HANDLE>(event_), INFINITE) !=
                                     WAIT_OBJECT_0)
            Sleep(10);
#elif defined(__linux__)
        futex_wait(&word_, seen, nullptr);
#else
        Timing::sleep_ms(10);
#endif
    }
}

int64_t Timing::now_ns() noexcept {
#if defined(_WIN32)
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    const int64_t frequency = performance_frequency();
    const int64_t ticks = value.QuadPart;
    // Split the division so a long uptime cannot overflow ticks * 1e9.
    return (ticks / frequency) * ns_per_sec + ((ticks % frequency) * ns_per_sec) / frequency;
#else
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * ns_per_sec + static_cast<int64_t>(ts.tv_nsec);
#endif
}

bool Timing::wait_until(const int64_t deadline_ns, const WakeSignal* wake,
                        const uint32_t seen) noexcept {
    const auto woken = [wake, seen]() noexcept { return wake != nullptr && wake->epoch() != seen; };
    if (woken())
        return false;

    const int64_t coarse_until = deadline_ns - spin_threshold_ns;

#if defined(_WIN32)
    HANDLE timer = thread_timer();
    HANDLE event = wake != nullptr ? static_cast<HANDLE>(wake->event_) : nullptr;
    while (true) {
        const int64_t remaining = coarse_until - now_ns();
        if (remaining <= 0)
            break;
        if (timer != nullptr) {
            // A negative due time is relative, in 100 ns units.
            LARGE_INTEGER due{};
            due.QuadPart = -(remaining / 100);
            if (due.QuadPart == 0)
                break;
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                const HANDLE handles[2] = {timer, event};
                const DWORD count = event != nullptr ? 2 : 1;
                const DWORD result = WaitForMultipleObjects(count, handles, FALSE, INFINITE);
                if (result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1) {
                    if (woken())
                        return false;
                    continue;
                }
            }
            timer = nullptr; // fall through to the plain wait for the rest of this call
        }
        // Without a waitable timer, a coarse wait still beats spinning for
        // milliseconds; the spin tail below restores the accuracy.
        const int64_t coarse_ms = (remaining - spin_threshold_ns) / 1'000'000;
        if (coarse_ms < 1)
            break;
        if (event != nullptr)
            WaitForSingleObject(event, static_cast<DWORD>(coarse_ms));
        else
            Sleep(static_cast<DWORD>(coarse_ms));
        if (woken())
            return false;
    }
#else
    while (true) {
        if (coarse_until <= now_ns())
            break;
        const timespec target = to_timespec(coarse_until);
#if defined(__linux__)
        if (wake != nullptr) {
            const long result = futex_wait(&wake->word_, seen, &target);
            if (woken())
                return false;
            if (result != 0 && errno == ETIMEDOUT)
                break;
            continue; // EINTR or a spurious wakeup
        }
#endif
        const int result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, nullptr);
        if (woken())
            return false;
        if (result != 0 && result != EINTR)
            break;
    }
#endif

    // Busy-spin the final spin_threshold_ns for sub-millisecond accuracy.
    if (wake != nullptr) {
        while (now_ns() < deadline_ns) {
            if (woken())
                return false;
        }
    } else {
        while (now_ns() < deadline_ns)
            ;
    }
    return true;
}

void Timing::sleep_ms(const int32_t milliseconds) noexcept {
    if (milliseconds <= 0)
        return;
#if defined(_WIN32)
    Sleep(static_cast<DWORD>(milliseconds));
#else
    timespec ts{};
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = static_cast<long>(milliseconds % 1000) * 1'000'000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
#endif
}

} // namespace aoap
