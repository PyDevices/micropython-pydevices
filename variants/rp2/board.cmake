# What every rp2 build adds to its board. build_mp.py includes this after the
# board's own mpconfigboard.cmake, and before any variant, so a variant can
# still change it.

# The CPU at the fastest clock its chip is rated for. Raspberry Pi certified the
# RP2040 for 200 MHz at a core voltage of 1.15 V (pico-sdk 2.1.1), where the
# SDK's default is still 125 MHz; with this set, the SDK raises the voltage at
# boot before it raises the clock. The RP2350's fastest is its default, 150 MHz,
# so this changes nothing there.
#
# Flash and PSRAM follow the CPU's clock through a divider, and stay inside
# their rating: the port works the divider out from the board's
# PICO_FLASH_SPI_CLKDIV, so a board whose flash ran at 62.5 MHz at 125 MHz runs
# it at 100 MHz at 200 (the Winbond W25Q parts on most RP2040 boards are rated
# for 133), and one whose boot stage reads at a quarter of the CPU's clock reads
# at 50 MHz. The RP2350 runs flash and PSRAM at 75 MHz, half its 150 MHz, since
# a divider of 1 would take them past their 133 MHz rating.
set(PICO_USE_FASTEST_SUPPORTED_CLOCK 1)
