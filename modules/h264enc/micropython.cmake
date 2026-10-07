# h264enc: the ESP32-P4's hardware H.264 encoder, fed RGB565 through the PPA.
# build_mp.py adds components/h264enc_esp_h264 to EXTRA_COMPONENT_DIRS; that
# component's idf_component.yml has the component manager fetch Espressif's
# esp_h264, which then exists as espressif__esp_h264. The usermod sources are
# also compiled in the executable's own target, which sees only what is linked
# here, so the headers' components are named explicitly. Every other port and
# chip skips it whole, so `all` needs no exception for it; make ports never
# look here (there is no micropython.mk).
if(ESP_PLATFORM AND IDF_TARGET STREQUAL "esp32p4")
    add_library(usermod_h264enc INTERFACE)
    target_sources(usermod_h264enc INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/mod_h264enc.c)
    target_include_directories(usermod_h264enc INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src)
    target_link_libraries(usermod_h264enc INTERFACE idf::espressif__esp_h264
        idf::esp_timer)
    target_link_libraries(usermod INTERFACE usermod_h264enc)
endif()
