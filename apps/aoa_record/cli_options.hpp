// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>

namespace aoap::record::cli {

// See README.md's "aoa_record — usage" section. Everything is optional: with
// no -o, the recording is saved as csv/record-<date>-<time>.csv next to the
// executable, where aoa_touch and the GUI list scripts.
struct Options {
    std::string output;       // -o: bare name (saved in csv/) or a path
    std::string adb_serial;   // -s: passed through to `adb -s <serial>`
    std::string input_device; // --input /dev/input/eventN, else every device
    bool echo{};              // --echo: also print each captured row
    bool help{};              // -h/--help was given: usage was printed, exit 0
};

// std::nullopt means the command line was invalid. -h/--help is not an error:
// usage is printed and the returned Options has `help` set.
std::optional<Options> parse(int argc, char** argv);

void print_usage(const char* program);

} // namespace aoap::record::cli
