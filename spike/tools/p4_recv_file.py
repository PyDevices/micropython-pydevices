# Receive "NAME\n" then the file bytes over TCP 5007 into /cast/NAME; print size and sha256.
import socket, os, hashlib, time
try:
    os.mkdir("/cast")
except OSError:
    pass
ls = socket.socket()
ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ls.bind(("0.0.0.0", 5007))
ls.listen(1)
print("listening 5007")
c, a = ls.accept()
c.settimeout(30)
name = b""
while not name.endswith(b"\n"):
    name += c.recv(1)
name = name.strip().decode()
h = hashlib.sha256()
n = 0
t0 = time.ticks_ms()
buf = bytearray(8192)
mv = memoryview(buf)
with open("/cast/" + name, "wb") as f:
    while True:
        k = c.readinto(buf)
        if not k:
            break
        f.write(mv[:k])
        h.update(mv[:k])
        n += k
dt = time.ticks_diff(time.ticks_ms(), t0)
digest = h.digest().hex()
try:
    c.write(("ok %d %s\n" % (n, digest)).encode())
except Exception:
    pass
c.close(); ls.close()
print("got", name, n, "bytes in", dt, "ms sha", digest[:16])
