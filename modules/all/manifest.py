# Every module beside this one, except those that say they are opt-in: a
# module with an OPT_IN file (its text says why) is built only when named.
# Otherwise all means all, and a module leaves "all" only by leaving modules/.
#
# This can't hand the list back to ../manifest.py, because a build already
# runs that file and the manifest tool includes a file only once, so it
# applies the same rule itself: a module's manifest.py if it has one, else
# its C half (ulab keeps its glue in code/).
import os

_root = os.path.dirname(os.getcwd())  # include() runs a manifest from its own directory
for _name in sorted(os.listdir(_root)):
    if _name.startswith(".") or _name == "all":
        continue
    _dir = os.path.realpath(os.path.join(_root, _name))
    if not os.path.isdir(_dir) or os.path.isfile(os.path.join(_dir, "OPT_IN")):
        continue
    if os.path.isfile(os.path.join(_dir, "manifest.py")):
        include(_dir)
        continue
    for _c in (_dir, os.path.join(_dir, "code")):
        if os.path.isfile(os.path.join(_c, "micropython.mk")) or os.path.isfile(
            os.path.join(_c, "micropython.cmake")
        ):
            c_module(_c)
            break
    else:
        # all means all: a module that can't be included is an error, not a skip.
        raise ValueError(
            "module '{}': {} has no manifest.py and no micropython.mk or micropython.cmake".format(
                _name, _dir
            )
        )
