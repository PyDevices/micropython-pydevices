# pngio CMake glue (see micropython.mk).
set(PNGIO_DIR ${CMAKE_CURRENT_LIST_DIR}/src)

# --- which pngio this firmware was built from ---------------------------
# Computed at build time from this repo's own git, never stored; "unknown" when
# there is no git (a tarball). Read on a target as <module>.__revision__.
execute_process(
    COMMAND git -C ${CMAKE_CURRENT_LIST_DIR} describe --always --dirty --abbrev=7
    OUTPUT_VARIABLE PNGIO_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT PNGIO_REVISION)
    set(PNGIO_REVISION "unknown")
endif()

add_library(usermod_pngio INTERFACE)
target_compile_definitions(usermod_pngio INTERFACE PNGIO_REVISION=\"${PNGIO_REVISION}\")
target_include_directories(usermod_pngio INTERFACE ${PNGIO_DIR})
target_sources(usermod_pngio INTERFACE
    ${PNGIO_DIR}/mod_pngio.c
    ${PNGIO_DIR}/pngio_core.c
)
target_link_libraries(usermod INTERFACE usermod_pngio)
