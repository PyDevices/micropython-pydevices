# offheap CMake glue (see micropython.mk): esp32, where the bytes come from
# PSRAM through heap_caps (internal RAM on a board without PSRAM). rp2 gives
# its RAM to the GC heap and has no allocator beside it, so there this
# compiles to nothing and `import offheap` raises ImportError.
if(ESP_PLATFORM)

# --- which offheap this firmware was built from --------------------------
# Computed at build time from this repo's own git, never stored; "unknown" when
# there is no git (a tarball). Read on a target as offheap.__revision__.
execute_process(
    COMMAND git -C ${CMAKE_CURRENT_LIST_DIR} describe --always --dirty --abbrev=7
    OUTPUT_VARIABLE OFFHEAP_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT OFFHEAP_REVISION)
    set(OFFHEAP_REVISION "unknown")
endif()

add_library(usermod_offheap INTERFACE)
target_compile_definitions(usermod_offheap INTERFACE OFFHEAP_REVISION=\"${OFFHEAP_REVISION}\")
target_sources(usermod_offheap INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/mod_offheap.c)
target_link_libraries(usermod INTERFACE usermod_offheap)

endif()
