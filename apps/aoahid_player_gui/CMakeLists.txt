# Dear ImGui front end. Dear ImGui and GLFW are fetched at pinned, hashed
# releases and linked statically, so the program needs nothing beyond the
# system's OpenGL driver and the libaoahid runtime next to it.
#
# To build offline, point FETCHCONTENT_SOURCE_DIR_IMGUI / _GLFW at local
# copies of the same releases.
include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW) # extracted files get the extraction time
endif()

FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9.tar.gz
  URL_HASH SHA256=af97ed649182c39314320514a672b82008ab462b9293fe23d37b30bfa5d05519
)
FetchContent_Declare(glfw
  URL https://github.com/glfw/glfw/releases/download/3.5.1/glfw-3.5.1.zip
  URL_HASH SHA256=ea79bc5feffc254c87291980c2d0bce9acebb68c4983b79f961dcd2cb8a611a0
)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
# Dear ImGui has no CMake project, so this only downloads its sources.
FetchContent_MakeAvailable(glfw imgui)
set(imgui_dir "${imgui_SOURCE_DIR}")

add_library(aoahid_player_imgui STATIC
  "${imgui_dir}/imgui.cpp"
  "${imgui_dir}/imgui_draw.cpp"
  "${imgui_dir}/imgui_tables.cpp"
  "${imgui_dir}/imgui_widgets.cpp"
  "${imgui_dir}/misc/cpp/imgui_stdlib.cpp"
  "${imgui_dir}/backends/imgui_impl_glfw.cpp"
  "${imgui_dir}/backends/imgui_impl_opengl3.cpp"
)
target_include_directories(aoahid_player_imgui PUBLIC "${imgui_dir}" "${imgui_dir}/backends")
target_compile_definitions(aoahid_player_imgui PUBLIC
  IMGUI_DISABLE_OBSOLETE_FUNCTIONS
  IMGUI_DISABLE_DEMO_WINDOWS
  IMGUI_DISABLE_DEBUG_TOOLS
  GLFW_INCLUDE_NONE
)
target_link_libraries(aoahid_player_imgui PUBLIC glfw ${CMAKE_DL_LIBS})

# The UI font (Roboto Medium, Apache-2.0, shipped with Dear ImGui) is
# compiled into the program.
set(font_source "${imgui_dir}/misc/fonts/Roboto-Medium.ttf")
set(font_cpp "${CMAKE_CURRENT_BINARY_DIR}/embedded_font.cpp")
add_custom_command(
  OUTPUT "${font_cpp}"
  COMMAND "${CMAKE_COMMAND}" "-DINPUT=${font_source}" "-DOUTPUT=${font_cpp}"
          -DSYMBOL=aoahid_player_font -P "${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake"
  DEPENDS "${font_source}" "${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake"
  COMMENT "Embedding Roboto-Medium.ttf"
  VERBATIM
)

add_executable(aoahid_player_gui WIN32
  main.cpp
  app.cpp
  app_live.cpp
  activity_log.cpp
  engine.cpp
  keymap.cpp
  live_image.cpp
  playlist.cpp
  settings.cpp
  theme.cpp
  widgets.cpp
  "${font_cpp}"
)
target_include_directories(aoahid_player_gui PRIVATE
  "${PROJECT_SOURCE_DIR}/third_party/stb")
target_link_libraries(aoahid_player_gui PRIVATE aoahid_player_core aoahid_player_imgui)
target_compile_definitions(aoahid_player_gui PRIVATE
  AOAHID_PLAYER_VERSION="${PROJECT_VERSION}")
if(MSVC)
  target_compile_options(aoahid_player_gui PRIVATE /W4)
  # A GUI subsystem program that still starts at main().
  target_link_options(aoahid_player_gui PRIVATE /ENTRY:mainCRTStartup)
else()
  target_compile_options(aoahid_player_gui PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()
aoahid_player_add_app(aoahid_player_gui)

install(FILES "${imgui_dir}/LICENSE.txt"
  DESTINATION third-party RENAME DearImGui-LICENSE.txt COMPONENT runtime)
install(FILES "${glfw_SOURCE_DIR}/LICENSE.md"
  DESTINATION third-party RENAME GLFW-LICENSE.md COMPONENT runtime)
install(FILES "${PROJECT_SOURCE_DIR}/third_party/licenses/Roboto-LICENSE.txt"
  DESTINATION third-party COMPONENT runtime)
install(FILES "${PROJECT_SOURCE_DIR}/third_party/licenses/stb_image-LICENSE.txt"
  DESTINATION third-party COMPONENT runtime)
