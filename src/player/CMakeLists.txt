add_executable(pelican_player main.cpp)
set_target_properties(pelican_player PROPERTIES CXX_STANDARD 20)

target_link_libraries(pelican_player pelican_core)

file(GLOB PELICAN_PLAYER_RUNTIME_JSON CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/resources/*.json
)
foreach(RESOURCE_FILE IN LISTS PELICAN_PLAYER_RUNTIME_JSON)
  add_custom_command(
    TARGET pelican_player POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${RESOURCE_FILE}
      $<TARGET_FILE_DIR:pelican_player>
    VERBATIM
  )
endforeach()
