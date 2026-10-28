# mdns — find and advertise services on the local network

`import mdns` lets a board or a desktop announce what it offers on the
network (a web page, a speaker, a remote control) and find what other devices
offer, the way phones and laptops do. It's CircuitPython's `mdns` module: the
same classes, arguments, defaults and errors, so one program runs on either.

```python
import mdns, network

server = mdns.Server(network.WLAN(network.STA_IF))     # None on a computer
server.instance_name = "Kitchen display"
server.advertise_service(service_type="_http", protocol="_tcp", port=80,
                         txt_records=["path=/"])

for s in server.find(service_type="_spotify-connect", protocol="_tcp"):
    print(s.instance_name, s.hostname, s.ipv4_address, s.port)
```

It builds on four ports, each on what that system already has:

| Port | How |
|---|---|
| esp32 | ESP-IDF's mDNS component, the one that answers for the board's `<hostname>.local` |
| rp2 with a CYW43 radio (Pico W, Pico 2 W) | lwIP's mDNS responder and search |
| unix | a responder and a querier of its own |
| windows | Windows' own mDNS service, through its DNS-SD API |

`--modules all` includes it. The browser build can't send multicast, so it
has no `mdns`. CircuitPython's boards keep CircuitPython's own; its unix port has none, so
`--interpreter circuitpython --port unix` adds this one. On CPython,
`mdns.py` in pydevices-desktop gives the same API over python-zeroconf.

[`examples/mdns_simpletest.py`](examples/mdns_simpletest.py) advertises a web service and lists the ones it finds, itself included.

## API

### `mdns.Server(network_interface)`

On a board, `network_interface` is a `network.WLAN` (or `network.LAN` on
esp32), or `None` for whichever has an address. On unix and Windows it's
`None`, or on a computer with more than one network the IPv4 address of the
one to use (`"192.168.1.20"`). Anything else raises `ValueError`.

One `Server` may be live at a time: a second raises
`RuntimeError("mDNS already initialized")` until the first is `deinit()`ed.

### `hostname`, `instance_name`

`hostname` is the name the device answers to as `<hostname>.local`. It
starts as the board's `network.hostname()`, or the computer's name. Setting
it renames the device for mDNS (on Windows it stays the computer's name,
because Windows' mDNS service answers for no other; setting another raises
`NotImplementedError`).

`instance_name` is the human-readable name advertised services go by. It's
the hostname until you set it.

### `find(*, service_type, protocol, timeout=1) -> tuple`

Every service of `service_type` (`"_http"`) over `protocol` (`"_tcp"` or
`"_udp"`) that answers within `timeout` seconds, as `RemoteService`s. The
arguments are keyword-only, as CircuitPython's binding has them. Ctrl-C ends
the wait.

### `advertise_service(*, service_type, protocol, port, txt_records=None)`

Answer for a service on `port`. `txt_records` is up to 32 strings, each
`"key=value"` or a key alone. Calling it again for the same `service_type`
and `protocol` changes the port and the records. A `Server` offers up to 8
services at a time (lwIP on rp2, one).

### `deinit()`

Withdraws every service the `Server` advertised and frees its place. A soft
reset does this too. On esp32 the board goes on answering for its hostname,
as it did before.

### `RemoteService`

Read-only, made only by `find()`: `hostname` (without `.local`),
`instance_name`, `service_type`, `protocol`, `port`, and `ipv4_address`, a
dotted string or `None` if no address came back. (CircuitPython's is an
`ipaddress.IPv4Address`; `str()` of either is the dotted address.)

## Tests

[`tests/test_mdns.py`](tests/test_mdns.py) runs on a unix or Windows build: the API, its errors,
and a service the process advertises and finds itself.
[`tests/check_mdns_zeroconf.py`](tests/check_mdns_zeroconf.py) runs that build against python-zeroconf in
another process, both ways.

```bash
./build_mp.py --port unix --modules mdns
./builds/unix/standard/micropython modules/mdns/tests/test_mdns.py
pip install zeroconf
python3 modules/mdns/tests/check_mdns_zeroconf.py ./builds/unix/standard/micropython
```

## How

**unix** binds 224.0.0.251:5353 beside whatever else holds it (Avahi,
systemd-resolved) and answers from a thread: for the hostname, each service's
PTR, SRV and TXT, the DNS-SD service list, and questions asked from other
ports, straight back to the asker. It announces a service twice when it
appears or changes and says goodbye when it goes. It doesn't probe for name
conflicts. `find()` asks from 5353 too, and asks again halfway through, since
multicast gets lost on Wi-Fi. Where 5353 can't be had, `find()` still works,
from a spare port, and `advertise_service()` raises `OSError(EADDRINUSE)`.

Under WSL with mirrored networking, the Linux side and Windows don't hear
each other's multicast: a unix build and a Windows program on the same
computer can't find each other, though both find, and are found by, every
other device on the network.

**windows** hands services to Windows' mDNS service (`DnsServiceRegister`),
which holds 5353 and answers for them, and finds with `DnsServiceBrowse` and
`DnsServiceResolve`. Windows may answer for a withdrawn service for a second
or so after `deinit()`.

**esp32** shares ESP-IDF's mDNS component with the port, which starts it
when the network gets an address. A `Server` starts it only if the port
hasn't, and never stops it.

**rp2** gives lwIP's responder the station's interface, which the port
doesn't, and takes it off again at `deinit()`. It builds, but hasn't been
run on a Pico W yet.
