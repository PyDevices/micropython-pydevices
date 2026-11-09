# btclassic: Bluetooth Classic (the radio, an audio sink with AVRCP, and a
# hands-free unit) on chips that have it. The port's main component already
# requires ESP-IDF's bt, so the C in esp-idf/btclassic compiles here as
# user-module source. sdkconfig.esp32 turns Bluedroid on in place of NimBLE
# (build_mp.py appends it; on a stock build, add it to your board's
# SDKCONFIG_DEFAULTS). ESP-IDF runs one Bluetooth host, so MicroPython's
# `bluetooth` (BLE) module moves onto Bluedroid too: src/modbluetooth_bluedroid.c
# is its backend in NimBLE's place, chosen with MICROPY_BLUETOOTH_BLUEDROID
# through MICROPY_DEF_BOARD, which the port compiles into every file and its
# qstr scan reads too (patches/ teaches the port that switch). Every other
# port and chip skips the module whole.
if(ESP_PLATFORM AND IDF_TARGET STREQUAL "esp32")
    set(BTCLASSIC_IDF ${CMAKE_CURRENT_LIST_DIR}/esp-idf/btclassic)
    add_library(usermod_btclassic INTERFACE)
    target_sources(usermod_btclassic INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/src/mod_btclassic.c
        ${CMAKE_CURRENT_LIST_DIR}/src/mod_btclassic_source.c
        ${CMAKE_CURRENT_LIST_DIR}/src/mod_btclassic_serial.c
        ${CMAKE_CURRENT_LIST_DIR}/src/mod_btclassic_hid.c
        ${CMAKE_CURRENT_LIST_DIR}/src/mod_btclassic_phone.c
        ${CMAKE_CURRENT_LIST_DIR}/src/mod_btclassic_tether.c
        ${CMAKE_CURRENT_LIST_DIR}/src/modbluetooth_bluedroid.c
        ${BTCLASSIC_IDF}/radio.c
        ${BTCLASSIC_IDF}/a2dp_sink.c
        ${BTCLASSIC_IDF}/avrcp_ct.c
        ${BTCLASSIC_IDF}/hfp_hf.c
        ${BTCLASSIC_IDF}/ring.c
        ${BTCLASSIC_IDF}/a2dp_source.c
        ${BTCLASSIC_IDF}/avrcp_tg.c
        ${BTCLASSIC_IDF}/inquiry.c
        ${BTCLASSIC_IDF}/spp.c
        ${BTCLASSIC_IDF}/hid.c
        ${BTCLASSIC_IDF}/pbap.c
        ${BTCLASSIC_IDF}/hfp_ag.c
        ${BTCLASSIC_IDF}/map.c
        ${BTCLASSIC_IDF}/sdp_shared.c)
    target_include_directories(usermod_btclassic INTERFACE ${BTCLASSIC_IDF}/include ${BTCLASSIC_IDF})
    # The source's track metadata for a remote, through link-time hooks into
    # Bluedroid (avrcp_tg.c); off where they aren't known to fit.
    include(${BTCLASSIC_IDF}/avrcp_tg.cmake)
    btclassic_avrcp_tg(usermod_btclassic ${BTCLASSIC_IDF}/avrcp_tg.c)

    # With audiodsp in the build, AudioSink and HandsFree are also sources the
    # audio pump pulls in C. It's found the way audioif finds it: among the
    # build's modules, then beside this one.
    foreach(_btc_mod ${USER_C_MODULES})
        if(NOT IS_DIRECTORY ${_btc_mod})
            get_filename_component(_btc_mod ${_btc_mod} DIRECTORY)
        endif()
        if(EXISTS ${_btc_mod}/src/audiocore/__init__.h)
            set(BTCLASSIC_AUDIODSP_DIR ${_btc_mod})
            break()
        endif()
    endforeach()
    if(BTCLASSIC_AUDIODSP_DIR)
        target_include_directories(usermod_btclassic INTERFACE ${BTCLASSIC_AUDIODSP_DIR}/src)
        target_compile_definitions(usermod_btclassic INTERFACE BTCLASSIC_AUDIOSAMPLE=1)
        message(STATUS "btclassic: sources for audiodsp at ${BTCLASSIC_AUDIODSP_DIR}")
    endif()

    # The message client, on Bluedroid's OBEX internals (map.c).
    include(${BTCLASSIC_IDF}/map.cmake)
    btclassic_map(usermod_btclassic ${BTCLASSIC_IDF}/map.c)
    target_link_libraries(usermod INTERFACE usermod_btclassic)
    list(APPEND MICROPY_DEF_BOARD MICROPY_BLUETOOTH_BLUEDROID=1)
    # The backend's roles: central and the GATT client as well as the
    # peripheral and server. Events queue for the scheduler, so the queue is
    # bigger than upstream's 128 bytes: a discovery's results come in a burst.
    # Pairing too: Bluedroid keeps the bonds itself (patches/ lets upstream's
    # module pair without synchronous events when the backend does that).
    list(APPEND MICROPY_DEF_BOARD
        MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE=1
        MICROPY_PY_BLUETOOTH_ENABLE_PAIRING_BONDING=1
        MICROPY_PY_BLUETOOTH_BACKEND_STORES_KEYS=1
        MICROPY_PY_BLUETOOTH_RINGBUF_SIZE=1024)
    # Through MICROPY_DEF_BOARD, so the qstr scan sees the HID and phone entries too.
    list(APPEND MICROPY_DEF_BOARD BTC_HAS_HID=1 BTC_HAS_PHONEBOOK=1 BTC_HAS_PHONE=1)
elseif(MICROPY_BLUETOOTH_BTSTACK)
    # Ports that build BTstack for the `bluetooth` module (rp2 boards with a
    # CYW43 radio): the BTstack backend in btstack/ turns Classic on in the
    # same stack, so BLE stays.
    include(${CMAKE_CURRENT_LIST_DIR}/btstack/btstack.cmake)
    add_library(usermod_btclassic INTERFACE)
    target_sources(usermod_btclassic INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/src/mod_btclassic.c
        ${CMAKE_CURRENT_LIST_DIR}/src/mod_btclassic_source.c
        ${CMAKE_CURRENT_LIST_DIR}/src/mod_btclassic_serial.c
        ${CMAKE_CURRENT_LIST_DIR}/src/mod_btclassic_hid.c
        ${CMAKE_CURRENT_LIST_DIR}/src/mod_btclassic_phone.c
        ${CMAKE_CURRENT_LIST_DIR}/src/mod_btclassic_tether.c)
    # Through MICROPY_DEF_BOARD, so the qstr scan sees the HID and phone book
    # entries too.
    list(APPEND MICROPY_DEF_BOARD BTC_HAS_HID=1 BTC_HAS_PHONEBOOK=1 BTC_HAS_TETHER=1)
    target_link_libraries(usermod_btclassic INTERFACE usermod_btclassic_btstack)
    target_link_libraries(usermod INTERFACE usermod_btclassic)
endif()
