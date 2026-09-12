// SPDX-License-Identifier: MIT
// Covers InputState, which the player uses to release everything on stop or
// pause and to rebuild the held state at a seek target.
#include "doctest.h"

#include "aoahid_player/input_state.hpp"

#include <ostream>
#include <vector>

using aoap::EventPayload;
using aoap::InputState;

namespace {

template <typename T>
size_t count_of(const std::vector<EventPayload>& rows) {
    size_t count = 0;
    for (const EventPayload& row : rows)
        count += std::holds_alternative<T>(row) ? 1U : 0U;
    return count;
}

} // namespace

TEST_CASE("A fresh InputState is neutral and mouse motion leaves no state") {
    InputState state;
    CHECK(state.neutral());
    state.apply(aoap::MouseMove{10, -3});
    CHECK(state.neutral());
}

TEST_CASE("Releasing everything lifts contacts at their last position") {
    InputState held;
    held.apply(aoap::TouchEvent{2, true, 100, 200});
    held.apply(aoap::TouchEvent{2, true, 150, 250}); // moved
    held.apply(aoap::KeyEvent{0x04, true});
    held.apply(aoap::MouseButton{1, true});
    held.apply(aoap::GamepadButton{3, true});
    held.apply(aoap::GamepadAxis{1, -500});
    held.apply(aoap::GamepadDpad{true, false, false, false});
    held.apply(aoap::PenSample{true, true, 5, 6, 100});
    CHECK_FALSE(held.neutral());

    std::vector<EventPayload> releases;
    std::vector<EventPayload> presses;
    InputState::transition(held, InputState{}, releases, presses);
    CHECK(presses.empty());
    CHECK(releases.size() == 7);

    const auto* lift = std::get_if<aoap::TouchEvent>(&releases.front());
    REQUIRE(lift != nullptr);
    CHECK(lift->finger_id == 2);
    CHECK_FALSE(lift->state);
    CHECK(lift->x == 150);
    CHECK(lift->y == 250);
    CHECK(count_of<aoap::KeyEvent>(releases) == 1);
    CHECK(count_of<aoap::PenSample>(releases) == 1);
}

TEST_CASE("A transition only sends what differs") {
    InputState from;
    from.apply(aoap::TouchEvent{0, true, 10, 10});
    from.apply(aoap::KeyEvent{0x05, true});

    InputState to;
    to.apply(aoap::TouchEvent{0, true, 10, 10}); // unchanged
    to.apply(aoap::TouchEvent{1, true, 20, 20}); // new
    to.apply(aoap::KeyEvent{0x06, true});        // 0x05 up, 0x06 down

    std::vector<EventPayload> releases;
    std::vector<EventPayload> presses;
    InputState::transition(from, to, releases, presses);
    CHECK(releases.size() == 1);
    CHECK(count_of<aoap::KeyEvent>(releases) == 1);
    CHECK(presses.size() == 2);
    CHECK(count_of<aoap::TouchEvent>(presses) == 1);
    CHECK(count_of<aoap::KeyEvent>(presses) == 1);

    InputState::transition(to, to, releases, presses);
    CHECK(releases.empty());
    CHECK(presses.empty());
}

TEST_CASE("Out-of-range controls are ignored rather than tracked") {
    InputState state;
    state.apply(aoap::TouchEvent{16, true, 1, 1});
    state.apply(aoap::KeyEvent{0x1FF, true});
    state.apply(aoap::GamepadAxis{InputState::max_axes, 5});
    CHECK(state.neutral());
}
