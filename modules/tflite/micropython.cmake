# tflite: esp-vision's py_tflite.cpp (TensorFlow Lite Micro), compiled as it
# is from the pinned esp-vision checkout in modules/.esp-vision. ESP32-P4 and
# ESP32-S3 only, the chips esp-vision builds it for: every other port and
# chip skips it whole, so modules/all needs no exception. Make ports never
# look here: there is no micropython.mk.
#
# build_mp.py adds components/tflite_micro to EXTRA_COMPONENT_DIRS; its
# idf_component.yml has the component manager fetch espressif/esp-tflite-micro
# at the version esp-vision uses.
if(ESP_PLATFORM AND (IDF_TARGET STREQUAL "esp32p4" OR IDF_TARGET STREQUAL "esp32s3"))
    execute_process(
        COMMAND sh ${CMAKE_CURRENT_LIST_DIR}/../.esp-vision/fetch.sh
        OUTPUT_VARIABLE ESP_VISION_SRC
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _tflite_fetch_rc)
    if(NOT _tflite_fetch_rc EQUAL 0)
        message(FATAL_ERROR "tflite: fetching esp-vision failed (modules/.esp-vision/fetch.sh exit ${_tflite_fetch_rc})")
    endif()
    # py_tflite.cpp includes "ulab/code/ndarray.h": the directory holding ulab.
    get_filename_component(_tflite_ulab ${CMAKE_CURRENT_LIST_DIR}/../ulab REALPATH)
    get_filename_component(_tflite_ulab_parent ${_tflite_ulab} DIRECTORY)

    # MicroPython's QSTR scan preprocesses py_tflite.cpp with the module's own
    # include paths only, never a linked component's, so TensorFlow Lite
    # Micro's headers are named here (esp-vision's own cmake does the same).
    # The component's library may not exist yet when this runs; its directory
    # always does, and these are its INCLUDE_DIRS at the pinned 1.3.7.
    idf_component_get_property(_tflm_dir espressif__esp-tflite-micro COMPONENT_DIR)
    set(_tflm_inc ${_tflm_dir} ${_tflm_dir}/third_party/gemmlowp
        ${_tflm_dir}/third_party/flatbuffers/include ${_tflm_dir}/third_party/ruy
        ${_tflm_dir}/third_party/kissfft)

    add_library(usermod_tflite INTERFACE)
    target_sources(usermod_tflite INTERFACE ${ESP_VISION_SRC}/modules/py_tflite.cpp)
    target_include_directories(usermod_tflite INTERFACE ${_tflite_ulab_parent} ${_tflm_inc})
    target_link_libraries(usermod_tflite INTERFACE idf::espressif__esp-tflite-micro idf::esp_hw_support)
    target_link_libraries(usermod INTERFACE usermod_tflite)
endif()
