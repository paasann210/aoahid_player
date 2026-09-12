// SPDX-License-Identifier: MIT
#pragma once

#include <aoahid.h>

#include <string>

namespace aoap {

// Formats the calling thread's last libaoahid diagnostic. The returned string
// owns its bytes, because aoahid_result_name() is itself a public call and
// would replace the borrowed field/reason pointers.
std::string describe_error(aoahid_result result);

// A plain-language sentence for `result`, followed by describe_error() in
// parentheses for diagnosis. Call it right after the failing libaoahid call.
std::string explain_error(aoahid_result result);

// RAII wrapper around aoahid_context. Always created with
// AOAHID_EVENT_INTERNAL_THREAD: this project hangs multiple Devices off one
// Context and lets libaoahid's own event thread drive completion, instead of
// the application running its own poll loop across several devices.
//
// libaoahid still requires the application to serialize every call that
// touches one Context, so all state mutation and submission happens on the
// single playback thread; see libaoahid's docs/API.md "Threading domains".
class Context {
  public:
    Context() noexcept = default;
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&& other) noexcept;
    Context& operator=(Context&& other) noexcept;

    aoahid_result create() noexcept;
    void destroy() noexcept;

    [[nodiscard]] aoahid_context* native_handle() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

  private:
    static constexpr uint32_t destroy_timeout_ms = 3000U;

    aoahid_context* handle_{};
};

} // namespace aoap
