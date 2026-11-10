# vision — OpenMV-style image processing, ESP-DL inference and H.264

`import image` gives an ESP32-P4 or ESP32-S3 the OpenMV image API: draw on a
picture, find blobs, lines, circles, QR codes and AprilTags, convert and save
JPEG or PNG. The same module adds `espdl`, which runs ESP-DL models for
detection, pose, classification and faces, and on the P4 `h264`, the hardware
H.264 encoder, and `rtsp`, a server that streams its output. They are
esp-vision's modules.

It is opt-in: `--modules all` leaves it out because ESP-DL makes the image about
2 MB bigger on the S3 and 3 MB on the P4. Name it, and ulab, which `image`
works with:

```bash
./build_mp.py --port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI --flash 16MB --modules ulab,vision
```

```python
import image

picture = image.Image(320, 240, image.RGB565)
picture.clear()
picture.draw_rectangle(100, 60, 80, 50, color=(255, 0, 0), fill=True)

RED = (30, 100, 15, 127, 15, 127)
for blob in picture.find_blobs([RED], pixels_threshold=100, area_threshold=100):
    print("Found red at x=%d, y=%d, %d wide and %d high" % blob.rect())
```

[`examples/vision_simpletest.py`](examples/vision_simpletest.py) is that
program whole: it draws a red rectangle and a blue circle, prints where it
found the red, and saves the picture as a JPEG you can look at. No camera or
display is needed.

More examples: [`espdl_face_landmarks.py`](examples/espdl_face_landmarks.py)
finds faces and marks their eyes, nose and mouth, and
[`rtsp_stream.py`](examples/rtsp_stream.py) streams a moving test pattern as
H.264 over RTSP.

`image.pyi`, `espdl.pyi`, `h264.pyi` and `rtsp.pyi` list the names, with
their arguments. [Use esp-vision's modules](../../docs/guides/use-esp-vision-modules.md)
has the build notes, including the P4 before chip revision 3.
