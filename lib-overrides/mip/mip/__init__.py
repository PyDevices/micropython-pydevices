# MicroPython package installer
# MIT license; Copyright (c) 2022 Jim Mussared

import sys

import requests

from micropython import const

_PACKAGE_INDEX = const("https://micropython.org/pi/v2")
_CHUNK_SIZE = const(128)

# Since all URLs are accessed via HTTPS, the URL scheme is added by _rewrite_url.
# The first three format parameters are assumed to be the organisation, the
# repository names, and the branch/tag name, in this order.
_HOSTS = {
    # https://codeberg.org/api/v1/repos/{org}/{repo}/raw/{path}?ref={branch}
    "codeberg:": "codeberg.org/api/v1/repos/{}/{}/raw/{p}?ref={}",
    # https://raw.githubusercontent.com/{org}/{repo}/{branch}/{path}
    "github:": "raw.githubusercontent.com/{}/{}/{}/{p}",
    # https://gitlab.com/{org}/{repo}/-/raw/{branch}/{path}
    "gitlab:": "gitlab.com/{}/{}/-/raw/{}/{p}",
}

_ALLOWED_MIP_URL_PREFIXES = const(("http://", "https://", "codeberg:", "github:", "gitlab:"))


# --- PyDevices patch begins: one connection per host (see ../../README.md) ---
# Upstream calls requests.get for every file, which opens a new socket and does
# a full TLS handshake each time (about 0.75 s a file on a desktop, a second on
# a board). _get keeps one socket per host and reuses it. It is the only change
# to this file besides the two call sites below.
_conn = [None, None]  # host, socket


class _Body:
    def __init__(self, s, n):
        self._s = s
        self._rem = n

    def readinto(self, buf):
        if self._rem <= 0:
            return 0
        if len(buf) > self._rem:
            buf = memoryview(buf)[: self._rem]
        n = self._s.readinto(buf)
        if not n:
            _drop()
            raise OSError("closed")
        self._rem -= n
        return n


class _Resp:
    def __init__(self, status, raw):
        self.status_code = status
        self.raw = raw

    def close(self):
        if self.raw is not None and self.raw._rem > 0:
            _drop()  # unread body left on the socket: not reusable
        self.raw = None

    def json(self):
        import json

        b = b""
        buf = bytearray(256)
        while True:
            n = self.raw.readinto(buf)
            if not n:
                break
            b += buf[:n]
        return json.loads(b)


def _drop():
    s = _conn[1]
    _conn[0] = _conn[1] = None
    if s:
        try:
            s.close()
        except Exception:
            pass


def _open(host):
    import socket
    import tls

    ai = socket.getaddrinfo(host, 443, 0, socket.SOCK_STREAM)[0]
    s = socket.socket(ai[0], socket.SOCK_STREAM, ai[2])
    s.connect(ai[-1])
    ctx = tls.SSLContext(tls.PROTOCOL_TLS_CLIENT)
    ctx.verify_mode = tls.CERT_NONE  # as requests does
    s = ctx.wrap_socket(s, server_hostname=host)
    _conn[0] = host
    _conn[1] = s
    return s


def _get(url):
    if not url.startswith("https://"):
        return requests.get(url)
    host, _, path = url[8:].partition("/")
    if ":" in host or "?" in host or "#" in host:
        return requests.get(url)
    for attempt in (0, 1):
        reuse = _conn[0] == host
        if not reuse:
            _drop()
        try:
            s = _conn[1] if reuse else _open(host)
            s.write(b"GET /%s HTTP/1.1\r\nHost: %s\r\n\r\n" % (path, host))
            line = s.readline()
            if not line:
                raise OSError("eof")
            status = int(line.split(None, 2)[1])
            n = -1
            while True:
                h = s.readline()
                if h in (b"\r\n", b"\n", b""):
                    break
                if h.lower().startswith(b"content-length:"):
                    n = int(h.split(b":")[1])
            if n < 0:
                # No length (chunked): this socket can't be reused safely.
                _drop()
                return requests.get(url)
            return _Resp(status, _Body(s, n))
        except OSError:
            _drop()
            if not reuse:
                raise


# --- PyDevices patch ends ---


# This implements os.makedirs(os.dirname(path))
def _ensure_path_exists(path):
    import os

    split = path.split("/")

    # Handle paths starting with "/".
    if not split[0]:
        split.pop(0)
        split[0] = "/" + split[0]

    prefix = ""
    for i in range(len(split) - 1):
        prefix += split[i]
        try:
            os.stat(prefix)
        except:
            os.mkdir(prefix)
        prefix += "/"


