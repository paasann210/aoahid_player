// SPDX-License-Identifier: MIT
#include "aoahid_player/device.hpp"

#include <cstdio>
#include <utility>
#include <vector>

namespace aoap {
namespace {

// Every tuning value below is this application's policy, not a libaoahid
// default. Zero-valued fields would select the fallbacks documented in
// libaoahid's docs/API.md; they are stated explicitly so the descriptor and
// transport budgets are visible in one place.
constexpr uint32_t control_timeout_ms = 500U;
constexpr uint32_t send_timeout_ms = 500U;
constexpr uint32_t descriptor_policy_bytes = 4096U;
constexpr uint32_t report_policy_bytes = 1024U;
constexpr uint32_t pool_slots = 8U;
constexpr uint32_t close_drain_timeout_ms = 1000U;
constexpr uint32_t first_report_attempts = 20U;
constexpr uint32_t first_report_backoff_us = 1000U;

aoahid_device_options make_device_options() noexcept {
    aoahid_device_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    options.startup_mode = AOAHID_START_CURRENT_USB_MODE;
    options.accept_future_protocol_versions = 0U;
    options.control_timeout_ms = control_timeout_ms;
    options.send_timeout_ms = send_timeout_ms;
    options.descriptor_fragment_bytes = descriptor_policy_bytes;
    options.transfer_pool_slots = pool_slots;
    options.maximum_report_bytes = report_policy_bytes;
    options.close_drain_timeout_ms = close_drain_timeout_ms;
    options.first_report_attempts = first_report_attempts;
    options.first_report_backoff_us = first_report_backoff_us;
    // The specs this player builds are fixed and already range-checked by the
    // profile factories, so the second wire-image scan is not run per report.
    options.validate_reports = 0U;
    options.aoa_descriptor_wire_policy_bytes = descriptor_policy_bytes;
    options.linux_descriptor_policy_bytes = descriptor_policy_bytes;
    // These five name the audited Linux HID parser revision in libaoahid's
    // docs/FACT_AUDIT.md.
    options.linux_hid_fields_per_report_policy = 256U;
    options.linux_hid_global_stack_depth_policy = 4U;
    options.linux_hid_usages_policy = 12288U;
    options.linux_hid_report_data_bits_policy = 65528U;
    options.linux_hid_report_size_bits_policy = 256U;
    options.target_ep0_data_policy_bytes = descriptor_policy_bytes;
    options.host_control_buffer_policy_bytes = descriptor_policy_bytes;
    options.interface_claim_policy = AOAHID_INTERFACE_CLAIM_NONE;
    options.interface_number = -1;
    return options;
}

aoahid_node_options make_node_options() noexcept {
    aoahid_node_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    // A Node has one report in flight, so one reserved slot removes pool
    // contention between the profiles sharing this Device.
    options.has_reserved_slots = 1U;
    options.reserved_slots = 1U;
    return options;
}

} // namespace

const char* profile_name(const Profile profile) noexcept {
    switch (profile) {
    case Profile::touch:
        return "touch";
    case Profile::mouse:
        return "mouse";
    case Profile::key:
        return "key";
    case Profile::gamepad:
        return "gamepad";
    case Profile::pen:
        return "pen";
    default:
        return "unknown";
    }
}

const char* profile_title(const Profile profile) noexcept {
    switch (profile) {
    case Profile::touch:
        return "Touchscreen";
    case Profile::mouse:
        return "Mouse";
    case Profile::key:
        return "Keyboard";
    case Profile::gamepad:
        return "Gamepad";
    case Profile::pen:
        return "Pen";
    default:
        return "Unknown";
    }
}

std::string describe_profiles(const uint32_t mask) {
    std::vector<std::string> names;
    for (size_t index = 0; index < profile_count; ++index) {
        if ((mask & (uint32_t{1} << index)) == 0U)
            continue;
        std::string name = profile_title(static_cast<Profile>(index));
        for (char& c : name)
            c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        names.push_back(std::move(name));
    }
    std::string text;
    for (size_t index = 0; index < names.size(); ++index) {
        if (index != 0)
            text += index + 1 == names.size() ? " and " : ", ";
        text += names[index];
    }
    return text.empty() ? std::string("no profile") : text;
}

std::string device_label(const aoahid_device_info* info) {
    if (info == nullptr)
        return "(unknown device)";
    char identity[32];
    std::snprintf(identity, sizeof identity, "%04x:%04x", static_cast<unsigned>(info->vendor_id),
                  static_cast<unsigned>(info->product_id));
    std::string text = info->product != nullptr && info->product[0] != '\0' ? info->product
                                                                            : "(no product name)";
    text += "  ";
    text += identity;
    text += "  ";
    text += info->serial != nullptr && info->serial[0] != '\0' ? info->serial : "(no serial)";
    return text;
}

Device::~Device() { close(); }

Device::Device(Device&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)), nodes_(other.nodes_),
      profile_mask_(std::exchange(other.profile_mask_, 0U)), touch_(other.touch_),
      mouse_(other.mouse_), key_(other.key_), gamepad_(other.gamepad_), pen_(other.pen_),
      label_(std::move(other.label_)) {
    other.nodes_.fill(nullptr);
    other.touch_ = {};
    other.mouse_ = {};
    other.key_ = {};
    other.gamepad_ = {};
    other.pen_ = {};
}

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, nullptr);
        nodes_ = other.nodes_;
        profile_mask_ = std::exchange(other.profile_mask_, 0U);
        touch_ = other.touch_;
        mouse_ = other.mouse_;
        key_ = other.key_;
        gamepad_ = other.gamepad_;
        pen_ = other.pen_;
        label_ = std::move(other.label_);
        other.nodes_.fill(nullptr);
        other.touch_ = {};
        other.mouse_ = {};
        other.key_ = {};
        other.gamepad_ = {};
        other.pen_ = {};
    }
    return *this;
}

