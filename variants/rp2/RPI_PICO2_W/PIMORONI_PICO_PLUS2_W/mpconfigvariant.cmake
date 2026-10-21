# Pimoroni Pico Plus 2 W (PIM726) on top of upstream's RPI_PICO2_W: an RP2350B
# (QFN-80, 48 GPIOs) with 16 MB of flash, 8 MB of PSRAM and an RM2 radio.
# build_mp.py includes this after upstream's own default variant, which sets
# PICO_PLATFORM for the RP2350.

# The SDK's own header for this board. It is what puts the 16 MB flash size,
# the 8 MB PSRAM and its chip select (GP47), the user switch (GP45), the
# Qw/ST I2C bus (GP4/GP5) and the RM2's pins (REG_ON GP23, data and host wake
# GP24, CS GP25, clock GP29, LED on the radio's GPIO 0) into the build. These
# are the same pins CircuitPython's pimoroni_pico_plus2w sets by hand.
set(PICO_BOARD "pimoroni_pico_plus2_w_rp2350")

# The QFN-80 package has 48 GPIOs where the Pico 2 W's RP2350A has 30. This also
# picks the RP2350B alternate-function table for the pin names.
set(PICO_NUM_GPIOS 48)

# 8 MB of PSRAM on GP47. The port brings it up at boot and adds it to the
# heap beside the on-chip RAM. The SDK header gives the size; the chip select
# must agree with its PICO_PSRAM_CS_PIN or main.c refuses to build.
set(MICROPY_HW_ENABLE_PSRAM 1)
set(MICROPY_HW_PSRAM_CS_PIN 47)

# 13 MB of the 16 MB flash is the filesystem, and the firmware gets the other
# 3 MB: the image with every module in it is 2.9 MB on the Pico 2 W. A
# smaller build can ask for more with MICROPY_HW_FLASH_STORAGE_BYTES; one
# that doesn't fit is shrunk by build_mp.py. Upstream's board sets this
# for 4 MB of flash before this file runs, so only a value given on the
# command line (a cache entry) may keep it.
if(NOT DEFINED CACHE{MICROPY_HW_FLASH_STORAGE_BYTES})
    set(MICROPY_HW_FLASH_STORAGE_BYTES 13631488)  # 13 * 1024 * 1024
endif()

# The RM2 talks to the RP2350 over PIO at the system clock divided by 2, the
# divider CircuitPython uses on this board (CYW43_PIO_CLOCK_DIV_INT=2) and the
# SDK's default. It is written out so a change in the SDK can't move it; raise
# it if the radio turns unreliable at a higher system clock. The pins and the
# radio's three GPIOs (CYW43_WL_GPIO_COUNT, the LED on 0) come from the header
# above, so CircuitPython's -DCYW43_DEFAULT_PIN_WL_* flags have nothing to add.
list(APPEND MICROPY_DEF_BOARD
    CYW43_PIO_CLOCK_DIV_INT=2
    CYW43_PIO_CLOCK_DIV_FRAC8=0
)
