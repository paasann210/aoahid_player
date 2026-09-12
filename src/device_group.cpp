// SPDX-License-Identifier: MIT
#include "aoahid_player/device_group.hpp"

#include <type_traits>
#include <utility>

namespace aoap {

Profile profile_of(const EventPayload& payload) noexcept {
    return std::visit(
        [](const auto& value) noexcept {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, TouchEvent>)
                return Profile::touch;
            else if constexpr (std::is_same_v<T, MouseMove> || std::is_same_v<T, MouseButton>)
                return Profile::mouse;
            else if constexpr (std::is_same_v<T, KeyEvent>)
                return Profile::key;
            else if constexpr (std::is_same_v<T, PenSample>)
                return Profile::pen;
            else
                return Profile::gamepad;
        },
        payload);
}

namespace {

// Applies one payload to one device. Every mutation is in-memory only and the
// caller has already checked that the profile is present.
aoahid_result mutate(const Device& device, const EventPayload& payload) {
    return std::visit(
        [&](const auto& value) -> aoahid_result {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, TouchEvent>) {
                return device.touch().touch(static_cast<uint32_t>(value.finger_id), value.state,
                                            value.x, value.y);
            } else if constexpr (std::is_same_v<T, MouseMove>) {
                return device.mouse().move(value.dx, value.dy);
            } else if constexpr (std::is_same_v<T, MouseButton>) {
                // Both the CSV column and aoahid_mouse_button are one-based.
                return device.mouse().button(value.button, value.pressed);
            } else if constexpr (std::is_same_v<T, KeyEvent>) {
                return device.key().key(value.usage, value.down);
            } else if constexpr (std::is_same_v<T, GamepadButton>) {
                return device.gamepad().button(value.button, value.pressed);
            } else if constexpr (std::is_same_v<T, GamepadAxis>) {
                return device.gamepad().axis(value.axis_index, value.value);
            } else if constexpr (std::is_same_v<T, GamepadDpad>) {
                return device.gamepad().dpad(value.up, value.down, value.right, value.left);
            } else {
                aoahid_pen_sample sample{};
                sample.in_range = value.in_range ? 1U : 0U;
                sample.tip = value.tip ? 1U : 0U;
                sample.x = value.x;
                sample.y = value.y;
                sample.pressure = value.pressure;
                return device.pen().update(sample);
            }
        },
        payload);
}

} // namespace

DeviceGroup::~DeviceGroup() { reset(); }

void DeviceGroup::add_device(Device&& device) {
    auto slot = std::make_unique<Slot>();
    slot->label = device.label();
    const uint32_t mask = device.profile_mask();
    slot->device = std::move(device);
    {
        const std::lock_guard lock(mutex_);
        const uint32_t current = profile_mask_.load(std::memory_order_relaxed);
        profile_mask_.store(slots_.empty() ? mask : (current & mask), std::memory_order_relaxed);
        slots_.push_back(std::move(slot));
    }
    active_count_.fetch_add(1, std::memory_order_relaxed);
}

void DeviceGroup::reset() noexcept {
    std::vector<std::unique_ptr<Slot>> closing;
    {
        const std::lock_guard lock(mutex_);
        closing.swap(slots_);
        profile_mask_.store(0U, std::memory_order_relaxed);
        active_count_.store(0U, std::memory_order_relaxed);
    }
    dirty_mask_ = 0U;
    rejected_rows_ = 0U;
    rejection_detail_.clear();
    // Closing drains and unregisters, which can block, so it runs without
    // the lock a UI thread may be waiting on in snapshot().
    for (const std::unique_ptr<Slot>& slot : closing)
        slot->device.close();
}

void DeviceGroup::fail(Slot& slot, const aoahid_result result) {
    bump(slot.errors);
    std::string detail = explain_error(result);
    const bool was_active = slot.active.exchange(false, std::memory_order_relaxed);
    {
        const std::lock_guard lock(mutex_);
        slot.last_error = detail;
    }
    if (!was_active)
        return;
    active_count_.fetch_sub(1, std::memory_order_relaxed);
    slot.device.close();
    if (sink_ != nullptr)
        sink_->message(Severity::warning, slot.label + " stopped responding and was dropped. " +
                                              detail);
}

