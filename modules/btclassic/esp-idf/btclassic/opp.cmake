# Object Push (opp.c) runs on Bluedroid's OBEX and GOEP internals and its
# task: it needs their private headers. Those are internal, so this turns it
# on only for the ESP-IDF versions it was written against and where the
# project turned the GOEP client on (the phone book client brings it); on any
# other, ObjectPush isn't in the firmware. Set BTCLASSIC_OPP to OFF to leave
# it out anyway, as a board short of flash does.
#
#   btclassic_opp(<target> <path of opp.c>)
#
# It sets BTCLASSIC_OPP_BUILT in the caller's scope when it's in.

set(BTCLASSIC_OPP ON CACHE BOOL "OPP client and server for btclassic's ObjectPush")

function(btclassic_opp target file)
    set(BTCLASSIC_OPP_BUILT OFF PARENT_SCOPE)
    set(known "5.5" "6.1")
    set(version "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")
    if(NOT BTCLASSIC_OPP OR NOT version IN_LIST known OR NOT CONFIG_BT_PBAC_ENABLED OR NOT CONFIG_BT_SDP_COMMON_ENABLED)
        message(STATUS "btclassic: no Object Push on this build")
        return()
    endif()
    idf_build_get_property(idf_path IDF_PATH)
    set(bt ${idf_path}/components/bt)
    set_source_files_properties(${file} DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR} PROPERTIES
        INCLUDE_DIRECTORIES "${bt}/common/include;${bt}/common/osi/include;${bt}/host/bluedroid/common/include;${bt}/host/bluedroid/stack/include;${bt}/host/bluedroid/bta/include"
        COMPILE_DEFINITIONS "BTC_OPP=1")
    set(BTCLASSIC_OPP_BUILT ON PARENT_SCOPE)
endfunction()
