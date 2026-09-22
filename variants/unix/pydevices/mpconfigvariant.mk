# The PyDevices unix build: upstream's standard variant, plus os.dupterm
# (mpconfigvariant.h) and the kitchen-sink preset by default. Pass
# FROZEN_MANIFEST=../../../micropython-pydevices/manifests/<preset>.py for
# a different set of modules.
FROZEN_MANIFEST ?= $(VARIANT_DIR)/manifest.py
