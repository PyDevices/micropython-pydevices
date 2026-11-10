# tsmux CMake glue (see micropython.mk).
# --- which tsmux this firmware was built from ---------------------------
# Computed at build time from this repo's own git, never stored; "unknown" when
# there is no git (a tarball). Read on a target as <module>.__revision__.
execute_process(
    COMMAND git -C ${CMAKE_CURRENT_LIST_DIR} describe --always --dirty --abbrev=7
    OUTPUT_VARIABLE TSMUX_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT TSMUX_REVISION)
    set(TSMUX_REVISION "unknown")
endif()

add_library(usermod_tsmux INTERFACE)
target_compile_definitions(usermod_tsmux INTERFACE TSMUX_REVISION=\"${TSMUX_REVISION}\")
target_sources(usermod_tsmux INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/mod_tsmux.c
    ${CMAKE_CURRENT_LIST_DIR}/src/tsmux_core.c
)
target_include_directories(usermod_tsmux INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src)
target_link_libraries(usermod INTERFACE usermod_tsmux)
