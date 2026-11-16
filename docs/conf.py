# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Sphinx configuration for the omnipython-builds documentation.

Everything shared by the OmniPython documentation projects (theme, top bar,
Markdown setup) comes from omnipython-docs. This repository has no Python
package to document, so the reference pages are written by hand.
"""

from omnipython_docs import configure

globals().update(configure(
    "omnipython-builds", __file__,
    repo="PyDevices/omnipython-builds", edit_path="docs",
    version_file=None,
))

# Planning notes and upstream reports that live here until they move.

# index.md is plain Markdown so the docs read on GitHub too; the site
# navigation lives in contents.md.
master_doc = "contents"
