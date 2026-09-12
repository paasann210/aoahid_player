cmake_minimum_required(VERSION 3.21)
project(aoahid-player VERSION 0.1.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/EHsc /utf-8 /wd4244)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(AOAHID_PLAYER_BUILD_GUI "Build the Dear ImGui front end (aoahid_player_gui)" ON)
option(AOAHID_PLAYER_BUNDLE_RUNTIME
  "Install libaoahid, libusb and, with MSVC, the C++ runtime next to the executables" ON)

find_package(aoahid CONFIG REQUIRED)
find_package(Threads REQUIRED)

# Every executable lands in one folder next to csv/, in the build tree
# (out/<config>) and in the install tree (its root), matching the release
# archive. Recordings are saved to that csv/ folder.
set(AOAHID_PLAYER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/out/$<CONFIG>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${AOAHID_PLAYER_OUTPUT_DIR}")

include(cmake/AoahidPlayerApp.cmake)

add_subdirectory(src)
add_subdirectory(apps/aoa_touch)
add_subdirectory(apps/aoa_record)
if(AOAHID_PLAYER_BUILD_GUI)
  add_subdirectory(apps/aoahid_player_gui)
endif()

# Unit tests (tests/) are only built when this is the top-level project.
include(CTest)
if(BUILD_TESTING AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_subdirectory(tests)
endif()

# --- runtime component: what the release archive contains ------------------
install(DIRECTORY csv/ DESTINATION csv COMPONENT runtime)
install(FILES README.md LICENSE DESTINATION . COMPONENT runtime)
aoahid_player_install_runtime()

# --- development component: aoahid_player_core as a CMake package ----------
install(DIRECTORY include/ DESTINATION include COMPONENT development)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/aoahid_player_coreConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/aoahid_player_coreConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/aoahid_player_coreConfig.cmake"
  INSTALL_DESTINATION lib/cmake/aoahid_player_core
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/aoahid_player_coreConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/aoahid_player_coreConfigVersion.cmake"
  DESTINATION lib/cmake/aoahid_player_core
  COMPONENT development
)
install(EXPORT aoahid_player_coreTargets
  FILE aoahid_player_coreTargets.cmake
  NAMESPACE aoahid_player::
  DESTINATION lib/cmake/aoahid_player_core
  COMPONENT development
)
