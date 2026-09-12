# Unit tests for the parsing-, bit-math- and state-heavy pieces of the core:
# CSV scripts and timelines, HID descriptor bit widths, `adb shell getevent`
# parsing, and the input-state transitions used for stop, pause, and seek.
#
# doctest is vendored as a single header (third_party/doctest.h) rather than
# fetched, so the tests build offline once libaoahid itself is available.
add_executable(aoahid_player_tests
  test_main.cpp
  test_event_script.cpp
  test_spec_builder.cpp
  test_getevent_parser.cpp
  test_input_state.cpp
)
target_include_directories(aoahid_player_tests PRIVATE third_party)
target_link_libraries(aoahid_player_tests PRIVATE aoahid_player_core)
target_compile_features(aoahid_player_tests PRIVATE cxx_std_20)

add_test(NAME aoahid_player_tests COMMAND aoahid_player_tests)
if(WIN32)
  # The tests run from out/<config>, where the stage target puts the DLLs.
  add_dependencies(aoahid_player_tests aoahid_player_stage)
endif()
