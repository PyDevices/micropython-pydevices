# picotts CMake glue (see micropython.mk for what the settings mean).

set(PICOTTS_DIR ${CMAKE_CURRENT_LIST_DIR})
set(PICOTTS_SVOX ${PICOTTS_DIR}/third_party/svox)

file(GLOB PICOTTS_SVOX_SRC ${PICOTTS_SVOX}/*.c)

add_library(usermod_picotts INTERFACE)
target_sources(usermod_picotts INTERFACE
    ${PICOTTS_DIR}/src/modpicotts.c
    ${PICOTTS_SVOX_SRC}
)
target_include_directories(usermod_picotts INTERFACE ${PICOTTS_SVOX})
target_compile_definitions(usermod_picotts INTERFACE PICO_PLATFORM=PICO_CIRCUITPY)
target_link_libraries(usermod INTERFACE usermod_picotts)
