// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aoap {

// Small helpers around the `adb` command line tool, which must be on PATH.
// Every function blocks, so a GUI calls them off its UI thread. Errors are
// returned as sentences a user can act on.

struct AdbDevice {
    std::string serial;
    std::string state; // "device", "unauthorized", "offline", ...
    std::string model; // may be empty
};

// `adb [-s serial] args...`; an empty serial lets adb pick the only device.
std::vector<std::string> adb_command(const std::string& serial,
                                     const std::vector<std::string>& arguments);

bool adb_list_devices(std::vector<AdbDevice>& devices, std::string& error);

// Reads `adb shell wm size`. An active size override wins over the physical
// size, because the override is what apps (and touch input) use.
bool adb_screen_size(const std::string& serial, int32_t& width, int32_t& height,
                     std::string& error);

// The adb server keeps the phone's USB device open, which can stop the AOA
// handshake from reaching it, so it is stopped before connecting.
bool adb_kill_server(std::string& error);

} // namespace aoap
