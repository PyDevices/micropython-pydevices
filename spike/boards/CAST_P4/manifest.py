# The panel's usual contents (every sibling module the workspace manifest finds,
# resolved from the DEFAULT micropython-pydevices tree so the sibling scan starts
# at the workspace root), plus the spike's encoder module.
include("/home/brad/gh/pydevices/micropython-pydevices/manifests/kitchen-sink.py")
c_module("$(BOARD_DIR)/../../h264enc")
c_module("$(BOARD_DIR)/../../jpegdec")
c_module("$(BOARD_DIR)/../../castif")
