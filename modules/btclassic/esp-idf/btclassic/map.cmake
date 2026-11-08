# The message client (map.c) runs on Bluedroid's OBEX and GOEP internals and
# its task: it needs their private headers. Those are internal, so this turns
# it on only for the ESP-IDF versions it was written against and where the
# project turned the GOEP client on (the phone book client brings it); on any
# other, Messages says it isn't in this firmware. Set BTCLASSIC_MAP to OFF to
# leave it out anyway.
#
#   btclassic_map(<target> <path of map.c>)

set(BTCLASSIC_MAP ON CACHE BOOL "MAP client for btclassic's Messages")

function(btclassic_map target file)
    set(known "5.5" "6.1")
    set(version "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")
    if(NOT BTCLASSIC_MAP OR NOT version IN_LIST known OR NOT CONFIG_BT_PBAC_ENABLED OR NOT CONFIG_BT_SDP_COMMON_ENABLED)
        message(STATUS "btclassic: no MAP client on this build")
        return()
    endif()
    idf_build_get_property(idf_path IDF_PATH)
    set(bt ${idf_path}/components/bt)
    set_source_files_properties(${file} DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR} PROPERTIES
        INCLUDE_DIRECTORIES "${bt}/common/include;${bt}/common/osi/include;${bt}/host/bluedroid/common/include;${bt}/host/bluedroid/stack/include;${bt}/host/bluedroid/bta/include"
        COMPILE_DEFINITIONS "BTC_MAP=1")
endfunction()
