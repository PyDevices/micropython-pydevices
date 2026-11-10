# mdns CMake glue (see micropython.mk): ESP-IDF's mDNS component on esp32,
# which the port already depends on, and lwIP's mDNS on rp2 boards with a
# CYW43 radio. Elsewhere it compiles to nothing.
# --- which mdns this firmware was built from ---------------------------
# Computed at build time from this repo's own git, never stored; "unknown" when
# there is no git (a tarball). Read on a target as <module>.__revision__.
execute_process(
    COMMAND git -C ${CMAKE_CURRENT_LIST_DIR} describe --always --dirty --abbrev=7
    OUTPUT_VARIABLE MDNS_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT MDNS_REVISION)
    set(MDNS_REVISION "unknown")
endif()

add_library(usermod_mdns INTERFACE)
target_compile_definitions(usermod_mdns INTERFACE MDNS_REVISION=\"${MDNS_REVISION}\")
target_sources(usermod_mdns INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/mod_mdns.c
    ${CMAKE_CURRENT_LIST_DIR}/src/mdns_esp32.c
    ${CMAKE_CURRENT_LIST_DIR}/src/mdns_lwip.c
)
target_include_directories(usermod_mdns INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src)
target_link_libraries(usermod INTERFACE usermod_mdns)
