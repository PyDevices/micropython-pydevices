# ESP-IDF's host-to-controller path for LE data copies each outgoing ACL
# packet into the controller's buffer inside an assert(). In
# components/bt/porting_btdm/transport/driver/vhci/hci_driver_standard.c
# (the ESP32-S31's dual-mode controller):
#
#     assert(ble_mbuf_append(om, &data[1], length - 1) == 0);
#
# and the same with os_mbuf_append() in porting/, for the other chips that
# use that driver. A build with assertions off
# (CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE, as MicroPython's are)
# defines NDEBUG, the copy goes with the assert, and every LE packet Bluedroid
# sends leaves empty. A central connects and exchanges the MTU (its requests
# arrive), then waits for answers that never come and drops the link.
# Classic data takes a memcpy outside any assert, and NimBLE doesn't use this
# driver, so only BLE on Bluedroid breaks.
#
# So these files are compiled with NDEBUG undefined, keeping their asserts.
# The rest of ESP-IDF keeps them off.
function(btclassic_vhci_assert)
    if(NOT CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE OR NOT CONFIG_BT_BLUEDROID_ENABLED)
        return()
    endif()
    idf_build_get_property(idf_path IDF_PATH)
    set(files)
    foreach(dir porting porting_btdm)
        set(file ${idf_path}/components/bt/${dir}/transport/driver/vhci/hci_driver_standard.c)
        if(EXISTS ${file})
            list(APPEND files ${file})
        endif()
    endforeach()
    if(NOT files)
        return()
    endif()
    if(NOT TARGET __idf_bt)
        message(FATAL_ERROR "btclassic: ESP-IDF's bt component isn't a target yet, so its VHCI driver "
            "can't keep its asserts, and BLE would send empty packets (vhci_assert.cmake)")
    endif()
    set_property(SOURCE ${files} TARGET_DIRECTORY __idf_bt APPEND PROPERTY COMPILE_OPTIONS "-UNDEBUG")
endfunction()
