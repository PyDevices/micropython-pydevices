# jpegio CMake glue (see micropython.mk for what the variables mean).

set(JPEGIO_DIR ${CMAKE_CURRENT_LIST_DIR}/src)
set(JPEGIO_THIRD_PARTY ${CMAKE_CURRENT_LIST_DIR}/third_party)

add_library(usermod_jpegio INTERFACE)
target_include_directories(usermod_jpegio INTERFACE
    ${JPEGIO_THIRD_PARTY}/tjpgd
    ${JPEGIO_THIRD_PARTY}/jpegenc
)
target_link_libraries(usermod INTERFACE usermod_jpegio)

# The vendored TJpgDec is the firmware's only one (LVGL's is off by config).
# jpegenc/ is the software encoder every port gets; jpegio_hw.c is the P4's
# JPEG engine, and two stubs everywhere else.
target_sources(usermod_jpegio INTERFACE
    ${JPEGIO_DIR}/jpegio.c
    ${JPEGIO_DIR}/jpegio_enc.c
    ${JPEGIO_DIR}/jpegio_hw.c
    ${JPEGIO_THIRD_PARTY}/jpegenc/jpegenc.c
    ${JPEGIO_THIRD_PARTY}/tjpgd/tjpgd.c
)

# The ESP32-P4 encodes and decodes on its JPEG engine (esp_driver_jpeg, which
# main does not require on its own, so it is named here).
if(ESP_PLATFORM AND IDF_TARGET STREQUAL "esp32p4")
    target_compile_definitions(usermod_jpegio INTERFACE JPEGIO_HW=1)
    target_link_libraries(usermod_jpegio INTERFACE idf::esp_driver_jpeg)
endif()

# The LVGL image decoder (lvgl_decoder.c) is built only when LVGL is in this
# build: when USER_C_MODULES names omnipython-lvgl's ports/micropython (its
# repository root, two levels up, holds the generated binding), decided the
# way micropython.mk decides it: what is on disk beside this module says
# nothing about what this build selected, and jpegio has to build without
# LVGL. Override with -DJPEGIO_LVGL=ON/OFF. The decoder includes "lvgl/lvgl.h"
# and "lvgl/src/...", which resolve through the root that port itself puts on
# the include path, so nothing here needs to know where the checkout is.
if(NOT DEFINED JPEGIO_LVGL)
    set(JPEGIO_LVGL OFF)
    foreach(_jpegio_mod ${USER_C_MODULES})
        if(NOT IS_DIRECTORY "${_jpegio_mod}")
            get_filename_component(_jpegio_mod "${_jpegio_mod}" DIRECTORY)
        endif()
        if(EXISTS "${_jpegio_mod}/../../generated/lvgl_micropython.c")
            set(JPEGIO_LVGL ON)
        endif()
    endforeach()
endif()

if(JPEGIO_LVGL)
    target_compile_definitions(usermod_jpegio INTERFACE JPEGIO_LVGL_DECODER=1)
    # No qstrs of its own (the Python-visible names live in jpegio.c).
    target_sources(usermod_jpegio INTERFACE
        ${JPEGIO_DIR}/lvgl_decoder.c
    )
endif()
