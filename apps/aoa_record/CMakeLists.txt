add_executable(aoa_record
  main.cpp
  cli_options.cpp
)
target_link_libraries(aoa_record PRIVATE aoahid_player_core)
aoahid_player_add_app(aoa_record)
