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

// Starts the adb server if it is not already running. Used before reading
// the screen size, so `wm size` does not pay the server's own start-up cost.
bool adb_start_server(std::string& error);

// True when `error`, as set by a failed call above, means the `adb`
// executable itself could not be launched (missing from PATH), as opposed to
// adb running but reporting some other problem.
bool adb_missing(const std::string& error);

} // namespace aoap
