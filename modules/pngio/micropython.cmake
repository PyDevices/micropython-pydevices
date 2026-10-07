# pngio CMake glue (see micropython.mk).
set(PNGIO_DIR ${CMAKE_CURRENT_LIST_DIR}/src)

add_library(usermod_pngio INTERFACE)
target_include_directories(usermod_pngio INTERFACE ${PNGIO_DIR})
target_sources(usermod_pngio INTERFACE
    ${PNGIO_DIR}/mod_pngio.c
    ${PNGIO_DIR}/pngio_core.c
)
target_link_libraries(usermod INTERFACE usermod_pngio)
