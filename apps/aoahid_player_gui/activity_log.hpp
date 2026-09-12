// SPDX-License-Identifier: MIT
#pragma once

#include "aoahid_player/events.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

namespace gui {

// The GUI's EventSink: keeps the most recent messages for the activity panel
// and wakes the UI thread whenever one arrives from a worker.
class ActivityLog final : public aoap::EventSink {
  public:
    struct Entry {
        aoap::Severity severity;
        std::string time; // "HH:MM:SS"
        std::string text;
    };

    explicit ActivityLog(std::function<void()> wake) : wake_(std::move(wake)) {}

    void message(aoap::Severity severity, std::string_view text) override;

    // UI thread. `visit` runs under the log's lock.
    template <typename Visitor>
    void visit(Visitor&& visitor) const {
        const std::lock_guard lock(mutex_);
        for (const Entry& entry : entries_)
            visitor(entry);
    }
    [[nodiscard]] uint64_t version() const;
    // The newest error or warning, for the status line; empty when none.
    [[nodiscard]] Entry latest_problem() const;
    void clear();

  private:
    static constexpr size_t capacity = 400;

    std::function<void()> wake_;
    mutable std::mutex mutex_;
    std::deque<Entry> entries_;
    uint64_t version_{};
};

} // namespace gui
