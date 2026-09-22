# The stock SPIRAM_OCT variant, plus this board's sdkconfig fragment (the
# partition table and the rest of sdkconfig.board). The append comes LAST:
# kconfgen takes the last assignment in SDKCONFIG_DEFAULTS, so a fragment
# listed before the port's own is silently overridden and the build
# succeeds against the wrong partition table (cmods#29).
include(${CMAKE_CURRENT_SOURCE_DIR}/boards/ESP32_GENERIC_S3/mpconfigvariant_SPIRAM_OCT.cmake)
list(APPEND SDKCONFIG_DEFAULTS ${CMAKE_CURRENT_LIST_DIR}/sdkconfig.board)
