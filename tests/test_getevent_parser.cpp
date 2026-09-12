// SPDX-License-Identifier: MIT
// Covers GeteventParser::feed()/finish() (raw `adb shell getevent -lt`
// line parsing, frame batching, inter-frame wait_ms derivation) and
// key_usage() (Linux keycode/name -> HID usage mapping). This app is
// deliberately independent of libaoahid (see apps/aoa_record/CMakeLists.txt),
// so this test binary links only event_script.hpp's header-only types and
// getevent_parser.cpp -- no aoahid device or context needed.
#include "doctest.h"

#include "aoahid_player/getevent_parser.hpp"

#include <ostream>

#include <cmath>

using aoap::EventPayload;
using aoap::EventRecord;
using aoap::KeyEvent;
using aoap::TouchEvent;
using aoap::record::GeteventParser;

namespace {

void feed_lines(GeteventParser& parser, const std::vector<std::string>& lines,
                std::vector<EventRecord>& out) {
    for (const std::string& line : lines)
        parser.feed(line, 0, out);
}

} // namespace

TEST_CASE("GeteventParser turns one tap into a down row then an up row") {
    GeteventParser parser;
    std::vector<EventRecord> out;

    feed_lines(parser,
               {
                   "[   100.100000] /dev/input/event4: EV_ABS       ABS_MT_SLOT          00000000",
                   "[   100.100000] /dev/input/event4: EV_ABS       ABS_MT_TRACKING_ID   00000005",
                   "[   100.100000] /dev/input/event4: EV_ABS       ABS_MT_POSITION_X    00000064",
                   "[   100.100000] /dev/input/event4: EV_ABS       ABS_MT_POSITION_Y    000000c8",
                   "[   100.100000] /dev/input/event4: EV_SYN       SYN_REPORT           00000000",
                   "[   100.200000] /dev/input/event4: EV_ABS       ABS_MT_TRACKING_ID   ffffffff",
                   "[   100.200000] /dev/input/event4: EV_SYN       SYN_REPORT           00000000",
               },
               out);
    parser.finish(out);

    REQUIRE(out.size() == 2);

    const auto& down = std::get<TouchEvent>(out[0].payload);
    CHECK(down.finger_id == 0);
    CHECK(down.state == true);
    CHECK(down.x == 0x64);
    CHECK(down.y == 0xc8);
    CHECK(out[0].once == false);
    // The down row carries the wait to the frame that follows it (~100ms).
    CHECK(out[0].wait_ns == doctest::Approx(100'000'000).epsilon(0.01));

    const auto& up = std::get<TouchEvent>(out[1].payload);
    CHECK(up.finger_id == 0);
    CHECK(up.state == false);
    // No frame follows the up row, so finish() emits it with no wait.
    CHECK(out[1].wait_ns == 0);
}

TEST_CASE("GeteventParser only records the device passed to set_device_filter") {
    GeteventParser parser;
    parser.set_device_filter("/dev/input/event4");
    std::vector<EventRecord> out;

    feed_lines(parser,
               {
                   "[ 1.000000] /dev/input/event7: EV_ABS ABS_MT_SLOT 00000000",
                   "[ 1.000000] /dev/input/event7: EV_ABS ABS_MT_TRACKING_ID 00000001",
                   "[ 1.000000] /dev/input/event7: EV_SYN SYN_REPORT 00000000",
               },
               out);
    parser.finish(out);

    CHECK(out.empty()); // event7 is filtered out, only event4 would pass
}

TEST_CASE("GeteventParser stages a key press and release into separate frames") {
    GeteventParser parser;
    std::vector<EventRecord> out;

    feed_lines(parser,
               {
                   "[ 5.000000] /dev/input/event3: EV_KEY KEY_A DOWN",
                   "[ 5.000000] /dev/input/event3: EV_SYN SYN_REPORT 00000000",
                   "[ 5.050000] /dev/input/event3: EV_KEY KEY_A UP",
                   "[ 5.050000] /dev/input/event3: EV_SYN SYN_REPORT 00000000",
               },
               out);
    parser.finish(out);

    REQUIRE(out.size() == 2);
    const auto& press = std::get<KeyEvent>(out[0].payload);
    CHECK(press.usage == 0x04);
    CHECK(press.down == true);
    const auto& release = std::get<KeyEvent>(out[1].payload);
    CHECK(release.usage == 0x04);
    CHECK(release.down == false);
}

TEST_CASE("GeteventParser counts an unmapped key instead of dropping the line silently") {
    GeteventParser parser;
    std::vector<EventRecord> out;

    feed_lines(parser,
               {
                   "[ 1.0] /dev/input/event3: EV_KEY KEY_VOLUMEUP DOWN",
                   "[ 1.0] /dev/input/event3: EV_SYN SYN_REPORT 00000000",
               },
               out);
    parser.finish(out);

    CHECK(out.empty());
    CHECK(parser.unmapped_keys() == 1);
    CHECK(parser.first_unmapped_key() == "KEY_VOLUMEUP");
}

TEST_CASE("GeteventParser::key_usage resolves names and raw hex codes alike") {
    uint16_t usage = 0;

    REQUIRE(GeteventParser::key_usage("KEY_A", usage));
    CHECK(usage == 0x04);

    // getevent prints the raw hex Linux keycode when it has no symbolic name;
    // 1e (30) is KEY_A's own code, so this must resolve to the same usage.
    REQUIRE(GeteventParser::key_usage("1e", usage));
    CHECK(usage == 0x04);

    CHECK_FALSE(GeteventParser::key_usage("KEY_TOTALLY_MADE_UP", usage));
}

TEST_CASE("parse_touch_range reads the multi-touch axis range from getevent -lp") {
    const std::string text = "add device 1: /dev/input/event5\n"
                             "  name:     \"gpio-keys\"\n"
                             "add device 2: /dev/input/event2\r\n"
                             "  name:     \"sec_touchscreen\"\n"
                             "    ABS (0003): ABS_MT_SLOT : value 0, min 0, max 9, fuzz 0\n"
                             "                ABS_MT_POSITION_X : value 0, min 0, max 1079, fuzz 0\n"
                             "                ABS_MT_POSITION_Y : value 0, min 0, max 2399, fuzz 0\n"
                             "add device 3: /dev/input/event7\n"
                             "                ABS_MT_POSITION_X : value 0, min 0, max 4095\n"
                             "                ABS_MT_POSITION_Y : value 0, min 0, max 4095\n";
    int32_t width = 0;
    int32_t height = 0;
    REQUIRE(aoap::record::parse_touch_range(text, "", width, height));
    CHECK(width == 1080);
    CHECK(height == 2400);
    REQUIRE(aoap::record::parse_touch_range(text, "/dev/input/event7", width, height));
    CHECK(width == 4096);
    CHECK(height == 4096);
    CHECK_FALSE(aoap::record::parse_touch_range(text, "/dev/input/event5", width, height));
    // A non-zero minimum is not a plain 0..max space, so no range is given.
    CHECK_FALSE(aoap::record::parse_touch_range(
        "add device 1: /x\n ABS_MT_POSITION_X : value 0, min 10, max 100\n"
        " ABS_MT_POSITION_Y : value 0, min 0, max 100\n",
        "", width, height));
}
