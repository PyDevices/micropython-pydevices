# The AVRCP target's track metadata (avrcp_tg.c) reaches inside Bluedroid:
# its private headers, and three functions wrapped at link time. Those are
# internal, so this turns it on only for the ESP-IDF versions it was written
# against; on any other, the source works and sends no metadata. Set
# BTCLASSIC_TG_METADATA to OFF to leave it out anyway.
#
#   btclassic_avrcp_tg(<target> <path of avrcp_tg.c>)
#
# <target> takes the definition and the link flags (an INTERFACE library or a
# component's library); the private include directories go on the one file.

set(BTCLASSIC_TG_METADATA ON CACHE BOOL "AVRCP target metadata for btclassic's audio source")

function(btclassic_avrcp_tg target file)
    set(known "5.5" "6.1")
    set(version "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")
    if(NOT BTCLASSIC_TG_METADATA OR NOT version IN_LIST known)
        message(STATUS "btclassic: no AVRCP target metadata on ESP-IDF ${version}")
        return()
    endif()
    idf_build_get_property(idf_path IDF_PATH)
    set(bt ${idf_path}/components/bt)
    # A source file's properties belong to a directory: set them both here and
    # at the top, where MicroPython's firmware target compiles user modules.
    set_source_files_properties(${file} DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR} PROPERTIES INCLUDE_DIRECTORIES
        "${bt}/common/include;${bt}/host/bluedroid/common/include;${bt}/host/bluedroid/stack/include;${bt}/host/bluedroid/bta/include")
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "INTERFACE_LIBRARY")
        set(scope INTERFACE)
    else()
        set(scope PUBLIC)
    endif()
    target_compile_definitions(${target} ${scope} BTC_TG_METADATA=1)
    target_link_libraries(${target} ${scope}
        "-Wl,--wrap=btc_rc_handler"
        "-Wl,--wrap=btc_avrc_tg_rn_evt_supported"
        "-Wl,--wrap=btc_avrc_tg_get_rn_supported_evt")
endfunction()
