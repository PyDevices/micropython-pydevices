# A guard on Bluedroid's A2DP event handler (a2dp_guard.c): a link event that
# arrives after A2DP was deinitialised is dropped instead of panicking the
# board. It wraps an internal function at link time, so it's on only for the
# ESP-IDF versions it was written against.
#
#   btclassic_a2dp_guard(<target> <path of a2dp_guard.c>)

function(btclassic_a2dp_guard target file)
    set(known "5.5" "6.1")
    set(version "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")
    if(NOT version IN_LIST known)
        return()
    endif()
    idf_build_get_property(idf_path IDF_PATH)
    set(bt ${idf_path}/components/bt)
    set_source_files_properties(${file} DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR} PROPERTIES INCLUDE_DIRECTORIES
        "${bt}/common/btc/include;${bt}/common/include;${bt}/host/bluedroid/common/include;${bt}/host/bluedroid/stack/include;${bt}/host/bluedroid/bta/include")
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "INTERFACE_LIBRARY")
        set(scope INTERFACE)
        target_sources(${target} INTERFACE ${file})
    else()
        set(scope PUBLIC)
        target_sources(${target} PRIVATE ${file})
    endif()
    # -u pulls the wrapper in from a component's archive, which nothing else
    # in it would otherwise reach.
    target_link_libraries(${target} ${scope} "-Wl,--wrap=btc_a2dp_cb_handler" "-Wl,-u,__wrap_btc_a2dp_cb_handler")
endfunction()
