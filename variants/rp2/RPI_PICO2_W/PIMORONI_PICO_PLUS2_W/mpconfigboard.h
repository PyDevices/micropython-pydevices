// Pimoroni Pico Plus 2 W, after upstream's RPI_PICO2_W header (which defines
// these without #ifndef, hence the #undef).

#undef MICROPY_HW_BOARD_NAME
#define MICROPY_HW_BOARD_NAME                   "Pimoroni Pico Plus 2 W"

// What the board calls itself on a network, as the stock Pico 2 W does with its own name.
#undef MICROPY_PY_NETWORK_HOSTNAME_DEFAULT
#define MICROPY_PY_NETWORK_HOSTNAME_DEFAULT     "PicoPlus2W"
