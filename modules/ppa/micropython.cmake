# ppa CMake glue (see micropython.mk). On an esp32p4 target the PPA is built
# in (PPA_HW=1, esp_driver_ppa, which main does not require on its own);
# everywhere else the software path does srm and convert, and fill and blend
# say there is no PPA.
add_library(usermod_ppa INTERFACE)
target_sources(usermod_ppa INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/mod_ppa.c
    ${CMAKE_CURRENT_LIST_DIR}/src/ppa_sw.c
    ${CMAKE_CURRENT_LIST_DIR}/src/ppa_hw.c
)
target_include_directories(usermod_ppa INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src)
if(ESP_PLATFORM AND IDF_TARGET STREQUAL "esp32p4")
    target_compile_definitions(usermod_ppa INTERFACE PPA_HW=1)
    target_link_libraries(usermod_ppa INTERFACE idf::esp_driver_ppa idf::esp_mm)
endif()
target_link_libraries(usermod INTERFACE usermod_ppa)
