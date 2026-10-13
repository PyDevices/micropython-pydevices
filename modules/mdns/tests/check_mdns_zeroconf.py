#!/usr/bin/env python3
"""mdns against another implementation: python-zeroconf, in another process.

    python3 check_mdns_zeroconf.py ./builds/unix/standard/micropython

MicroPython advertises a service and zeroconf finds it, port, address and TXT
records; then zeroconf advertises one and MicroPython's find() lists it.
Prints PASS or FAIL per check; exits 1 on a failure. Needs `pip install
zeroconf` and a network with a route for multicast.
"""

import socket
import subprocess
import sys
import time

from zeroconf import IPVersion, ServiceBrowser, ServiceInfo, ServiceStateChange, Zeroconf

failures = 0


def check(name, ok, detail=""):
    global failures
    print(("PASS " if ok else "FAIL ") + name + ("" if ok or not detail else ": " + str(detail)))
    failures += not ok


def local_ipv4():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("224.0.0.251", 5353))
        return s.getsockname()[0]
    finally:
        s.close()


def main(micropython):
    zc = Zeroconf(ip_version=IPVersion.V4Only)
    try:
        # MicroPython advertises; zeroconf finds it.
        advertise = """
import mdns, time
s = mdns.Server(None)
s.instance_name = "from micropython"
s.advertise_service(service_type="_mpycheck", protocol="_tcp", port=8123, txt_records=["path=/", "flag"])
print("ready")
time.sleep(10)
s.deinit()
"""
        mp = subprocess.Popen([micropython, "-c", advertise], stdout=subprocess.PIPE, text=True)
        check("MicroPython advertises", mp.stdout.readline().strip() == "ready")
        names = []
        ServiceBrowser(zc, "_mpycheck._tcp.local.",
                       handlers=[lambda zeroconf, service_type, name, state_change:
                                 state_change is ServiceStateChange.Added and names.append(name)])
        deadline = time.monotonic() + 6
        while "from micropython._mpycheck._tcp.local." not in names and time.monotonic() < deadline:
            time.sleep(0.1)
        check("zeroconf finds it", "from micropython._mpycheck._tcp.local." in names, names)
        info = zc.get_service_info("_mpycheck._tcp.local.", "from micropython._mpycheck._tcp.local.", timeout=3000)
        check("with its port", info is not None and info.port == 8123, info and info.port)
        check("its address", info is not None and info.parsed_addresses(IPVersion.V4Only) != [],
              info and info.parsed_addresses())
        check("and its TXT records", info is not None and info.properties == {b"path": b"/", b"flag": None},
              info and info.properties)
        mp.terminate()
        mp.wait()

        # zeroconf advertises; MicroPython finds it.
        theirs = ServiceInfo(
            "_zccheck._tcp.local.",
            "from zeroconf._zccheck._tcp.local.",
            addresses=[socket.inet_aton(local_ipv4())],
            port=9123,
            server="zccheck-host.local.",
        )
        zc.register_service(theirs)
        find = """
import mdns
s = mdns.Server(None)
for r in s.find(service_type="_zccheck", protocol="_tcp", timeout=3):
    print(r.instance_name, r.hostname, r.ipv4_address, r.port)
s.deinit()
"""
        out = subprocess.run([micropython, "-c", find], capture_output=True, text=True, timeout=30).stdout
        lines = [l.split(" ") for l in out.splitlines()]
        check("MicroPython finds zeroconf's service",
              any(l[:2] == ["from", "zeroconf"] and l[2] == "zccheck-host" and l[4] == "9123" for l in lines), out)
        zc.unregister_service(theirs)
    finally:
        zc.close()
    print("%d failed" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "micropython"))
