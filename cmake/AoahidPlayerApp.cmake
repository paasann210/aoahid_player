# Shared setup for the three executables and the runtime files they need.

# The libaoahid runtime files (and the libusb runtime its package bundles)
# that sit next to the imported library.
function(_aoahid_player_runtime_files out_var)
  set(files "")
  get_target_property(type aoahid::aoahid TYPE)
  if(type STREQUAL "SHARED_LIBRARY")
    set(location "")
    foreach(property IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION_RELWITHDEBINFO
                     IMPORTED_LOCATION_MINSIZEREL IMPORTED_LOCATION_NOCONFIG
                     IMPORTED_LOCATION IMPORTED_LOCATION_DEBUG)
      get_target_property(location aoahid::aoahid ${property})
      if(location)
        break()
      endif()
    endforeach()
    if(location)
      get_filename_component(directory "${location}" DIRECTORY)
      if(WIN32)
        file(GLOB files "${directory}/*.dll")
      else()
        file(GLOB files "${directory}/libaoahid.so.*" "${directory}/libusb-1.0.so.*")
      endif()
    endif()
  endif()
  set(${out_var} "${files}" PARENT_SCOPE)
endfunction()

_aoahid_player_runtime_files(AOAHID_PLAYER_RUNTIME_FILES)

# Build tree: example scripts, and on Windows the DLLs, next to the programs,
# so they run straight from out/<config>.
file(GLOB AOAHID_PLAYER_CSV_FILES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/csv/*.csv")
set(_stage_commands
  COMMAND ${CMAKE_COMMAND} -E make_directory "${AOAHID_PLAYER_OUTPUT_DIR}/csv")
if(AOAHID_PLAYER_CSV_FILES)
  list(APPEND _stage_commands
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AOAHID_PLAYER_CSV_FILES}
            "${AOAHID_PLAYER_OUTPUT_DIR}/csv")
endif()
if(WIN32 AND AOAHID_PLAYER_RUNTIME_FILES)
  list(APPEND _stage_commands
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AOAHID_PLAYER_RUNTIME_FILES}
            "${AOAHID_PLAYER_OUTPUT_DIR}")
endif()
add_custom_target(aoahid_player_stage ${_stage_commands} VERBATIM)

function(aoahid_player_add_app target)
  if(MSVC)
    target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/cmake/utf8.manifest")
  endif()
  if(UNIX AND NOT APPLE)
    # Installed programs find libaoahid and libusb in their own folder.
    set_target_properties(${target} PROPERTIES INSTALL_RPATH "$ORIGIN")
  endif()
  add_dependencies(${target} aoahid_player_stage)
  install(TARGETS ${target} RUNTIME DESTINATION . COMPONENT runtime)
endfunction()

function(aoahid_player_install_runtime)
  if(NOT AOAHID_PLAYER_BUNDLE_RUNTIME)
    return()
  endif()
  if(AOAHID_PLAYER_RUNTIME_FILES)
    # file(INSTALL) keeps the .so version symlinks as symlinks.
    install(FILES ${AOAHID_PLAYER_RUNTIME_FILES} DESTINATION . COMPONENT runtime)
  else()
    message(STATUS "aoahid-player: no shared libaoahid runtime found to bundle")
  endif()

  # libaoahid's own notices, including libusb's LGPL text and source archive.
  get_filename_component(_notices "${aoahid_DIR}/../../../share/doc/libaoahid" ABSOLUTE)
  if(EXISTS "${_notices}")
    install(DIRECTORY "${_notices}/" DESTINATION third-party COMPONENT runtime)
  endif()

  if(MSVC)
    # libaoahid.dll is built against the dynamic MSVC runtime, so its DLLs
    # are deployed app-locally instead of requiring the redistributable.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT runtime)
    set(CMAKE_INSTALL_UCRT_LIBRARIES OFF)
    include(InstallRequiredSystemLibraries)
  endif()
endfunction()
