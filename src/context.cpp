// SPDX-License-Identifier: MIT
#include "aoahid_player/context.hpp"

#include <string_view>
#include <utility>

namespace aoap {

std::string describe_error(const aoahid_result result) {
    const aoahid_error_detail* detail = aoahid_last_error();
    // Copy both borrowed strings before aoahid_result_name() replaces them.
    const std::string field =
        detail != nullptr && detail->field != nullptr ? detail->field : "no field";
    const std::string reason =
        detail != nullptr && detail->reason != nullptr ? detail->reason : "no detail";
    std::string text = aoahid_result_name(result);
    text += " (";
    text += field;
    text += ": ";
    text += reason;
    text += ')';
    return text;
}

namespace {

const char* plain_reason(const aoahid_result result) noexcept {
    switch (result) {
    case AOAHID_OK:
        return "No error";
    case AOAHID_ERR_PARAM:
        return "A value was rejected as invalid";
    case AOAHID_ERR_UNSET_FIELD:
        return "A required setting is missing";
    case AOAHID_ERR_UNSUPPORTED:
        return "The device does not support this";
    case AOAHID_ERR_NOT_AOA:
        return "The device does not support Android Open Accessory (AOA)";
    case AOAHID_ERR_VERSION:
        return "The device reports an AOA protocol version this program cannot use";
    case AOAHID_ERR_ACCESS:
        return "Access to the USB device was denied. On Linux, install udev/51-aoahid.rules "
               "and replug the device; on Windows, the device needs a driver libusb can open "
               "(such as WinUSB)";
    case AOAHID_ERR_BUSY:
        return "The device is busy or held by another program, such as the adb server";
    case AOAHID_ERR_NO_DEVICE:
        return "The device is no longer connected";
    case AOAHID_ERR_STALL:
        return "The device refused the request";
    case AOAHID_ERR_TIMEOUT:
        return "The device did not respond in time";
    case AOAHID_ERR_SHORT_TRANSFER:
        return "A USB transfer was cut short";
    case AOAHID_ERR_DESCRIPTOR_REJECTED:
        return "Android rejected the HID description of this profile";
    case AOAHID_ERR_IO:
        return "USB communication failed; check the cable and the port";
    case AOAHID_ERR_OVERFLOW:
        return "A limit was exceeded";
    case AOAHID_CLOSE_PENDING:
        return "The device is still closing";
    case AOAHID_ERR_INTERNAL:
    default:
        return "Internal error in libaoahid";
    }
}

} // namespace

std::string explain_error(const aoahid_result result) {
    // describe_error() reads the thread's last diagnostic, so it goes first.
    const std::string detail = describe_error(result);
    std::string text = plain_reason(result);
    text += " (";
    text += detail;
    text += ')';
    return text;
}

Context::~Context() { destroy(); }

Context::Context(Context&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

Context& Context::operator=(Context&& other) noexcept {
    if (this != &other) {
        destroy();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

aoahid_result Context::create() noexcept {
    destroy();
    aoahid_context_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    options.event_mode = AOAHID_EVENT_INTERNAL_THREAD;
    options.log_level = AOAHID_LOG_DISABLED;
    options.log_sink = nullptr;
    options.log_user = nullptr;
    return aoahid_context_create(&options, &handle_);
}

void Context::destroy() noexcept {
    if (handle_ == nullptr)
        return;
    // Only a deadline-expired AOAHID_ERR_TIMEOUT retains ownership; every
    // other result consumes the Context and invalidates the pointer.
    aoahid_result result = aoahid_context_destroy_blocking(handle_, destroy_timeout_ms);
    if (result == AOAHID_ERR_TIMEOUT) {
        const aoahid_error_detail* detail = aoahid_last_error();
        const bool retained = detail != nullptr && detail->field != nullptr &&
                              std::string_view(detail->field) == "context.destroy";
        if (retained)
            result = aoahid_context_destroy_blocking(handle_, destroy_timeout_ms);
    }
    static_cast<void>(result);
    handle_ = nullptr;
}

} // namespace aoap
