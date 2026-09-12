// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "aoahid_player/spec_builder.hpp"

namespace aoap::cli {

// Parsed --touch-res / --gamepad-axes / etc. Used once at startup to build
// the immutable aoahid_spec via spec_builder.hpp; never re-parsed per loop.
// See README.md's "aoa_touch — usage" section for the flag list, including
// --devices (skips the interactive picker when given) and --speed / --loop / -A.
struct Options {
    // Device selection. `all_devices` is set by --devices all; an empty
    // `devices` with `have_devices == false` means "ask interactively".
    bool have_devices{};
    bool all_devices{};
    std::vector<size_t> devices; // one-based indices from the discovery list

    // Profile setup, ready for SpecSet::build().
    ProfileSetup profiles;
    bool auto_resolution{}; // -A: read the size from `adb shell wm size`

    // Playback.
    double speed{1.0};
    int64_t loop_count{}; // 0 = infinite
    std::string script_path;
    bool no_prompt{}; // --no-prompt: skip the live "-> " offset thread
    bool help{};      // -h/--help was given: usage was printed, exit 0
};

// Returns std::nullopt after printing the problem when the command line is
// invalid. -h/--help is not an error: usage is printed and the returned
// Options has `help` set, so the caller exits successfully.
std::optional<Options> parse(int argc, char** argv);

void print_usage(const char* program);

} // namespace aoap::cli
