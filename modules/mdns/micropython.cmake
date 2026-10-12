# mdns CMake glue (see micropython.mk): ESP-IDF's mDNS component on esp32,
# which the port already depends on, and lwIP's mDNS on rp2 boards with a
# CYW43 radio. Elsewhere it compiles to nothing.
add_library(usermod_mdns INTERFACE)
target_sources(usermod_mdns INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/mod_mdns.c
    ${CMAKE_CURRENT_LIST_DIR}/src/mdns_esp32.c
    ${CMAKE_CURRENT_LIST_DIR}/src/mdns_lwip.c
)
target_include_directories(usermod_mdns INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src)
target_link_libraries(usermod INTERFACE usermod_mdns)
