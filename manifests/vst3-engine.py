# The sidecar interpreter micropython-vst3 ships: everything the workspace
# builds, plus the engine's two usermods. Pair with the vst3-engine variant,
# which turns sockets, SSL and FFI off (compositions are code, and some of it
# runs at plugin-scan time).
include("kitchen-sink.py")
include("../../mpvst/usermods/vstaudio/manifest.py")
include("../../mpvst/usermods/vstui/manifest.py")
