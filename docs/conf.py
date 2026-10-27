"""Sphinx configuration for the micropython-pydevices documentation.

Everything shared by the PyDevices documentation projects (theme, top bar,
Markdown setup) comes from pydevices-docs. This repository has no Python
package to document, so the reference pages are written by hand.
"""

from pydevices_docs import configure

globals().update(configure(
    "micropython-pydevices", __file__,
    repo="PyDevices/micropython-pydevices", edit_path="docs",
    version_file=None,
))

# Planning notes and upstream reports that live here until they move.
