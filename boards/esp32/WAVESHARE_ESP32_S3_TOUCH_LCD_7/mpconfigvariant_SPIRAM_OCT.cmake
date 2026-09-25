# The stock SPIRAM_OCT variant, then the 4.3" board's sdkconfig fragment (the
# partition table and everything the two boards share), then this board's
# own. The appends come LAST, in that order: kconfgen takes the last
# assignment in SDKCONFIG_DEFAULTS, so a fragment listed before the port's own
# is silently overridden (cmods#29), and sdkconfig.lcd7 must win over the
# shared values it changes.
include(${CMAKE_CURRENT_SOURCE_DIR}/boards/ESP32_GENERIC_S3/mpconfigvariant_SPIRAM_OCT.cmake)
list(APPEND SDKCONFIG_DEFAULTS ${CMAKE_CURRENT_LIST_DIR}/../WAVESHARE_ESP32_S3_TOUCH_43/sdkconfig.board)
list(APPEND SDKCONFIG_DEFAULTS ${CMAKE_CURRENT_LIST_DIR}/sdkconfig.lcd7)
