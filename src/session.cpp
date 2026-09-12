// SPDX-License-Identifier: MIT
#include "aoahid_player/session.hpp"

#include <cstdio>
#include <utility>

namespace aoap {
namespace {

constexpr uint32_t discover_timeout_ms = 500U;

std::string text_or(const char* value, const char* fallback) {
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

} // namespace

Session::Session(EventSink* sink) : sink_(sink), group_(sink) {}

Session::~Session() {
    disconnect();
    if (discovery_ != nullptr)
        aoahid_discovery_destroy(discovery_);
}

void Session::note(const Severity severity, const std::string& text) const {
    if (sink_ != nullptr)
        sink_->message(severity, text);
}

bool Session::ensure_context(std::string& error) {
    if (context_)
        return true;
    const aoahid_result result = context_.create();
    if (result != AOAHID_OK) {
        // UNSUPPORTED here means libusb itself could not start, not the phone.
        error = "USB access could not be initialised (" + describe_error(result) + ").";
        return false;
    }
    return true;
}

bool Session::refresh(std::vector<DeviceEntry>& devices, std::string& error) {
    devices.clear();
    if (connected()) {
        error = "Disconnect before refreshing the device list.";
        return false;
    }
    if (!ensure_context(error))
        return false;

    aoahid_discovery* found = nullptr;
    // aoahid_discover already probes AOA request 51 and returns only devices
    // that answered with a nonzero protocol version.
    const aoahid_result result = aoahid_discover(context_.native_handle(), discover_timeout_ms,
                                                 &found);
    if (result != AOAHID_OK) {
        error = "Searching for devices failed. " + explain_error(result);
        return false;
    }
    if (discovery_ != nullptr)
        aoahid_discovery_destroy(discovery_);
    discovery_ = found;

    const size_t count = aoahid_discovery_count(discovery_);
    devices.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const aoahid_device_info* info = aoahid_discovery_get(discovery_, index);
        if (info == nullptr)
            continue;
        DeviceEntry entry;
        entry.label = device_label(info);
        entry.product = text_or(info->product, "Android device");
        entry.serial = text_or(info->serial, "");
        entry.vendor_id = info->vendor_id;
        entry.product_id = info->product_id;
        char identity[48];
        std::snprintf(identity, sizeof identity, "%04x:%04x@%u.%u",
                      static_cast<unsigned>(info->vendor_id),
                      static_cast<unsigned>(info->product_id),
                      static_cast<unsigned>(info->bus_number),
                      static_cast<unsigned>(info->device_address));
        // The serial survives a replug; the bus address is the fallback.
        entry.key = entry.serial.empty() ? std::string(identity) : entry.serial;
        devices.push_back(std::move(entry));
    }
    return true;
}

bool Session::connect(const std::vector<size_t>& selection, const ProfileSetup& setup,
                      std::string& error) {
    if (connected()) {
        error = "Already connected. Disconnect first.";
        return false;
    }
    if (discovery_ == nullptr) {
        error = "Refresh the device list first.";
        return false;
    }
    if (selection.empty()) {
        error = "Select at least one device.";
        return false;
    }
    if (!validate_setup(setup, error))
        return false;
    if (!ensure_context(error))
        return false;
    if (!specs_.build(setup, error))
        return false;

    const size_t count = aoahid_discovery_count(discovery_);
    for (const size_t index : selection) {
        const aoahid_device_info* info =
            index < count ? aoahid_discovery_get(discovery_, index) : nullptr;
        if (info == nullptr) {
            note(Severity::warning, "A selected device is no longer in the list; refresh it.");
            continue;
        }
        const std::string label = device_label(info);
        Device device;
        aoahid_result result = device.open(context_, info);
        if (result != AOAHID_OK) {
            note(Severity::warning, "Could not open " + label + ". " + explain_error(result));
            continue;
        }
        bool ready = true;
        for (size_t profile = 0; profile < profile_count && ready; ++profile) {
            aoahid_spec* spec = specs_.get(static_cast<Profile>(profile));
            if (spec == nullptr)
                continue;
            result = device.open_node(static_cast<Profile>(profile), spec);
            if (result != AOAHID_OK) {
                note(Severity::warning,
                     std::string("Could not register the ") +
                         describe_profiles(profile_bit(static_cast<Profile>(profile))) + " on " +
                         label + ". " + explain_error(result));
                ready = false;
            }
        }
        if (!ready)
            continue; // Device's destructor closes whatever was opened
        note(Severity::info, "Connected " + label + ".");
        group_.add_device(std::move(device));
    }

    if (group_.empty()) {
        specs_.release();
        error = "None of the selected devices could be connected.";
        return false;
    }
    setup_ = setup;
    return true;
}

void Session::disconnect() noexcept {
    group_.reset();
    specs_.release();
}

} // namespace aoap
