# MicroPython CMake glue for the audio pump's platform driver (esp32).
# For Make-based ports (unix, windows), see micropython.mk in this dir.
#
# The engine is not here any more: the pull loop, the ring, the events, the tap
# and the lock all live in audiodsp and are built by audiodsp's own glue. This
# builds the one file that knows what a thread, a mutex, a clock and a sink
# are. AUDIOPUMP_AUDIODSP_DIR points at the audiodsp checkout; without it,
# audiodsp is looked for among the build's user C modules, then beside this
# directory.

set(AUDIOPUMP_MOD_DIR ${CMAKE_CURRENT_LIST_DIR})

# esp32 is the only CMake port this driver has: _audioif.c includes ESP-IDF's
# driver/i2s_std.h, and a multi-module build links every usermod into every port.
# Without this guard an rp2 (or any non-IDF CMake) build fails at the last
# file with "driver/i2s_std.h: No such file" whether or not it wanted the
# pump (#15). cameraif carries the same guard for the same reason.
if(NOT ESP_PLATFORM)
    message(STATUS "audioif: skipped -- the platform driver is esp32-only on CMake ports")
    return()
endif()

# audiodsp is found first among the modules this build was given (each entry
# of USER_C_MODULES, which by now includes the manifests' c_module()s, is a
# module directory or its micropython.cmake), so it can live anywhere: a git
# worktree, a vendored copy. The sibling layout and CMOD_DIR come after.
if(NOT DEFINED AUDIOPUMP_AUDIODSP_DIR)
    foreach(_audioif_mod ${USER_C_MODULES})
        if(NOT IS_DIRECTORY ${_audioif_mod})
            get_filename_component(_audioif_mod ${_audioif_mod} DIRECTORY)
        endif()
        if(EXISTS ${_audioif_mod}/src/shared/audiodsp_port.h)
            set(AUDIOPUMP_AUDIODSP_DIR ${_audioif_mod})
            break()
        endif()
    endforeach()
endif()
if(NOT DEFINED AUDIOPUMP_AUDIODSP_DIR)
    if(EXISTS ${AUDIOPUMP_MOD_DIR}/../audiodsp/src/shared/audiodsp_port.h)
        set(AUDIOPUMP_AUDIODSP_DIR ${AUDIOPUMP_MOD_DIR}/../audiodsp)
    elseif(DEFINED CMOD_DIR AND EXISTS ${CMOD_DIR}/audiodsp/src/shared/audiodsp_port.h)
        set(AUDIOPUMP_AUDIODSP_DIR ${CMOD_DIR}/audiodsp)
    endif()
endif()

# Without audiodsp this module has nothing to drive, and a firmware that quietly
# left it out would fall back to machine.I2S with nothing failing to say why
# (#28). So the build stops here, naming the fix.
if(NOT DEFINED AUDIOPUMP_AUDIODSP_DIR)
    message(FATAL_ERROR "audioif: audiodsp not found. audioif drives audiodsp's "
        "audiopump and needs its headers: add audiodsp to the build's modules "
        "(its manifest, or its directory in USER_C_MODULES), or set "
        "AUDIOPUMP_AUDIODSP_DIR to an audiodsp checkout.")
elseif(NOT EXISTS ${AUDIOPUMP_AUDIODSP_DIR}/src/shared/audiodsp_port.h)
    message(FATAL_ERROR "audioif: AUDIOPUMP_AUDIODSP_DIR is ${AUDIOPUMP_AUDIODSP_DIR}, "
        "which has no src/shared/audiodsp_port.h; point it at an audiodsp checkout.")
endif()
message(STATUS "audioif: audiodsp at ${AUDIOPUMP_AUDIODSP_DIR}")

add_library(usermod_audiopump INTERFACE)

target_sources(usermod_audiopump INTERFACE
    ${AUDIOPUMP_MOD_DIR}/_audioif.c
    ${AUDIOPUMP_MOD_DIR}/audiobusio.c
)

target_include_directories(usermod_audiopump INTERFACE
    ${AUDIOPUMP_AUDIODSP_DIR}/src
)

# The IDF does NOT hand ESP_PLATFORM to user C modules -- it is a CMake
# variable here and a compile definition only inside IDF components. Without
# this the source compiles its POSIX branch instead, and it links, because
# ESP-IDF's newlib has pthread.h: you get an unpinned pump on a default
# pthread stack and no I2S sink, with nothing failing to say so. displayif
# hits the same thing (src/ports/esp32/micropython.cmake:24).
#
# What is new is that getting it wrong is now VISIBLE: audiopump.driver() says
# "pthread" on a board that should say "esp32".
target_compile_definitions(usermod_audiopump INTERFACE AUDIOIF_DRIVER_ESP32=1)

target_link_libraries(usermod INTERFACE usermod_audiopump)

# --- which audioif this firmware was built from ----------------------
# Computed at build time from this repo's own git, never stored; "unknown" when
# there is no git (a tarball). Read on a target as <module>.__revision__.
execute_process(
    COMMAND git -C ${AUDIOPUMP_MOD_DIR} describe --always --dirty --abbrev=7
    OUTPUT_VARIABLE AUDIOIF_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT AUDIOIF_REVISION)
    set(AUDIOIF_REVISION "unknown")
endif()
target_compile_definitions(usermod_audiopump INTERFACE AUDIOIF_REVISION=\"${AUDIOIF_REVISION}\")
