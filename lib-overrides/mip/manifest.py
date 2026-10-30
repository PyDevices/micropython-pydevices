# mip: micropython-lib's mip with one change, a connection kept open per host
# (see ../README.md). modules/manifest.py puts lib-overrides ahead of
# micropython-lib in require()'s search, so every require("mip") -- the port's
# bundle-networking, mip-cmdline -- freezes this copy and upstream's is left out.
metadata(version="0.5.0", description="On-device package installer for network-capable boards")

require("requests")

package("mip", opt=3)
