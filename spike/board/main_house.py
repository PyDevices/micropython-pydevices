# The P4 boots into the Batch 2 smart-home appliance: the house panel on the
# display, the dashboard at http://<board ip>/, and a SHOW-TV touch button that
# casts to the 65". Ctrl-C at the REPL to stop it; the spike's cast scripts stay
# in /cast. Installed as /main.py.
import sys
sys.path.insert(0, "/cast")
try:
    import house_app
    house_app.run(port=80)
except KeyboardInterrupt:
    print("house app stopped; scripts in /cast")
