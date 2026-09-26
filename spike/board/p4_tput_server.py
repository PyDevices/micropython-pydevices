# Throughput with the P4 as the server, so the PC's firewall is not in the way:
# the PC connects (TCP 5006) or sends one datagram (UDP 5005); the P4 then
# floods it for 6 s. Prints what it sent; the PC prints what it received.
import socket, time, network
w = network.WLAN(network.STA_IF)
print("ip", w.ifconfig()[0], "rssi", w.status("rssi"))
try:
    w.config(pm=network.WLAN.PM_NONE)
except Exception as e:
    print("pm", e)
u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u.bind(("0.0.0.0", 5005))
u.settimeout(30)
d, a = u.recvfrom(64)
buf = bytearray(1316)
n = 0
stalls = 0
t0 = time.ticks_ms()
while time.ticks_diff(time.ticks_ms(), t0) < 6000:
    try:
        u.sendto(buf, a)
        n += 1
    except OSError:
        stalls += 1
        time.sleep_ms(1)
dt = time.ticks_diff(time.ticks_ms(), t0)
print("udp sent %d pkts %.2f Mbps, %d stalls" % (n, n * 1316 * 8 / dt / 1000, stalls))
u.close()
ls = socket.socket()
ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ls.bind(("0.0.0.0", 5006))
ls.listen(1)
ls.settimeout(30)
c, a = ls.accept()
buf = bytearray(4096)
n = 0
t0 = time.ticks_ms()
while time.ticks_diff(time.ticks_ms(), t0) < 6000:
    c.write(buf)
    n += 1
dt = time.ticks_diff(time.ticks_ms(), t0)
c.close()
ls.close()
print("tcp sent %d KB %.2f Mbps" % (n * 4, n * 4096 * 8 / dt / 1000))
print("TPUT_DONE")
