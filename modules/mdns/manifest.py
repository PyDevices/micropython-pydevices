# mdns: CircuitPython's mdns API (find and advertise services on the local
# network). C only; esp32, rp2 with a network, unix and windows. On CPython,
# pydevices-desktop's mdns.py gives the same API over python-zeroconf.
c_module(".")
