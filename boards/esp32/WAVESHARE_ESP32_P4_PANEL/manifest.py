# What this board carries when no FROZEN_MANIFEST is given: every module
# (modules/all, castif included where the chip is a P4). build_mp.py always
# passes FROZEN_MANIFEST=modules/manifest.py, so this is only for a direct make.
include("../../../modules", modules="all")
