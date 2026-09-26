# Waveshare ESP32-P4-WIFI6-Touch-LCD-4B: a pre-rev-3 P4 with the C6 Wi-Fi companion, 32 MB flash, and the OV5647 camera driver configured in.
# The stock ESP32_GENERIC_P4 board, unchanged; what this board adds is in
# mpconfigvariant_PRE_REV3_C6_WIFI.cmake beside it.
include(${CMAKE_CURRENT_SOURCE_DIR}/boards/ESP32_GENERIC_P4/mpconfigboard.cmake)

# Two things upstream's esp32 port does with FROZEN_MANIFEST that this file
# straightens out, in the place upstream gives a board to do it:
#
# 1. A FROZEN_MANIFEST given relative to the port directory (how every other
#    port takes it) reaches makemanifest relative to the BUILD directory,
#    because the Makefile hands it to idf.py unchanged. Absolutize it against
#    the port directory, in both the variable and the copy the port took of
#    it before including this file, so the same spelling works everywhere.
# 2. With no FROZEN_MANIFEST at all, the port falls back to its own
#    boards/manifest.py, not this board's -- a board that wants its own
#    default says so here, which is upstream's own idiom.
# idf.py caches -D values: to return to this board's default after building
# with a preset, start from a clean build directory.
if(MICROPY_USER_FROZEN_MANIFEST)
    if(NOT IS_ABSOLUTE "${MICROPY_USER_FROZEN_MANIFEST}")
        get_filename_component(MICROPY_USER_FROZEN_MANIFEST "${MICROPY_USER_FROZEN_MANIFEST}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        set(MICROPY_FROZEN_MANIFEST "${MICROPY_USER_FROZEN_MANIFEST}")
    endif()
else()
    set(MICROPY_FROZEN_MANIFEST ${MICROPY_BOARD_DIR}/manifest.py)
endif()

# Spike: the esp_h264 component from the registry, dropped beside this board
# (spike/components/esp_h264, fetched by spike/fetch_esp_h264.sh). ESP-IDF's
# project() reads EXTRA_COMPONENT_DIRS as a plain variable, and this file is
# included before project() runs, so a board directory can add a component.
list(APPEND EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../components)
