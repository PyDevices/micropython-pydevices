/* Arduino GIGA R1 WiFi with the GIGA Display Shield: the Wi-Fi chip's firmware
   in the QSPI flash the board reserves for it, read in place by the driver.
   Linked after the board's own stm32h747.ld. */

MEMORY
{
    FLASH_CYW43 (r) : ORIGIN = 0x90F00000, LENGTH = 1024K
}

SECTIONS
{
    .cyw43_fw :
    {
        . = ALIGN(4);
        KEEP(*(.cyw43_fw*))
        . = ALIGN(4);
    } > FLASH_CYW43
}
