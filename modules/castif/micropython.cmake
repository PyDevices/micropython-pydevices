# castif: the cast as a FreeRTOS task on core 0 (h264enc's PPA + H.264 encode,
# tsmux's MPEG-TS, and RTP), ESP32-P4 only. The encoding and the muxing are
# h264enc's and tsmux's, called through their C APIs with weak symbols, so a
# firmware names all three to cast (`--modules castif,h264enc,tsmux`, or
# `all`); without either, Cast() says which is missing. Every other port and chip skips castif whole. Make ports never look
# here: there is no micropython.mk.
if(ESP_PLATFORM AND IDF_TARGET STREQUAL "esp32p4")
    add_library(usermod_castif INTERFACE)
    target_sources(usermod_castif INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/mod_castif.c)
    target_include_directories(usermod_castif INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src)
    target_link_libraries(usermod_castif INTERFACE idf::freertos idf::esp_timer idf::lwip)
    target_link_libraries(usermod INTERFACE usermod_castif)
endif()
