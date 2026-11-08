# btclassic: Bluetooth Classic (the radio, an audio sink with AVRCP, and a
# hands-free unit) on chips that have it. The port's main component already
# requires ESP-IDF's bt, so the C in esp-idf/btclassic compiles here as
# user-module source. sdkconfig.esp32 turns Bluedroid on in place of NimBLE
# (build_mp.py appends it; on a stock build, add it to your board's
# SDKCONFIG_DEFAULTS). ESP-IDF runs one Bluetooth host, so MicroPython's
# `bluetooth` (BLE) module goes: through MICROPY_DEF_BOARD, which the port
# compiles into every file and its qstr scan reads too. Every other port and
# chip skips the module whole.
if(ESP_PLATFORM AND IDF_TARGET STREQUAL "esp32")
    set(BTCLASSIC_IDF ${CMAKE_CURRENT_LIST_DIR}/esp-idf/btclassic)
    add_library(usermod_btclassic INTERFACE)
    target_sources(usermod_btclassic INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/src/mod_btclassic.c
        ${BTCLASSIC_IDF}/radio.c
        ${BTCLASSIC_IDF}/a2dp_sink.c
        ${BTCLASSIC_IDF}/avrcp_ct.c
        ${BTCLASSIC_IDF}/hfp_hf.c
        ${BTCLASSIC_IDF}/ring.c)
    target_include_directories(usermod_btclassic INTERFACE ${BTCLASSIC_IDF}/include ${BTCLASSIC_IDF})
    target_link_libraries(usermod INTERFACE usermod_btclassic)
    list(APPEND MICROPY_DEF_BOARD MICROPY_PY_BLUETOOTH=0)
endif()
