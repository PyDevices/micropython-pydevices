# Stream a moving test pattern as H.264 over RTSP (P4): open
# rtsp://<board-ip>:8554/ in VLC or ffplay. Connects Wi-Fi from
# /lib/secrets.py (pydevices' wifi.py), prints the URL, runs for SECONDS.
#
# Run it as main.py, or with nothing attached to the USB port: an mpftp exec
# that outlasts mpftp's wait resets the board over USB (rst:0x17,
# CHIP_USB_UART_RESET), which looks like the stream crashing.
import time

import h264
import image
import network
import rtsp
import wifi

wifi.connect_from_secrets()
print("rtsp://%s:8554/" % network.WLAN(network.STA_IF).ifconfig()[0])

W, H, FPS, SECONDS = 320, 240, 15, 90

img = image.Image(W, H, image.RGB565)
enc = h264.H264Encoder(W, H, fps=FPS)
srv = rtsp.RTSPServer(W, H, fps=FPS, listen_port=8554)
period = 1000 // FPS
t_end = time.ticks_add(time.ticks_ms(), SECONDS * 1000)
n = 0
try:
    while time.ticks_diff(t_end, time.ticks_ms()) > 0:
        t0 = time.ticks_ms()
        img.clear()
        x = (n * 4) % (W - 40)
        img.draw_rectangle(x, 100, 40, 40, color=(255, 0, 0), fill=True)
        img.draw_string(8, 8, "frame %d" % n, color=(255, 255, 255), scale=2)
        srv.send(enc.encode(img))
        n += 1
        time.sleep_ms(max(0, period - time.ticks_diff(time.ticks_ms(), t0)))
finally:
    srv.stop()
    enc.close()
