# mdns on a desktop MicroPython (unix or windows): the API, its errors, and a
# service this process advertises, found by this process.
#
#   micropython modules/mdns/tests/test_mdns.py
#
# Prints PASS or FAIL per check and ends with a count; exits 1 on a failure.
# It needs a network with a route for multicast, but nothing else on it.

import sys
import time

import mdns

failures = 0


def check(name, ok, detail=""):
    global failures
    print(("PASS " if ok else "FAIL ") + name + (": " + str(detail) if detail and not ok else ""))
    if not ok:
        failures += 1


def raises(exc, fn, *args, **kw):
    try:
        fn(*args, **kw)
    except exc as e:
        return str(e)
    return None


def find_ours(server, port=None, timeout=2):
    for s in server.find(service_type="_mpytest", protocol="_tcp", timeout=timeout):
        if s.instance_name == INSTANCE and (port is None or s.port == port):
            return s
    return None


# CircuitPython's unix build reports errors tersely: the exception, no message.
TERSE = sys.implementation.name == "circuitpython"


def says(got, want):
    return got == want or (TERSE and got == "")


INSTANCE = "mdns test %d" % (time.ticks_ms() & 0xFFFF)

check("Server and RemoteService are the module's", hasattr(mdns, "Server") and hasattr(mdns, "RemoteService"))
check("Server needs a network_interface", raises(TypeError, mdns.Server) is not None)
check("network_interface must be None or an address",
      says(raises(ValueError, mdns.Server, 42), "network_interface must be None or an IPv4 address"))

server = mdns.Server(None)
check("hostname is a name", isinstance(server.hostname, str) and len(server.hostname) > 0, server.hostname)
check("instance_name defaults to the hostname", server.instance_name == server.hostname)
check("one Server at a time", says(raises(RuntimeError, mdns.Server, None), "mDNS already initialized"))
check("find takes keywords only", raises(TypeError, server.find, "_mpytest", "_tcp") is not None)
check("advertise_service takes keywords only", raises(TypeError, server.advertise_service, "_mpytest", "_tcp", 1) is not None)
check("txt_records must be strings",
      raises(ValueError, server.advertise_service, service_type="_x", protocol="_tcp", port=1, txt_records=[1])
      == "Failed to add service TXT record; non-string or bytes found in txt_records")
check("at most 32 txt_records",
      raises(ValueError, server.advertise_service, service_type="_x", protocol="_tcp", port=1,
             txt_records=["k%d=v" % i for i in range(33)]) == "txt_records length must be <= 32")

server.instance_name = INSTANCE
check("instance_name can be set", server.instance_name == INSTANCE)
server.advertise_service(service_type="_mpytest", protocol="_tcp", port=8123, txt_records=["path=/", "flag"])
t = time.ticks_ms()
found = find_ours(server)
took = time.ticks_diff(time.ticks_ms(), t)
check("find() sees the advertised service", found is not None)
check("find() returns within its timeout", took < 2600, took)
if found:
    check("RemoteService has the service's fields",
          (found.service_type, found.protocol, found.port) == ("_mpytest", "_tcp", 8123),
          (found.service_type, found.protocol, found.port))
    check("RemoteService has the host", found.hostname.lower() == server.hostname.lower(), found.hostname)
    check("ipv4_address is a dotted address", found.ipv4_address is not None and len(found.ipv4_address.split(".")) == 4,
          found.ipv4_address)
    check("RemoteService is read-only", raises(AttributeError, setattr, found, "port", 1) is not None)

server.advertise_service(service_type="_mpytest", protocol="_tcp", port=8124)
check("a second call updates the port", find_ours(server, 8124) is not None)

host = "mpytest-%d" % (time.ticks_ms() & 0xFFFF)
if sys.platform == "win32":
    # Windows' mDNS service answers for the computer's own name only.
    check("hostname stays the computer's on Windows",
          raises(NotImplementedError, setattr, server, "hostname", host) is not None)
    server.hostname = server.hostname
else:
    server.hostname = host
    check("hostname can be set", server.hostname == host, server.hostname)
    found = find_ours(server, 8124)
    check("the service moves to the new hostname",
          found is not None and found.hostname.lower() == host and found.ipv4_address is not None,
          found and (found.hostname, found.ipv4_address))

check("nothing answers for a service nobody offers",
      server.find(service_type="_nobody-offers-this", protocol="_tcp", timeout=0.5) == ())

server.deinit()
check("deinit() twice is fine", raises(Exception, server.deinit) is None)
check("a deinited Server refuses", raises(ValueError, getattr, server, "hostname") is not None)

again = mdns.Server(None)
check("a new Server once the old one is deinited", again.hostname)
# Windows' mDNS service can answer for a withdrawn service for a moment.
gone = False
for _ in range(4):
    if find_ours(again, timeout=1) is None:
        gone = True
        break
check("deinit() withdrew the service", gone)
again.deinit()

print("%d failed" % failures)
sys.exit(1 if failures else 0)
