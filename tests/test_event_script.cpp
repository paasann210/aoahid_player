// SPDX-License-Identifier: MIT
// Covers EventScript::load() (CSV row parsing, wait_ms -> wait_ns
// conversion, once/loop routing, comment/BOM handling) and batch_key().
#include "doctest.h"

#include "aoahid_player/event_script.hpp"

#include <ostream>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

// Writes `content` to a fresh temp file and returns its path. Each call uses
// a distinct name so parallel/-repeated test runs can't collide.
std::string write_temp_csv(const std::string& name, const std::string& content) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("aoahid_player_test_" + name + "_" + std::to_string(::rand()) + ".csv");
    std::ofstream out(path, std::ios::binary);
    out << content;
    out.close();
    return path.string();
}

} // namespace

TEST_CASE("EventScript::load parses one row of every profile") {
    const std::string path = write_temp_csv("all_profiles",
        "t,1,1,100,200,16.5\n"
        "m,10,-5,5\n"
        "b,1,1,5\n"
        "k,0x04,1,5\n"
        "g,1,1,5\n"
        "a,0,32767,5\n"
        "h,1,0,0,1,5\n"
        "p,1,1,50,60,2000,5\n");

    aoap::EventScript script;
    std::string error;
    REQUIRE(script.load(path, error));
    CHECK(error.empty());
    CHECK(script.loop_rows.size() == 8);
    CHECK(script.once_rows.empty());

    const auto& touch = std::get<aoap::TouchEvent>(script.loop_rows[0].payload);
    CHECK(touch.finger_id == 1);
    CHECK(touch.state == true);
    CHECK(touch.x == 100);
    CHECK(touch.y == 200);
    // 16.5ms -> 16,500,000ns; the multiply happens once at parse time.
    CHECK(script.loop_rows[0].wait_ns == 16'500'000);

    const auto& key = std::get<aoap::KeyEvent>(script.loop_rows[3].payload);
    CHECK(key.usage == 0x04); // hex column accepted via the 0x.. prefix
    CHECK(key.down == true);

    const auto& pen = std::get<aoap::PenSample>(script.loop_rows[7].payload);
    CHECK(pen.in_range == true);
    CHECK(pen.tip == true);
    CHECK(pen.pressure == 2000);

    std::filesystem::remove(path);
}

TEST_CASE("EventScript::load routes uppercase prefixes to once_rows") {
    const std::string path =
        write_temp_csv("once_vs_loop", "T,1,1,0,0,0\nt,2,1,0,0,10\n");

    aoap::EventScript script;
    std::string error;
    REQUIRE(script.load(path, error));
    CHECK(script.once_rows.size() == 1);
    CHECK(script.loop_rows.size() == 1);
    CHECK(script.once_rows[0].once == true);
    CHECK(script.loop_rows[0].once == false);

    std::filesystem::remove(path);
}

TEST_CASE("EventScript::load skips comments, blank lines, and a UTF-8 BOM") {
    const std::string path = write_temp_csv(
        "comments",
        "\xEF\xBB\xBF# leading comment on the BOM line\n"
        "\n"
        "   \n"
        "t,1,1,0,0,5 # trailing comment\n"
        "# whole-line comment\n"
        "t,2,1,0,0,5\n");

    aoap::EventScript script;
    std::string error;
    REQUIRE(script.load(path, error));
    CHECK(script.size() == 2);

    std::filesystem::remove(path);
}

TEST_CASE("EventScript::load rejects a malformed row with a file:line message") {
    const std::string path = write_temp_csv("broken", "t,0,1,500\n"); // missing a column

    aoap::EventScript script;
    std::string error;
    CHECK_FALSE(script.load(path, error));
    CHECK(error.find(":1:") != std::string::npos);

    std::filesystem::remove(path);
}

TEST_CASE("EventScript::load rejects pen tip=1 without in_range=1") {
    const std::string path = write_temp_csv("pen_invariant", "p,0,1,50,60,2000,5\n");

    aoap::EventScript script;
    std::string error;
    CHECK_FALSE(script.load(path, error));
    CHECK(error.find("in_range") != std::string::npos);

    std::filesystem::remove(path);
}

TEST_CASE("EventScript::load rejects an unknown row prefix") {
    const std::string path = write_temp_csv("bad_prefix", "z,1,2,3\n");

    aoap::EventScript script;
    std::string error;
    CHECK_FALSE(script.load(path, error));

    std::filesystem::remove(path);
}

TEST_CASE("EventScript::load rejects a file with no event rows") {
    const std::string path = write_temp_csv("empty", "# only a comment\n\n");

    aoap::EventScript script;
    std::string error;
    CHECK_FALSE(script.load(path, error));
    CHECK(error.find("no event rows") != std::string::npos);

    std::filesystem::remove(path);
}

TEST_CASE("EventScript::load reports a missing file") {
    aoap::EventScript script;
    std::string error;
    CHECK_FALSE(script.load("/no/such/path/really.csv", error));
    CHECK(error.find("cannot open") != std::string::npos);
}

