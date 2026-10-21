# MicroPython CMake glue for usbhost (ESP32-S2/S3/P4; see the guard below).
# For Make-based ports (unix, windows), see micropython.mk in this dir.
#
# Point USER_C_MODULES at this repo (or this file) directly, e.g.:
#   idf.py build -DUSER_C_MODULES=<path to usbhost>/micropython.cmake
# Or add one line to your own manifest.py, which names this module via c_module():
#   include("<path to usbhost>/manifest.py")

set(USBHOST_MOD_DIR ${CMAKE_CURRENT_LIST_DIR})
set(USBHOST_SRC_DIR ${USBHOST_MOD_DIR}/src)

# usbhost is TinyUSB all the way down -- every source below needs it -- and
# TinyUSB is present on parts with a USB OTG controller: ESP32-S2, S3, and P4.
# That is the same line MicroPython draws with MICROPY_PY_TINYUSB
# (ports/esp32/esp32_common.cmake).
#
# Other ESP32 parts (C6, C3, C61, H2, classic) have USB Serial/JTAG at most, so
# the TinyUSB component is absent from their build. rp2 fails differently and
# more quietly: it HAS TinyUSB, but a usermod there does not get its include
# path, so every source fails on `tusb.h: No such file or directory`. Wiring
# that up is plausible work nobody has done, so rp2 is skipped too rather than
# half-supported.
#
# Without this guard the whole firmware fails to configure on those targets:
# idf_component_get_property() below is a hard CMake error for a component that
# does not exist, so the `if(usbhost_tusb_lib)` test never gets to run. Skipping
# the module leaves the rest of the firmware buildable.
if(NOT (ESP_PLATFORM AND (CONFIG_IDF_TARGET_ESP32S2 OR CONFIG_IDF_TARGET_ESP32S3 OR CONFIG_IDF_TARGET_ESP32P4)))
    message(STATUS
        "usbhost: skipped -- builds only on the ESP32-S2, S3, and P4. The firmware "
        "builds without the _usbhost module.")
    return()
endif()

add_library(usermod_usbhost INTERFACE)

target_sources(usermod_usbhost INTERFACE
    ${USBHOST_SRC_DIR}/mod_usbhost.c
    ${USBHOST_SRC_DIR}/usbhost_host.c
    ${USBHOST_SRC_DIR}/usbhost_host_xfer.c
    ${USBHOST_SRC_DIR}/usbhost_host_net.c
    ${USBHOST_SRC_DIR}/usbhost_host_stream.c
    ${USBHOST_SRC_DIR}/shared/usbhost_ringbuf.c
    ${USBHOST_SRC_DIR}/shared/usbhost_pcm_sink.c
)

target_include_directories(usermod_usbhost INTERFACE
    ${USBHOST_SRC_DIR}
)

if(ESP_PLATFORM)
    idf_component_get_property(usbhost_tusb_lib espressif__tinyusb COMPONENT_LIB)
    if(usbhost_tusb_lib)
        # TinyUSB's device-side interrupt path in IRAM (src/usbhost_isr_iram.lf),
        # so cache misses don't delay machine.USBDevice's streaming endpoints
        # while a display or anything else keeps the cache busy. Only that
        # path: the rest of TinyUSB stays in flash.
        __ldgen_add_fragment_files("${USBHOST_SRC_DIR}/usbhost_isr_iram.lf")
    endif()
    # The IDF defines ESP_PLATFORM for its own components but not for a user C
    # module, and FreeRTOS's ESP-IDF trace-macro defaults sit behind it: on the
    # S3 (Xtensa) portYIELD_FROM_ISR then fails to compile with
    # an implicit traceISR_EXIT_TO_SCHEDULER. Kitchen-sink builds never saw it
    # because another module's glue defines it; usbhost built on its own did.
    target_compile_definitions(usermod_usbhost INTERFACE ESP_PLATFORM=1)

endif()

target_link_libraries(usermod INTERFACE usermod_usbhost)

# --- which usbhost this firmware was built from ----------------------
# Computed at build time from this repo's own git, never stored; "unknown" when
# there is no git (a tarball). Read on a target as <module>.__revision__.
execute_process(
    COMMAND git -C ${USBHOST_MOD_DIR} describe --always --dirty --abbrev=7
    OUTPUT_VARIABLE USBHOST_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT USBHOST_REVISION)
    set(USBHOST_REVISION "unknown")
endif()
target_compile_definitions(usermod_usbhost INTERFACE USBHOST_REVISION=\"${USBHOST_REVISION}\")
