# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

# The Audio Kit's frozen content: the port's and the modules you named
# (modules/manifest.py), plus appkit and touchkit, which its apps import.
# Frozen, they run from flash and leave the kit's small filesystem for the
# app itself. Only this variant freezes them; other builds install them with
# mip.
include("../../../../modules/manifest.py")
include("../../../../modules/library.py")

require("appkit", library="omnipython-lib")
require("touchkit", library="omnipython-lib")
