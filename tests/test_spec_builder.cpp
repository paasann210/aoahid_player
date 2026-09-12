// SPDX-License-Identifier: MIT
// Covers spec_detail::unsigned_bits() and axis_is_unipolar(), the small pure
// functions inside spec_builder.hpp that decide HID logical-range bit
// widths. The rest of spec_builder.hpp calls into libaoahid itself (spec
// creation), which needs a real device/context to exercise meaningfully and
// is out of scope for a unit test.
#include <ostream>

#include "doctest.h"

#include "aoahid_player/spec_builder.hpp"

using aoap::spec_detail::axis_is_unipolar;
using aoap::spec_detail::unsigned_bits;

TEST_CASE("unsigned_bits picks the smallest width that represents the maximum") {
    CHECK(unsigned_bits(0) == 1);
    CHECK(unsigned_bits(1) == 1);
    CHECK(unsigned_bits(2) == 2);
    CHECK(unsigned_bits(15) == 4);   // e.g. 15 simultaneous touch contacts
    CHECK(unsigned_bits(255) == 8);
    CHECK(unsigned_bits(1079) == 11); // 1080-wide touch surface, width - 1
    CHECK(unsigned_bits(1919) == 11); // 1920-tall touch surface, height - 1
    CHECK(unsigned_bits(4095) == 12); // 12-bit pressure, the CLI default
}

TEST_CASE("unsigned_bits never returns a width the value doesn't fit in") {
    for (const int64_t maximum : {0, 1, 7, 8, 9, 1023, 1024, 1025, 65535, 65536}) {
        const uint32_t bits = unsigned_bits(maximum);
        REQUIRE(bits >= 1);
        REQUIRE(bits <= 32);
        // (1 << bits) - 1 must be able to represent `maximum`.
        CHECK((int64_t{1} << bits) - 1 >= maximum);
    }
}

TEST_CASE("axis_is_unipolar matches only the 0..max simulation axes") {
    CHECK(axis_is_unipolar(AOAHID_AXIS_SIMULATION_ACCELERATOR));
    CHECK(axis_is_unipolar(AOAHID_AXIS_SIMULATION_BRAKE));
    CHECK(axis_is_unipolar(AOAHID_AXIS_SIMULATION_THROTTLE));

    CHECK_FALSE(axis_is_unipolar(AOAHID_AXIS_X));
    CHECK_FALSE(axis_is_unipolar(AOAHID_AXIS_SIMULATION_STEERING)); // signed, not unipolar
    CHECK_FALSE(axis_is_unipolar(AOAHID_AXIS_SIMULATION_RUDDER));
}

TEST_CASE("find_axis resolves the same entry by name and by role") {
    const auto* by_name = aoap::spec_detail::find_axis(std::string_view{"steering"});
    const auto* by_role = aoap::spec_detail::find_axis(AOAHID_AXIS_SIMULATION_STEERING);
    REQUIRE(by_name != nullptr);
    REQUIRE(by_role != nullptr);
    CHECK(by_name == by_role);
    CHECK(by_name->linux_code == std::string_view{"ABS_WHEEL"});
}

TEST_CASE("find_axis returns nullptr for an unknown axis name") {
    CHECK(aoap::spec_detail::find_axis(std::string_view{"not-a-real-axis"}) == nullptr);
}
