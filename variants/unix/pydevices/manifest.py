# The PyDevices desktop build: the port's own content and the modules you
# named (modules/manifest.py), plus the universal desktop board config,
# frozen. A board_config.py beside your script still wins: the script's
# directory comes before .frozen on sys.path. Board images never carry it,
# since no board variant includes it.
include("../../../modules/manifest.py")
include("../../../modules/pydevices/board_configs/desktop")
