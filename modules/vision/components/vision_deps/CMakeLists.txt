# An ESP-IDF component with no sources of its own: its idf_component.yml asks
# the component manager for esp_new_jpeg (and esp_h264 on the P4), and it
# pulls ESP-IDF's JPEG driver into the build. imlib itself is compiled in the
# module (../../micropython.cmake): as a component of its own it would have
# to wait for MicroPython's generated headers while being one of their
# inputs, a cycle.
idf_component_register(REQUIRES esp_driver_jpeg)
