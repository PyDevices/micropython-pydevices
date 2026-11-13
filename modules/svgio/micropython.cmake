# svgio CMake glue (see micropython.mk).
set(SVGIO_DIR ${CMAKE_CURRENT_LIST_DIR}/src)

# --- which svgio this firmware was built from ----------------------------
# Computed at build time from this repo's own git, never stored; "unknown" when
# there is no git (a tarball). Read on a target as <module>.__revision__.
execute_process(
    COMMAND git -C ${CMAKE_CURRENT_LIST_DIR} describe --always --dirty --abbrev=7
    OUTPUT_VARIABLE SVGIO_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT SVGIO_REVISION)
    set(SVGIO_REVISION "unknown")
endif()

add_library(usermod_svgio INTERFACE)
target_compile_definitions(usermod_svgio INTERFACE SVGIO_REVISION=\"${SVGIO_REVISION}\")
target_include_directories(usermod_svgio INTERFACE
    ${SVGIO_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/third_party/nanosvg
)
target_sources(usermod_svgio INTERFACE
    ${SVGIO_DIR}/mod_svgio.c
    ${SVGIO_DIR}/svgio_nanosvg.c
)
target_link_libraries(usermod INTERFACE usermod_svgio)

# The LVGL image decoder (lvgl_decoder.c) is built only when LVGL is in this
# build: when USER_C_MODULES names omnipython-lvgl's ports/micropython, whose
# repository root, two levels up, holds the generated binding. Override with
# -DSVGIO_LVGL=ON/OFF.
if(NOT DEFINED SVGIO_LVGL)
    set(SVGIO_LVGL OFF)
    foreach(_svgio_mod ${USER_C_MODULES})
        if(NOT IS_DIRECTORY "${_svgio_mod}")
            get_filename_component(_svgio_mod "${_svgio_mod}" DIRECTORY)
        endif()
        if(EXISTS "${_svgio_mod}/../../generated/lvgl_micropython.c")
            set(SVGIO_LVGL ON)
        endif()
    endforeach()
endif()

if(SVGIO_LVGL)
    target_compile_definitions(usermod_svgio INTERFACE SVGIO_LVGL_DECODER=1)
    target_sources(usermod_svgio INTERFACE ${SVGIO_DIR}/lvgl_decoder.c)
endif()
