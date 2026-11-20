# On the ESP32-S31, Bluetooth Classic audio left internal RAM so short that
# lwIP couldn't send (a WebREPL answer stopped partway). Bluedroid makes its
# semaphores and mutexes through its osi layer, and FreeRTOS puts every one
# in internal RAM. So the two files that make them,
# components/bt/common/osi/semaphore.c and mutex.c, are compiled with
# osi_psram.h forced in, which makes and frees them with the WithCaps calls
# in PSRAM. With an AudioSink started, that's about 6 KB.
#
# Only tasks use them: nothing in Bluedroid gives one from an interrupt, and
# while the flash cache is off no task runs, which is the condition ESP-IDF
# sets for these objects in PSRAM. Other chips keep them internal.
function(btclassic_osi_psram)
    if(NOT CONFIG_IDF_TARGET_ESP32S31 OR NOT CONFIG_SPIRAM OR NOT CONFIG_BT_BLUEDROID_ENABLED)
        return()
    endif()
    idf_build_get_property(idf_path IDF_PATH)
    set(files
        ${idf_path}/components/bt/common/osi/semaphore.c
        ${idf_path}/components/bt/common/osi/mutex.c)
    foreach(file ${files})
        if(NOT EXISTS ${file})
            message(FATAL_ERROR "btclassic: ${file} is gone, so Bluedroid's semaphores would stay "
                "in internal RAM (osi_psram.cmake)")
        endif()
    endforeach()
    if(NOT TARGET __idf_bt)
        message(FATAL_ERROR "btclassic: ESP-IDF's bt component isn't a target yet, so Bluedroid's "
            "semaphores can't move to PSRAM (osi_psram.cmake)")
    endif()
    set_property(SOURCE ${files} TARGET_DIRECTORY __idf_bt APPEND PROPERTY COMPILE_OPTIONS
        "-include" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/osi_psram.h")
endfunction()
