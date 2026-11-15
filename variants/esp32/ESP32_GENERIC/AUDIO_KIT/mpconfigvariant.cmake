# Upstream's PSRAM variant, unchanged. This board's own settings are the
# sdkconfig beside this file, which build_mp.py appends last.
include(${MICROPY_BOARD_DIR}/mpconfigvariant_SPIRAM.cmake)

# A ROMFS partition for your own compiled packages: mounted at /rom, with
# /rom and /rom/lib on sys.path, and imported in place from flash.
list(APPEND MICROPY_DEF_BOARD MICROPY_VFS_ROM=1)
