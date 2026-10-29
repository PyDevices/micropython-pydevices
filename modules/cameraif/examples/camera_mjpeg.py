# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Serve the camera to any browser on your network as MJPEG.

Run it, then open the URL it prints. Chrome, Firefox, Safari and VLC all play
the stream without a plugin or an app. MJPEG is one long HTTP response that
never ends, carrying JPEG after JPEG separated by a marker string named in
the Content-Type. There is no handshake and no codec state, so a client that
joins halfway through gets the next whole frame, and a dropped frame costs
nothing. The ESP32-P4 encodes JPEG in hardware, so the board spends its time
moving bytes, not making them.

The network limits the frame rate, not the camera. At 800x800 over an
ESP32-P4 whose Wi-Fi is an ESP32-C6 co-processor across a hosted link:

    quality 70    ~73 KB/frame     7 fps    ~4.3 Mbit/s
    quality 40    ~38 KB/frame    15 fps    ~4.6 Mbit/s

The same bitrate gives twice the frame rate at the smaller size, which means
the link is running flat out, and smaller frames are the only way to go faster.
So quality is a URL parameter (``/stream?q=40``) rather than a constant: the
right value depends on your network. A board with native Wi-Fi, or Ethernet,
goes much faster. Success is live video in the browser.

Needs: an ESP32-P4 board with a MIPI-CSI camera, set up in board_config,
firmware built with cameraif, and a ``secrets.py`` on the board with ``SSID``
and ``PASSWORD`` for your Wi-Fi.

Run it:

    mpremote cp secrets.py :
    mpremote run camera_mjpeg.py

Tested on: an ESP32-P4 board with an OV5647 camera at 800x800, with Wi-Fi
through an ESP32-C6.
"""

import socket
import time

import board_config

# Change these to match your setup.
PORT = 80
QUALITY = 70  # 1 to 100; /stream?q=40 overrides it for one viewer
BOUNDARY = "frameboundary"  # separates the JPEGs in the stream

# The page the browser gets: it shows the stream full size.
PAGE = """<!DOCTYPE html>
<html><head><title>ESP32-P4 camera</title>
<style>body{margin:0;background:#111;display:flex;align-items:center;
justify-content:center;height:100vh}img{max-width:100%;max-height:100vh}</style>
</head><body><img src="/stream"></body></html>"""


def connect():
    import network

    import secrets

    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    if not wlan.isconnected():
        print("connecting to %s..." % secrets.SSID)
        wlan.connect(secrets.SSID, secrets.PASSWORD)
        for _ in range(200):
            if wlan.isconnected():
                break
            time.sleep_ms(100)
    if not wlan.isconnected():
        raise OSError("wifi did not connect")
    return wlan.ifconfig()[0]


def send_all(sock, data):
    """Write every byte, or raise.

    ``socket.send`` may accept only part of a large buffer, and a JPEG is
    large. Treating a partial write as a whole one truncates the frame, and a
    truncated JPEG in an MJPEG stream does not error -- the browser simply
    shows the last good frame, so the stream looks alive and stops being
    true.
    """
    view = memoryview(data)
    while view:
        sent = sock.send(view)
        if sent <= 0:
            raise OSError("connection closed")
        view = view[sent:]


def stream_to(client, cam, quality=QUALITY):
    """Send JPEGs to one client until it hangs up."""
    # The headers announce a response that never ends, in parts.
    send_all(client, bytes(
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=%s\r\n"
        "Cache-Control: no-store\r\n\r\n" % BOUNDARY, "ascii"))
    # Send each frame with its own part header, and report the rate.
    frames = 0
    t0 = time.ticks_ms()
    while True:
        img = cam.capture_jpeg(quality)
        if img is None:
            continue
        send_all(client, bytes(
            "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n"
            % (BOUNDARY, len(img)), "ascii"))
        send_all(client, img)
        send_all(client, b"\r\n")
        frames += 1
        if frames % 25 == 0:
            dt = time.ticks_diff(time.ticks_ms(), t0)
            print("   %d frames, %.1f fps, %.1f KB/frame"
                  % (frames, frames * 1000 / dt, len(img) / 1024))


def main():
    # Join Wi-Fi with the details in secrets.py, then open the camera.
    ip = connect()
    cam = board_config.camera
    print("camera %s %dx%d" % ((cam.sensor()[0],) + cam.size()[:2]))
    print("open  http://%s/" % ip)

    # Serve one client at a time.
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", PORT))
    server.listen(1)
    try:
        while True:
            client, addr = server.accept()
            try:
                request = client.recv(512)
                path = b"/"
                if b" " in request:
                    path = request.split(b" ")[1]
                if path.startswith(b"/stream"):
                    quality = QUALITY
                    if b"q=" in path:
                        try:
                            quality = min(100, max(1, int(path.split(b"q=")[1][:3])))
                        except ValueError:
                            pass    # a junk query is not worth refusing over
                    print("streaming to %s at quality %d" % (addr[0], quality))
                    stream_to(client, cam, quality)
                else:
                    send_all(client, bytes(
                        "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n"
                        "Content-Length: %d\r\n\r\n%s" % (len(PAGE), PAGE), "ascii"))
            except OSError:
                pass        # a browser closing a tab is not an error
            finally:
                client.close()
    except KeyboardInterrupt:
        print("stopped")
    finally:
        server.close()
        cam.deinit()


if __name__ == "__main__":
    main()
