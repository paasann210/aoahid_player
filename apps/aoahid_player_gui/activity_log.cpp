// SPDX-License-Identifier: MIT
#include "activity_log.hpp"

#include <ctime>

namespace gui {

void ActivityLog::message(const aoap::Severity severity, const std::string_view text) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[16];
    std::strftime(stamp, sizeof stamp, "%H:%M:%S", &local);
    {
        const std::lock_guard lock(mutex_);
        entries_.push_back({severity, stamp, std::string(text)});
        if (entries_.size() > capacity)
            entries_.pop_front();
        ++version_;
    }
    if (wake_)
        wake_();
}

uint64_t ActivityLog::version() const {
    const std::lock_guard lock(mutex_);
    return version_;
}

ActivityLog::Entry ActivityLog::latest_problem() const {
    const std::lock_guard lock(mutex_);
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->severity != aoap::Severity::info)
            return *it;
    }
    return {aoap::Severity::info, {}, {}};
}

void ActivityLog::clear() {
    const std::lock_guard lock(mutex_);
    entries_.clear();
    ++version_;
}

} // namespace gui
