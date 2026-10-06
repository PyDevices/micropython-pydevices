# h264enc: RGB565 frames to H.264 on the ESP32-P4's hardware encoder (the PPA
# converts, esp_h264 encodes). C only, ESP32-P4 only; castif encodes through
# its C API. Moved out of castif (media modules roadmap, Phase 3).
c_module(".")
