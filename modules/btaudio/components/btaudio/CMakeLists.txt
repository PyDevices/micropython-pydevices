# btaudio's ESP-IDF half: A2DP sink and AVRCP on Bluedroid, no MicroPython.
# Its source builds only where the chip has Bluetooth Classic and the project
# turned Bluedroid's A2DP on (sdkconfig.esp32 beside micropython.cmake does);
# elsewhere the component is empty. ESP-IDF works out requirements before it
# reads sdkconfig, so `bt` is required either way.
set(srcs)
if(CONFIG_BT_CLASSIC_ENABLED AND CONFIG_BT_A2DP_ENABLE)
    list(APPEND srcs "btaudio.c")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES bt)
