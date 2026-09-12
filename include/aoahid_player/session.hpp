// SPDX-License-Identifier: MIT
#pragma once

#include <aoahid.h>

#include <cstdint>
#include <string>
#include <vector>

#include "context.hpp"
#include "device_group.hpp"
#include "events.hpp"
#include "spec_builder.hpp"

namespace aoap {

// One AOA-capable device from the last refresh.
struct DeviceEntry {
    std::string label; // "product  vid:pid  serial"
    std::string product;
    std::string serial;
    uint16_t vendor_id{};
    uint16_t product_id{};
    std::string key; // stable across refreshes, for keeping a selection
};

// Owns the libaoahid Context, the device list, and the connected DeviceGroup.
//
// Connecting builds one Spec per enabled profile and opens a Node for each on
// every selected device; the profiles stay fixed until disconnect(). Every
// method belongs to the single thread that owns the Context.
class Session {
  public:
    explicit Session(EventSink* sink = nullptr);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    // Lists AOA-capable devices. Not allowed while connected.
    bool refresh(std::vector<DeviceEntry>& devices, std::string& error);

    // Opens the given entries of the last refresh. A device that fails is
    // reported through the sink and skipped; false means none could be opened.
    bool connect(const std::vector<size_t>& selection, const ProfileSetup& setup,
                 std::string& error);

    // Closes every device; each Node close sends its neutral report first.
    void disconnect() noexcept;

    [[nodiscard]] bool connected() const noexcept { return !group_.empty(); }
    // The profiles of the current connection; meaningful while connected.
    [[nodiscard]] const ProfileSetup& setup() const noexcept { return setup_; }
    [[nodiscard]] DeviceGroup& group() noexcept { return group_; }
    [[nodiscard]] const DeviceGroup& group() const noexcept { return group_; }

  private:
    bool ensure_context(std::string& error);
    void note(Severity severity, const std::string& text) const;

    EventSink* sink_;
    Context context_;
    aoahid_discovery* discovery_{};
    SpecSet specs_;
    ProfileSetup setup_;
    DeviceGroup group_;
};

} // namespace aoap
