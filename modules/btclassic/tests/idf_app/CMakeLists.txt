# The btclassic component alone, in a plain ESP-IDF app: proof that it compiles
# for every chip it should, on ESP-IDF releases MicroPython doesn't build with
# yet (the ESP32-S31 needs 6.1). CI builds it; nothing here is flashed.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../esp-idf)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(btclassic_compile)
