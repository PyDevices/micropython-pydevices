# btclassic's ESP-IDF half as a component of its own: the radio, the audio
# sink (A2DP sink and AVRCP) and the hands-free unit (HFP) on Bluedroid, no
# MicroPython. MicroPython compiles these files as user-module source instead
# (../../micropython.cmake); this is for a plain ESP-IDF app, such as the
# compile check in ../../tests/idf_app.
# The source builds only where the chip has Bluetooth Classic and the project
# turned Bluedroid's A2DP on; elsewhere the component is empty. ESP-IDF works
# out requirements before it reads sdkconfig, so `bt` is required either way.
set(srcs)
if(CONFIG_BT_CLASSIC_ENABLED AND CONFIG_BT_A2DP_ENABLE)
    list(APPEND srcs "radio.c" "a2dp_sink.c" "avrcp_ct.c" "hfp_hf.c" "ring.c")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES bt nvs_flash esp_timer)
