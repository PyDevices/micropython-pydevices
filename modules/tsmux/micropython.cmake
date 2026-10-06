# tsmux CMake glue (see micropython.mk).
add_library(usermod_tsmux INTERFACE)
target_sources(usermod_tsmux INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/mod_tsmux.c
    ${CMAKE_CURRENT_LIST_DIR}/src/tsmux_core.c
)
target_include_directories(usermod_tsmux INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src)
target_link_libraries(usermod INTERFACE usermod_tsmux)
