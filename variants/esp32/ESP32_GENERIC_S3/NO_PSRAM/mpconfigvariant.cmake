# Upstream's generic S3 build with its quad-PSRAM settings taken out, for an
# S3 board that has no PSRAM. The settings beside this file, which build_mp.py
# appends last, take the TLS library back into internal RAM.
list(REMOVE_ITEM SDKCONFIG_DEFAULTS boards/sdkconfig.spiram_quad)
