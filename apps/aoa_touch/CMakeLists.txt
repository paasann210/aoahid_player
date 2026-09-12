add_executable(aoa_touch
  main.cpp
  cli_options.cpp
)
target_link_libraries(aoa_touch PRIVATE aoahid_player_core)
aoahid_player_add_app(aoa_touch)