# Copy from src (stream) to dest (function-taking-bytes)
def _chunk(src, dest):
    buf = memoryview(bytearray(_CHUNK_SIZE))
    while True:
        n = src.readinto(buf)
        if n == 0:
            break
        dest(buf if n == _CHUNK_SIZE else buf[:n])


# Check if the specified path exists and matches the hash.
def _check_exists(path, short_hash):
    import os

    try:
        import binascii
        import hashlib

        with open(path, "rb") as f:
            hs256 = hashlib.sha256()
            _chunk(f, hs256.update)
            existing_hash = str(binascii.hexlify(hs256.digest())[: len(short_hash)], "utf-8")
            return existing_hash == short_hash
    except:
        return False


def _rewrite_url(url, branch=None):
    for provider, url_format in _HOSTS.items():
        if not url.startswith(provider):
            continue
        components = url[len(provider) :].split("/")
        # Add https:// prefix to final URL.
        return _ALLOWED_MIP_URL_PREFIXES[1] + url_format.format(
            components[0], components[1], branch or "HEAD", p="/".join(components[2:])
        )
    return url


def _download_file(url, dest):
    response = _get(url)  # PyDevices: was requests.get
    try:
        if response.status_code != 200:
            print("Error", response.status_code, "requesting", url)
            return False

        print("Copying:", dest)
        _ensure_path_exists(dest)
        with open(dest, "wb") as f:
            _chunk(response.raw, f.write)

        return True
    finally:
        response.close()


def _install_json(package_json_url, index, target, version, mpy):
    response = _get(_rewrite_url(package_json_url, version))  # PyDevices: was requests.get
    try:
        if response.status_code != 200:
            print("Package not found:", package_json_url)
            return False

        package_json = response.json()
    finally:
        response.close()
    for target_path, short_hash in package_json.get("hashes", ()):
        fs_target_path = target + "/" + target_path
        if _check_exists(fs_target_path, short_hash):
            print("Exists:", fs_target_path)
        else:
            file_url = "{}/file/{}/{}".format(index, short_hash[:2], short_hash)
            if not _download_file(file_url, fs_target_path):
                print("File not found: {} {}".format(target_path, short_hash))
                return False
    base_url = package_json_url.rpartition("/")[0]
    for target_path, url in package_json.get("urls", ()):
        fs_target_path = target + "/" + target_path
        is_full_url = any(url.startswith(p) for p in _ALLOWED_MIP_URL_PREFIXES)
        if base_url and not is_full_url:
            url = f"{base_url}/{url}"  # Relative URLs
        if not _download_file(_rewrite_url(url, version), fs_target_path):
            print("File not found: {} {}".format(target_path, url))
            return False
    for dep, dep_version in package_json.get("deps", ()):
        if not _install_package(dep, index, target, dep_version, mpy):
            return False
    return True


def _install_package(package, index, target, version, mpy):
    if any(package.startswith(p) for p in _ALLOWED_MIP_URL_PREFIXES):
        if package.endswith(".py") or package.endswith(".mpy"):
            print("Downloading {} to {}".format(package, target))
            return _download_file(
                _rewrite_url(package, version), target + "/" + package.rsplit("/")[-1]
            )
        else:
            if not package.endswith(".json"):
                if not package.endswith("/"):
                    package += "/"
                package += "package.json"
            print("Installing {} to {}".format(package, target))
    else:
        if not version:
            version = "latest"
        print("Installing {} ({}) from {} to {}".format(package, version, index, target))

        mpy_version = (
            sys.implementation._mpy & 0xFF if mpy and hasattr(sys.implementation, "_mpy") else "py"
        )

        package = "{}/package/{}/{}/{}.json".format(index, mpy_version, package, version)

    return _install_json(package, index, target, version, mpy)


def install(package, index=None, target=None, version=None, mpy=True):
    if not target:
        for p in sys.path:
            if not p.startswith("/rom") and p.endswith("/lib"):
                target = p
                break
        else:
            print("Unable to find lib dir in sys.path")
            return

    if not index:
        index = _PACKAGE_INDEX

    if _install_package(package, index.rstrip("/"), target, version, mpy):
        print("Done")
        _drop()  # PyDevices
    else:
        print("Package may be partially installed")
        _drop()  # PyDevices