aoahid_result DeviceGroup::apply(const EventPayload& payload) {
    const Profile profile = profile_of(payload);
    if ((profile_mask() & profile_bit(profile)) == 0U)
        return AOAHID_ERR_UNSUPPORTED;

    bool applied = false;
    aoahid_result outcome = AOAHID_OK;
    for (const std::unique_ptr<Slot>& slot : slots_) {
        if (!slot->active.load(std::memory_order_relaxed))
            continue;
        const aoahid_result result = mutate(slot->device, payload);
        if (result == AOAHID_OK) {
            applied = true;
            continue;
        }
        if (result == AOAHID_ERR_BUSY) {
            if (!applied) {
                // Nothing has been applied yet, so the caller can flush and
                // retry without leaving the devices in different states.
                return AOAHID_ERR_BUSY;
            }
            // Another device already took this event, so retrying would
            // apply it twice there. This one is just behind on draining its
            // previous report, not unresponsive, so drop the event for it
            // alone instead of disconnecting it.
            continue;
        }
        if (result == AOAHID_ERR_PARAM) {
            // The row is outside what the Spec declared: identical on every
            // device, so reject the row instead of dropping hardware.
            ++rejected_rows_;
            if (rejection_detail_.empty())
                rejection_detail_ = explain_error(result);
            outcome = AOAHID_ERR_PARAM;
            break;
        }
        fail(*slot, result);
    }
    if (applied)
        dirty_mask_ |= profile_bit(profile);
    return outcome;
}

aoahid_result DeviceGroup::scroll(const int32_t wheel) {
    if ((profile_mask() & profile_bit(Profile::mouse)) == 0U)
        return AOAHID_ERR_UNSUPPORTED;
    bool applied = false;
    for (const std::unique_ptr<Slot>& slot : slots_) {
        if (!slot->active.load(std::memory_order_relaxed))
            continue;
        const aoahid_result result = slot->device.mouse().scroll(wheel, 0);
        if (result == AOAHID_OK) {
            applied = true;
            continue;
        }
        if (result == AOAHID_ERR_BUSY) {
            if (!applied)
                return AOAHID_ERR_BUSY;
            // See the matching case in apply(): drop it for this device
            // alone rather than disconnecting one that is merely behind.
            continue;
        }
        fail(*slot, result);
    }
    if (applied)
        dirty_mask_ |= profile_bit(Profile::mouse);
    return AOAHID_OK;
}

bool DeviceGroup::flush() {
    if (dirty_mask_ == 0U)
        return false;
    const uint32_t flushed = dirty_mask_;
    dirty_mask_ = 0U;

    for (const std::unique_ptr<Slot>& slot : slots_) {
        if (!slot->active.load(std::memory_order_relaxed))
            continue;
        aoahid_result failure = AOAHID_OK;
        for (size_t profile = 0; profile < profile_count; ++profile) {
            if ((flushed & (uint32_t{1} << profile)) == 0U)
                continue;
            aoahid_node* node = slot->device.node(static_cast<Profile>(profile));
            if (node == nullptr)
                continue;
            const aoahid_result result = aoahid_node_submit(node);
            if (result != AOAHID_OK) {
                failure = result;
                break;
            }
        }
        if (failure != AOAHID_OK)
            fail(*slot, failure);
    }

    for (const std::unique_ptr<Slot>& slot : slots_) {
        if (!slot->active.load(std::memory_order_relaxed))
            continue;
        aoahid_result failure = AOAHID_OK;
        for (size_t profile = 0; profile < profile_count; ++profile) {
            if ((flushed & (uint32_t{1} << profile)) == 0U)
                continue;
            aoahid_node* node = slot->device.node(static_cast<Profile>(profile));
            if (node == nullptr)
                continue;
            // Drains the report queued above, including any continuation
            // fragment the profile still owes.
            const aoahid_result result = aoahid_node_submit_blocking(node, drain_deadline_ms);
            if (result != AOAHID_OK) {
                failure = result;
                break;
            }
        }
        if (failure != AOAHID_OK) {
            fail(*slot, failure);
            continue;
        }
        bump(slot->reports);
    }
    return true;
}

std::vector<DeviceStatus> DeviceGroup::snapshot() const {
    std::vector<DeviceStatus> list;
    const std::lock_guard lock(mutex_);
    list.reserve(slots_.size());
    for (const std::unique_ptr<Slot>& slot : slots_) {
        DeviceStatus status;
        status.label = slot->label;
        status.reports = slot->reports.load(std::memory_order_relaxed);
        status.errors = slot->errors.load(std::memory_order_relaxed);
        status.active = slot->active.load(std::memory_order_relaxed);
        status.last_error = slot->last_error;
        list.push_back(std::move(status));
    }
    return list;
}

} // namespace aoap
