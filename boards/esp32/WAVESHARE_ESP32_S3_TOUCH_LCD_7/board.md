Waveshare ESP32-S3-Touch-LCD-7: an ESP32-S3 with octal PSRAM driving an
800x480 RGB panel, with Wi-Fi and often a USB host running beside it. Use the
SPIRAM_OCT variant.

It is the 4.3" board's configuration (same chip, memory and partition table:
`../WAVESHARE_ESP32_S3_TOUCH_43/sdkconfig.board`) plus `sdkconfig.lcd7`,
which protects internal DMA RAM and the Wi-Fi link from the panel; see the
comments there. Its `board_config.py` should pass `bounce_rows=10` and a
14 MHz pixel clock (PyDevices/pydevices#96).
