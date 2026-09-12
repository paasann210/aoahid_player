// SPDX-License-Identifier: MIT
#include "aoahid_player/adb.hpp"

#include "aoahid_player/process.hpp"

#include <cstdio>
#include <sstream>

namespace aoap {
namespace {

std::string first_line(const std::string& text) {
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        // adb reports its own server start-up on lines starting with '*'.
        if (!line.empty() && line.front() != '*')
            return line;
    }
    return {};
}

// Turns a failed adb run into one readable sentence.
bool check(const CommandResult& result, std::string& error) {
    if (!result.started) {
        error = "adb could not be run: " + result.start_error +
                ". Install Android SDK Platform-Tools and add it to PATH.";
        return false;
    }
    if (result.timed_out) {
        error = "adb did not answer within the time limit.";
        return false;
    }
    if (result.exit_code != 0) {
        std::string reason = first_line(result.error_output);
        if (reason.empty())
            reason = first_line(result.output);
        error = reason.empty() ? "adb failed (exit code " + std::to_string(result.exit_code) + ")."
                               : "adb: " + reason;
        return false;
    }
    return true;
}

} // namespace

std::vector<std::string> adb_command(const std::string& serial,
                                     const std::vector<std::string>& arguments) {
    std::vector<std::string> command{"adb"};
    if (!serial.empty()) {
        command.emplace_back("-s");
        command.push_back(serial);
    }
    command.insert(command.end(), arguments.begin(), arguments.end());
    return command;
}

bool adb_list_devices(std::vector<AdbDevice>& devices, std::string& error) {
    devices.clear();
    const CommandResult result = run_command(adb_command({}, {"devices", "-l"}));
    if (!check(result, error))
        return false;
    std::istringstream stream(result.output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line.front() == '*' || line.rfind("List of devices", 0) == 0)
            continue;
        std::istringstream fields(line);
        AdbDevice device;
        if (!(fields >> device.serial >> device.state))
            continue;
        std::string field;
        while (fields >> field) {
            if (field.rfind("model:", 0) == 0) {
                device.model = field.substr(6);
                for (char& c : device.model)
                    c = c == '_' ? ' ' : c;
            }
        }
        devices.push_back(std::move(device));
    }
    return true;
}

bool adb_screen_size(const std::string& serial, int32_t& width, int32_t& height,
                     std::string& error) {
    const CommandResult result = run_command(adb_command(serial, {"shell", "wm", "size"}));
    if (!check(result, error))
        return false;
    int physical_w = 0;
    int physical_h = 0;
    int override_w = 0;
    int override_h = 0;
    std::istringstream stream(result.output);
    std::string line;
    while (std::getline(stream, line)) {
        int w = 0;
        int h = 0;
        if (std::sscanf(line.c_str(), " Override size: %dx%d", &w, &h) == 2) {
            override_w = w;
            override_h = h;
        } else if (std::sscanf(line.c_str(), " Physical size: %dx%d", &w, &h) == 2) {
            physical_w = w;
            physical_h = h;
        }
    }
    // sscanf cannot report overflow, so the result is range-checked here.
    const auto plausible = [](const int value) { return value > 0 && value <= 65536; };
    if (plausible(override_w) && plausible(override_h)) {
        width = override_w;
        height = override_h;
        return true;
    }
    if (plausible(physical_w) && plausible(physical_h)) {
        width = physical_w;
        height = physical_h;
        return true;
    }
    error = "adb did not report a screen size.";
    return false;
}

bool adb_kill_server(std::string& error) {
    return check(run_command(adb_command({}, {"kill-server"})), error);
}

} // namespace aoap
