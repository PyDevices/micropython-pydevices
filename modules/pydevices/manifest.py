# PyDevices' own Python, from omnipython-lib: what lib/ holds, which is
# `pydevices` and `bledev` as the library splits them, plus the host-only
# parts of audiodev, bledev, displaydev and multimer that mip ships in
# `pydevices-desktop` and that lib/ has always carried, so a build's frozen
# files are the ones it had when this module froze lib/ itself.
#
# Frozen at opt=3, as before. A frozen PyDevices comes before /lib on
# sys.path, so it shadows a copy installed with mip.
import os

include("../library.py")
require("pydevices", library="omnipython-lib", opt=3)
require("bledev", library="omnipython-lib", opt=3)

# pydevices-desktop also holds mip.py, micropython.py, pngio.py and mdns.py,
# which would stand in front of the firmware's own mip and of the C modules
# of the same names, and the three desktop helpers (usdl2, uwin32,
# frame_recorder) that only the desktop board config freezes. So only its
# packages' files are taken, from the library tree.
#
# A board's build leaves them out (build_mp.py sets PYDEVICES_PLATFORM to
# "board" for every port but unix, windows and webassembly, and the same for
# CircuitPython, whose only desktop port is unix): they are 26 files of desktop-only code, and on a
# CircuitPython board their names are more than a frozen-string pool of 64 KB
# can spare. With it unset, as when freeze_list.py lists a desktop manifest,
# they are frozen.
if os.environ.get("PYDEVICES_PLATFORM") != "board":
    _host = os.path.join(os.environ["PYDEVICES_LIBRARY"], "pydevices-desktop")
    for _pkg in sorted(os.listdir(_host)):
        if os.path.isdir(os.path.join(_host, _pkg)):
            _files = sorted(f for f in os.listdir(os.path.join(_host, _pkg)) if f.endswith(".py"))
            package(_pkg, files=_files, base_path=_host, opt=3)
