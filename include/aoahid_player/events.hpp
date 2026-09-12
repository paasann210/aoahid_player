// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string_view>

namespace aoap {

enum class Severity : uint8_t { info, warning, error };

// Receives human-readable status from the core instead of the core printing
// it. Calls can arrive on the engine, playback, or recorder thread, so an
// implementation must be thread-safe. None of them sit on a per-report path.
class EventSink {
  public:
    virtual ~EventSink() = default;

    virtual void message(Severity severity, std::string_view text) = 0;

    // After every completed loop iteration.
    virtual void loop_completed(uint64_t /*loops*/, uint64_t /*reports*/) {}

    // Each CSV row a recording writes, including its newline.
    virtual void recorded_row(std::string_view /*row*/) {}
};

} // namespace aoap
