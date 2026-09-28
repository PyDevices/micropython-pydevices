# What this board carries when no FROZEN_MANIFEST is given: everything, plus
# castif (casting the panel over Wi-Fi Display with the hardware H.264
# encoder). Pass FROZEN_MANIFEST=../../../micropython-pydevices/manifests/<preset>.py
# for a smaller set; a preset leaves castif out.
include("../../../manifests/kitchen-sink.py")
c_module("$(BOARD_DIR)/../../../usermods/castif")
