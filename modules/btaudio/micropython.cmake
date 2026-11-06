# btaudio: a Bluetooth speaker's receiving half (A2DP sink and AVRCP) on
# chips with Bluetooth Classic. build_mp.py adds components/btaudio to
# EXTRA_COMPONENT_DIRS, appends sdkconfig.<chip>, which turns Bluedroid on in
# place of NimBLE, and includes mpconfigboard.<chip>.h, which turns
# MicroPython's `bluetooth` (BLE) module off: ESP-IDF runs one Bluetooth host.
# Every other port and chip skips the module whole.
if(ESP_PLATFORM AND IDF_TARGET STREQUAL "esp32")
    add_library(usermod_btaudio INTERFACE)
    target_sources(usermod_btaudio INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/mod_btaudio.c)
    target_include_directories(usermod_btaudio INTERFACE ${CMAKE_CURRENT_LIST_DIR}/components/btaudio/include)
    target_link_libraries(usermod_btaudio INTERFACE idf::btaudio)
    target_link_libraries(usermod INTERFACE usermod_btaudio)
endif()
