# lib-overrides

Packages from micropython-lib that these firmware builds freeze their own copy
of. [`modules/manifest.py`](../modules/manifest.py) registers this folder ahead of micropython-lib, so a
port's `require("mip")` (bundle-networking on ESP32, `mip-cmdline` on unix)
gets the copy here and upstream's is left out. `mpremote mip` is unaffected,
since it runs on the host.

## mip

What differs from upstream's `micropython/mip`: one connection per host.
Upstream calls `requests.get` for each file, and `requests` opens a socket and
does a TLS handshake every time and closes it after. Installing a package of
146 files from `https://lib.omnipython.org` took about 110 s on a desktop,
nearly all of it handshakes. The copy here keeps the socket open between files
of the same host (HTTP/1.1 keep-alive) and does the same install in about 13 s.
The files it writes are identical.

The change is the block between `PyDevices patch begins` and `ends` in
`mip/__init__.py`, the two `requests.get` calls swapped for `_get`, and a
`_drop()` at the end of `install`. Anything that isn't `https://` with a
Content-Length (a redirect to http, chunked bodies, a port in the URL) falls
back to `requests.get`. [`test_lib_overrides.py`](../test_lib_overrides.py) checks that removing those
pieces gives upstream's file back, so to move to a newer upstream, copy its
`mip/__init__.py` in and re-apply the pieces.
