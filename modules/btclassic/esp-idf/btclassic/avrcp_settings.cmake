# The sender's player settings read back by the AVRCP controller
# (avrcp_settings.c): it wraps two internal Bluedroid functions at link time,
# so it's on only for the ESP-IDF versions it was written against. Elsewhere
# the sink's repeat and shuffle read as unknown.
#
#   btclassic_avrcp_settings(<target> <path of avrcp_settings.c>)

function(btclassic_avrcp_settings target file)
    set(known "5.5" "6.1")
    set(version "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")
    if(NOT version IN_LIST known)
        message(STATUS "btclassic: no AVRCP player settings on ESP-IDF ${version}")
        return()
    endif()
    idf_build_get_property(idf_path IDF_PATH)
    set(bt ${idf_path}/components/bt)
    set_source_files_properties(${file} DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR} PROPERTIES INCLUDE_DIRECTORIES
        "${bt}/common/include;${bt}/host/bluedroid/common/include;${bt}/host/bluedroid/stack/include;${bt}/host/bluedroid/bta/include")
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "INTERFACE_LIBRARY")
        set(scope INTERFACE)
        target_sources(${target} INTERFACE ${file})
    else()
        set(scope PUBLIC)
        target_sources(${target} PRIVATE ${file})
    endif()
    # -u pulls the wrappers in from a component's archive, which nothing else
    # in it would otherwise reach.
    target_link_libraries(${target} ${scope}
        "-Wl,--wrap=btc_avrc_ct_rn_evt_supported" "-Wl,-u,__wrap_btc_avrc_ct_rn_evt_supported"
        "-Wl,--wrap=AVRC_ParsResponse" "-Wl,-u,__wrap_AVRC_ParsResponse")
endfunction()
