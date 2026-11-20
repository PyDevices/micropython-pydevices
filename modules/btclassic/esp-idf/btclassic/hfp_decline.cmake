# Call audio declined (hfp_decline.c): on chips whose Bluetooth controller
# can't be trusted to set up a call's audio link, the hands-free unit asks an
# Android phone to keep calls' audio, and turns down any audio link a phone
# offers anyway. The call itself still reaches the program: its state, the
# caller, answer and hang-up.
#
# The ESP32-S31's controller library in ESP-IDF 6.1 asserts in about half the
# calls as the link starts, and sets none up in 6.1.1. Take a chip off the
# list once its controller carries call audio again.
#
# It wraps two internal Bluedroid functions at link time, so it's on only for
# the ESP-IDF versions it was written against. Elsewhere the unit takes call
# audio as usual.
#
#   btclassic_hfp_decline(<target> <path of hfp_decline.c>)

function(btclassic_hfp_decline target file)
    set(chips "esp32s31")
    set(known "5.5" "6.1")
    set(version "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")
    if(NOT IDF_TARGET IN_LIST chips OR NOT CONFIG_BT_HFP_CLIENT_ENABLE)
        return()
    endif()
    if(NOT version IN_LIST known)
        message(WARNING "btclassic: call audio on ${IDF_TARGET} isn't declined on ESP-IDF ${version}")
        return()
    endif()
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "INTERFACE_LIBRARY")
        set(scope INTERFACE)
        target_sources(${target} INTERFACE ${file})
    else()
        set(scope PUBLIC)
        target_sources(${target} PRIVATE ${file})
    endif()
    target_compile_definitions(${target} ${scope} BTCLASSIC_DECLINE_CALL_AUDIO=1)
    # -u pulls the wrappers in from a component's archive, which nothing else
    # in it would otherwise reach.
    target_link_libraries(${target} ${scope}
        "-Wl,--wrap=btm_sco_conn_req" "-Wl,-u,__wrap_btm_sco_conn_req"
        "-Wl,--wrap=PORT_WriteData" "-Wl,-u,__wrap_PORT_WriteData")
endfunction()