TEST_CASE("batch_key distinguishes controls and merges mouse moves") {
    using aoap::batch_key;

    CHECK(batch_key(aoap::TouchEvent{0, true, 0, 0}) != batch_key(aoap::TouchEvent{1, true, 0, 0}));
    CHECK(batch_key(aoap::TouchEvent{3, true, 10, 20}) ==
          batch_key(aoap::TouchEvent{3, false, 99, 99})); // key ignores state/x/y, only finger_id
    CHECK(batch_key(aoap::MouseMove{1, 1}) == batch_key(aoap::MouseMove{-5, 8}));
    CHECK(batch_key(aoap::KeyEvent{0x04, true}) != batch_key(aoap::MouseButton{1, true}));
}

TEST_CASE("Timeline accumulates waits per segment and finds seek rows") {
    aoap::EventScript script;
    script.once_rows.push_back({aoap::KeyEvent{0x04, true}, true, 8'000'000});
    script.loop_rows.push_back({aoap::TouchEvent{0, true, 1, 1}, false, 0});
    script.loop_rows.push_back({aoap::TouchEvent{1, true, 2, 2}, false, 10'000'000});
    script.loop_rows.push_back({aoap::TouchEvent{0, false, 1, 1}, false, 5'000'000});

    const aoap::Timeline timeline = aoap::build_timeline(script);
    CHECK(timeline.rows(aoap::Segment::intro) == 1);
    CHECK(timeline.duration(aoap::Segment::intro) == 8'000'000);
    CHECK(timeline.rows(aoap::Segment::loop) == 3);
    CHECK(timeline.duration(aoap::Segment::loop) == 15'000'000);

    // Rows 0 and 1 share t=0 (a zero-wait batch); row 2 starts at 10 ms.
    CHECK(timeline.row_at(aoap::Segment::loop, 0) == 0);
    CHECK(timeline.row_at(aoap::Segment::loop, 1) == 2);
    CHECK(timeline.row_at(aoap::Segment::loop, 10'000'000) == 2);
    CHECK(timeline.row_at(aoap::Segment::loop, 10'000'001) == 3);
    CHECK(timeline.row_at(aoap::Segment::loop, 99'000'000) == 3);
}

TEST_CASE("A '# screen WxH' comment names the coordinate space") {
    aoap::EventScript script;
    std::string error;
    REQUIRE(script.load(write_temp_csv("screen_directive.csv",
                                       "# recorded by something\n"
                                       "#   Screen 4096X4096  \n"
                                       "# screen 1x1\n" // only the first one counts
                                       "t,0,1,2048,1024,0\n"),
                        error));
    CHECK(script.screen_width == 4096);
    CHECK(script.screen_height == 4096);

    aoap::EventScript plain;
    REQUIRE(plain.load(write_temp_csv("screen_none.csv", "# screensaver 2x3\nt,0,1,5,5,0\n"),
                       error));
    CHECK(plain.screen_width == 0);
    CHECK(plain.screen_height == 0);
}

TEST_CASE("scale_script maps touch and pen rows onto the connected surfaces") {
    aoap::EventScript script;
    script.screen_width = 4096;
    script.screen_height = 4096;
    script.once_rows.push_back({aoap::TouchEvent{0, true, 0, 0}, true, 0});
    script.loop_rows.push_back({aoap::TouchEvent{1, true, 2048, 4095}, false, 1'000'000});
    script.loop_rows.push_back({aoap::PenSample{true, true, 4095, 1024, 100}, false, 0});
    script.loop_rows.push_back({aoap::KeyEvent{0x04, true}, false, 0});

    aoap::EventScript scaled;
    REQUIRE(aoap::scale_script(script, 1080, 2400, 32768, 32768, scaled));
    const auto& first = std::get<aoap::TouchEvent>(scaled.once_rows[0].payload);
    CHECK(first.x == 0);
    CHECK(first.y == 0);
    const auto& moved = std::get<aoap::TouchEvent>(scaled.loop_rows[0].payload);
    CHECK(moved.x == 540);  // the middle stays the middle
    CHECK(moved.y == 2399); // the last coordinate stays inside the surface
    const auto& pen = std::get<aoap::PenSample>(scaled.loop_rows[1].payload);
    CHECK(pen.x == 32760);
    CHECK(pen.y == 8192);
    CHECK(std::get<aoap::KeyEvent>(scaled.loop_rows[2].payload).usage == 0x04);
    CHECK(scaled.loop_rows[0].wait_ns == 1'000'000); // timing untouched
    CHECK(scaled.screen_width == 1080);

    aoap::EventScript untouched;
    CHECK_FALSE(aoap::scale_script(script, 4096, 4096, 0, 0, untouched)); // same size
    aoap::EventScript undeclared;
    undeclared.loop_rows = script.loop_rows;
    CHECK_FALSE(aoap::scale_script(undeclared, 1080, 2400, 0, 0, untouched)); // no space
}
