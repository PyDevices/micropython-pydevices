# castif: the cast as a FreeRTOS task on core 0 (h264enc's PPA + H.264 encode,
# tsmux's MPEG-TS, and RTP), ESP32-P4 only. The encoding and the muxing are
# h264enc's and tsmux's, called through their C APIs with weak symbols, so a
# firmware names all three to cast (`--modules castif,h264enc,tsmux`, or
# `all`); without either, Cast() says which is missing. Every other port and
# chip skips castif whole. CircuitPython builds it through micropython.mk.
if(ESP_PLATFORM AND IDF_TARGET STREQUAL "esp32p4")
    # --- which castif this firmware was built from ---------------------------
    # Computed at build time from this repo's own git, never stored; "unknown" when
    # there is no git (a tarball). Read on a target as <module>.__revision__.
    execute_process(
        COMMAND git -C ${CMAKE_CURRENT_LIST_DIR} describe --always --dirty --abbrev=7
        OUTPUT_VARIABLE CASTIF_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT CASTIF_REVISION)
        set(CASTIF_REVISION "unknown")
    endif()
    add_library(usermod_castif INTERFACE)
    target_compile_definitions(usermod_castif INTERFACE CASTIF_REVISION=\"${CASTIF_REVISION}\")
    target_sources(usermod_castif INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/src/mod_castif.c
        ${CMAKE_CURRENT_LIST_DIR}/src/castif_hls.c
    )
    target_include_directories(usermod_castif INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src)
    target_link_libraries(usermod_castif INTERFACE idf::freertos idf::esp_timer idf::lwip)
    target_link_libraries(usermod INTERFACE usermod_castif)
endif()
