# h264enc: the ESP32-P4 hardware H.264 encoder (espressif/esp_h264) as a native module.
add_library(usermod_h264enc INTERFACE)
target_sources(usermod_h264enc INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/mod_h264enc.c)
target_include_directories(usermod_h264enc INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src)
if(ESP_PLATFORM AND IDF_TARGET STREQUAL "esp32p4")
    target_compile_definitions(usermod_h264enc INTERFACE H264ENC_HAVE_HW=1)
    target_link_libraries(usermod_h264enc INTERFACE idf::esp_h264 idf::esp_driver_ppa)
endif()
target_link_libraries(usermod INTERFACE usermod_h264enc)
