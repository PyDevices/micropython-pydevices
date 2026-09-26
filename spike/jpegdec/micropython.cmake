# jpegdec: the ESP32-P4 hardware JPEG decoder (esp_driver_jpeg) as a native module.
add_library(usermod_jpegdec INTERFACE)
target_sources(usermod_jpegdec INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/mod_jpegdec.c)
target_include_directories(usermod_jpegdec INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src)
if(ESP_PLATFORM AND IDF_TARGET STREQUAL "esp32p4")
    target_link_libraries(usermod_jpegdec INTERFACE idf::esp_driver_jpeg)
endif()
target_link_libraries(usermod INTERFACE usermod_jpegdec)
