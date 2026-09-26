import socket, sys, time, hashlib
path, name = sys.argv[1], sys.argv[2]
data = open(path, "rb").read()
for attempt in range(30):
    try:
        s = socket.create_connection(("192.168.1.147", 5007), timeout=30)
        break
    except OSError:
        time.sleep(0.5)
t0 = time.time(); s.sendall(name.encode() + b"\n" + data); s.shutdown(socket.SHUT_WR)
s.settimeout(120)
reply = b""
while not reply.endswith(b"\n"):
    d = s.recv(256)
    if not d:
        break
    reply += d
s.close()
want = hashlib.sha256(data).hexdigest()
print("pushed", name, len(data), "bytes in %.1f s; board says %r; %s" % (time.time() - t0, reply.strip(), "MATCH" if want.encode() in reply else "MISMATCH"))
