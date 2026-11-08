# btaudio: a Bluetooth speaker's receiving half (A2DP sink and AVRCP) on
# chips with Bluetooth Classic. The port's main component already requires
# ESP-IDF's bt, so the C in esp-idf/btaudio compiles here as user-module
# source. sdkconfig.esp32 turns Bluedroid on in place of NimBLE (build_mp.py
# appends it; on a stock build, add it to your board's SDKCONFIG_DEFAULTS).
# ESP-IDF runs one Bluetooth host, so MicroPython's `bluetooth` (BLE) module
# goes: through MICROPY_DEF_BOARD, which the port compiles into every file and
# its qstr scan reads too. Every other port and chip skips the module whole.
if(ESP_PLATFORM AND IDF_TARGET STREQUAL "esp32")
    add_library(usermod_btaudio INTERFACE)
    target_sources(usermod_btaudio INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/src/mod_btaudio.c
        ${CMAKE_CURRENT_LIST_DIR}/esp-idf/btaudio/btaudio.c)
    target_include_directories(usermod_btaudio INTERFACE ${CMAKE_CURRENT_LIST_DIR}/esp-idf/btaudio/include)
    target_link_libraries(usermod INTERFACE usermod_btaudio)
    list(APPEND MICROPY_DEF_BOARD MICROPY_PY_BLUETOOTH=0)
endif()
