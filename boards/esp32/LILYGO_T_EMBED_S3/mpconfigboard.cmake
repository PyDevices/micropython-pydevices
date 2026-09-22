# LilyGO T-Embed S3: the same silicon as the Waveshare S3 boards, plus flash auto-suspend, which cuts what a flash write costs the audio pump from ~45 ms to ~12 ms (cmods#32).
# The stock ESP32_GENERIC_S3 board, unchanged; what this board adds is in
# mpconfigvariant_SPIRAM_OCT.cmake beside it.
include(${CMAKE_CURRENT_SOURCE_DIR}/boards/ESP32_GENERIC_S3/mpconfigboard.cmake)

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
