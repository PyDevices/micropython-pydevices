# vision: esp-vision's image stack (modules/.esp-vision), compiled as it is.
# SPIKE. ESP32-P4 and ESP32-S3 only; every other port and chip skips it.
# Make ports never look here: there is no micropython.mk.
#
# Ours: components/vision_deps (the ESP-IDF components it needs) and
# src/vision_preview_none.c (preview without esp-vision's USB multiplexer).
# Everything else is esp-vision's file as it is, imlib (OpenMV's, MIT)
# included: compiled here, in MicroPython's own target, because it includes
# MicroPython's generated headers, and esp-vision's components/imlib
# CMakeLists finds its per-board headers by a board name we don't have. The
# per-board headers come from one of esp-vision's own boards per chip.
if(ESP_PLATFORM AND (IDF_TARGET STREQUAL "esp32p4" OR IDF_TARGET STREQUAL "esp32s3"))
    execute_process(
        COMMAND sh ${CMAKE_CURRENT_LIST_DIR}/../.esp-vision/fetch.sh
        OUTPUT_VARIABLE EV
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "vision: fetching esp-vision failed (modules/.esp-vision/fetch.sh exit ${_rc})")
    endif()
    if(IDF_TARGET STREQUAL "esp32p4")
        set(EV_BOARD ${EV}/boards/ESP32_P4X_FUNCTION_EV_BOARD)
    else()
        set(EV_BOARD ${EV}/boards/ESP32_S3_EYE)
    endif()
    get_filename_component(_ulab ${CMAKE_CURRENT_LIST_DIR}/../ulab REALPATH)
    get_filename_component(_ulab_parent ${_ulab} DIRECTORY)

    # MicroPython's QSTR scan sees only the module's own include paths, never
    # a linked component's: every header directory these sources reach.
    idf_component_get_property(_jpegdrv esp_driver_jpeg COMPONENT_DIR)
    idf_component_get_property(_newjpeg espressif__esp_new_jpeg COMPONENT_DIR)
    set(_inc ${EV}/modules ${EV}/platform ${EV_BOARD}
        ${EV}/components/imlib/upstream ${EV}/components/imlib/include
        ${_ulab_parent} ${_jpegdrv}/include ${_newjpeg}/include)

    set(_src ${EV}/modules/py_image.c ${EV}/modules/py_imageio.c ${EV}/modules/py_helper.c
        ${EV}/platform/jpeg.c ${CMAKE_CURRENT_LIST_DIR}/src/vision_preview_none.c)
    # imlib, esp-vision's components/imlib list, with its options.
    set(_up ${EV}/components/imlib/upstream)
    set(_compat ${EV}/components/imlib/compat)
    set(_imlib
        ${_up}/collections.c ${_up}/apriltag.c ${_up}/binary.c ${_up}/bayer.c
        ${_up}/blob.c ${_up}/bmp.c ${_up}/draw.c ${_up}/edge.c ${_up}/edl.c
        ${_up}/eye.c ${_up}/filter.c ${_up}/fmath.c ${_up}/font.c ${_up}/fsort.c
        ${_up}/hough.c ${_up}/imlib.c ${_up}/integral.c ${_up}/jpegd.c ${_up}/jpege.c
        ${_up}/line.c ${_up}/mathop.c ${_up}/png.c ${_up}/point.c ${_up}/ppm.c
        ${_up}/qrcode.c ${_up}/rectangle.c ${_up}/stats.c ${_up}/template.c ${_up}/yuv.c
        ${_compat}/array.c ${_compat}/fb_alloc.c ${_compat}/file_utils.c
        ${_compat}/framebuffer_compat.c ${_compat}/lab_table.c ${_compat}/sincos_table.c
        ${_compat}/umm_malloc.c ${_compat}/unaligned_memcpy.c)
    set_source_files_properties(${_imlib} PROPERTIES COMPILE_OPTIONS "-O3;-Wno-error")
    list(APPEND _src ${_imlib})
    set(_libs idf::esp_driver_jpeg idf::espressif__esp_new_jpeg)
    if(IDF_TARGET STREQUAL "esp32p4")
        idf_component_get_property(_h264 espressif__esp_h264 COMPONENT_DIR)
        list(APPEND _src ${EV}/platform/h264.c ${EV}/modules/py_h264.c)
        list(APPEND _inc ${_h264}/interface/include ${_h264}/sw/include ${_h264}/port/include ${_h264}/hw/include)
        list(APPEND _libs idf::espressif__esp_h264)
    endif()

    add_library(usermod_vision INTERFACE)
    target_sources(usermod_vision INTERFACE ${_src})
    target_include_directories(usermod_vision INTERFACE ${_inc})
    target_compile_definitions(usermod_vision INTERFACE CMSIS_MCU_H="cmsis_compiler.h" OMV_NO_GPL=1)
    # ...and the QSTR scan takes definitions only from this list.
    list(APPEND MICROPY_CPP_DEF_EXTRA "CMSIS_MCU_H=\"cmsis_compiler.h\"" OMV_NO_GPL=1)
    target_link_libraries(usermod_vision INTERFACE ${_libs})
    target_link_libraries(usermod INTERFACE usermod_vision)
    # image's feature-flagged names (esp-vision's own list).
    list(APPEND MICROPY_QSTRDEFS_PORT ${EV}/modules/qstrdefs_esp_vision.h)
endif()
