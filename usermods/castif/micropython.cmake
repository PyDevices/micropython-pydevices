# castif: the cast as a FreeRTOS task on core 0 (PPA + esp_h264 + MPEG-TS mux
# + RTP), ESP32-P4 only. The board adds castif_h264/ to EXTRA_COMPONENT_DIRS;
# that component's idf_component.yml has the component manager fetch
# Espressif's esp_h264, which then exists as espressif__esp_h264. The usermod
# sources are also compiled in the executable's own target, which sees only
# what is linked here, so the headers' components are named explicitly.
add_library(usermod_castif INTERFACE)
target_sources(usermod_castif INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/mod_castif.c)
target_include_directories(usermod_castif INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src)
if(ESP_PLATFORM AND IDF_TARGET STREQUAL "esp32p4")
    target_link_libraries(usermod_castif INTERFACE idf::espressif__esp_h264 idf::esp_driver_ppa
        idf::freertos idf::esp_timer idf::lwip)
endif()
target_link_libraries(usermod INTERFACE usermod_castif)
