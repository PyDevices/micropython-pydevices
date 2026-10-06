# jpegio CMake glue (see micropython.mk for what the variables mean).

set(JPEGIO_DIR ${CMAKE_CURRENT_LIST_DIR}/src)

add_library(usermod_jpegio INTERFACE)
target_include_directories(usermod_jpegio INTERFACE
    ${JPEGIO_DIR}/tjpgd
)
target_link_libraries(usermod INTERFACE usermod_jpegio)

# The vendored TJpgDec is the firmware's only one (LVGL's is off by config).
target_sources(usermod_jpegio INTERFACE
    ${JPEGIO_DIR}/jpegio.c
    ${JPEGIO_DIR}/tjpgd/tjpgd.c
)

# The LVGL image decoder (lvgl_decoder.c) is built only when lvgl-micropython is
# in this build, decided from USER_C_MODULES the way micropython.mk decides it:
# what is on disk beside this module says nothing about what this build
# selected, and jpegio has to build without LVGL. Override with -DJPEGIO_LVGL=ON/OFF.
# The decoder includes "lvgl/lvgl.h" and "lvgl/src/...", which resolve through
# the bindings directory lvgl-micropython itself puts on the include path, so
# nothing here needs to know where the bindings are.
if(NOT DEFINED JPEGIO_LVGL)
    if("${USER_C_MODULES}" MATCHES "lvgl-micropython")
        set(JPEGIO_LVGL ON)
    else()
        set(JPEGIO_LVGL OFF)
    endif()
endif()

if(JPEGIO_LVGL)
    target_compile_definitions(usermod_jpegio INTERFACE JPEGIO_LVGL_DECODER=1)
    # No qstrs of its own (the Python-visible names live in jpegio.c).
    target_sources(usermod_jpegio INTERFACE
        ${JPEGIO_DIR}/lvgl_decoder.c
    )
endif()