aoahid_result Device::open(const Context& context, const aoahid_device_info* selected) {
    close();
    if (context.native_handle() == nullptr || selected == nullptr)
        return AOAHID_ERR_PARAM;
    label_ = device_label(selected);
    const aoahid_device_options options = make_device_options();
    return aoahid_device_open(context.native_handle(), selected, &options, &handle_);
}

aoahid_result Device::open_node(const Profile profile, aoahid_spec* spec) noexcept {
    if (handle_ == nullptr || spec == nullptr)
        return AOAHID_ERR_PARAM;
    const size_t index = static_cast<size_t>(profile);
    if (nodes_[index] != nullptr)
        return AOAHID_ERR_PARAM;

    const aoahid_node_options options = make_node_options();
    aoahid_node* opened_node = nullptr;
    const aoahid_result opened = aoahid_node_open(handle_, spec, &options, &opened_node);
    if (opened != AOAHID_OK)
        return opened;

    aoahid_result bound = AOAHID_OK;
    switch (profile) {
    case Profile::touch:
        bound = aoa::bind(opened_node, touch_);
        break;
    case Profile::mouse:
        bound = aoa::bind(opened_node, mouse_);
        break;
    case Profile::key:
        bound = aoa::bind(opened_node, key_);
        break;
    case Profile::gamepad:
        bound = aoa::bind(opened_node, gamepad_);
        break;
    case Profile::pen:
        bound = aoa::bind(opened_node, pen_);
        break;
    default:
        bound = AOAHID_ERR_PARAM;
        break;
    }
    if (bound != AOAHID_OK) {
        static_cast<void>(aoahid_node_close(opened_node));
        return bound;
    }

    nodes_[index] = opened_node;
    profile_mask_ |= profile_bit(profile);
    return AOAHID_OK;
}

void Device::close() noexcept {
    // The first aoahid_device_close consumes the Device and every child Node,
    // including on AOAHID_CLOSE_PENDING, so the Nodes are not closed here.
    if (handle_ != nullptr)
        static_cast<void>(aoahid_device_close(handle_));
    handle_ = nullptr;
    nodes_.fill(nullptr);
    profile_mask_ = 0U;
    touch_ = {};
    mouse_ = {};
    key_ = {};
    gamepad_ = {};
    pen_ = {};
}

} // namespace aoap
