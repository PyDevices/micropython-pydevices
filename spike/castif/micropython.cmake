# castif: the cast as a FreeRTOS task on core 0 (PPA + esp_h264 + TS mux + RTP).
add_library(usermod_castif INTERFACE)
target_sources(usermod_castif INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/mod_castif.c)
target_include_directories(usermod_castif INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src)
if(ESP_PLATFORM AND IDF_TARGET STREQUAL "esp32p4")
    target_link_libraries(usermod_castif INTERFACE idf::esp_h264 idf::esp_driver_ppa idf::lwip)
endif()
target_link_libraries(usermod INTERFACE usermod_castif)
