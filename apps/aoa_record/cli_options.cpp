// SPDX-License-Identifier: MIT
#include "cli_options.hpp"

#include <cstdio>
#include <string_view>

namespace aoap::record::cli {
namespace {

bool needs_value(const int index, const int argc, const char* flag) {
    if (index + 1 < argc)
        return true;
    std::fprintf(stderr, "[ERROR] %s needs a value\n", flag);
    return false;
}

} // namespace

void print_usage(const char* program) {
    std::printf("usage: %s [options]\n"
                "\n"
                "  -o, --output NAME   Recording name or path. A bare name is saved in the\n"
                "                       csv/ folder next to this program (\".csv\" is added\n"
                "                       when missing). Default: record-<date>-<time>\n"
                "  -s, --serial ID     adb device serial, for `adb -s ID`\n"
                "      --input PATH    Only record /dev/input/eventN; default is every\n"
                "                       device getevent reports\n"
                "      --echo          Print each captured row while recording\n"
                "  -h, --help          Show this text\n"
                "\n"
                "Recording stops on Ctrl+C; the CSV is flushed and closed first.\n",
                program);
}

std::optional<Options> parse(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "-h" || argument == "--help") {
            print_usage(argv[0]);
            options.help = true;
            return options;
        }
        if (argument == "-o" || argument == "--output") {
            if (!needs_value(index, argc, "-o"))
                return std::nullopt;
            options.output = argv[++index];
            continue;
        }
        if (argument == "-s" || argument == "--serial") {
            if (!needs_value(index, argc, "-s"))
                return std::nullopt;
            options.adb_serial = argv[++index];
            continue;
        }
        if (argument == "--input") {
            if (!needs_value(index, argc, "--input"))
                return std::nullopt;
            options.input_device = argv[++index];
            continue;
        }
        if (argument == "--echo") {
            options.echo = true;
            continue;
        }
        std::fprintf(stderr, "[ERROR] unknown option \"%.*s\"\n",
                     static_cast<int>(argument.size()), argument.data());
        return std::nullopt;
    }
    return options;
}

} // namespace aoap::record::cli
