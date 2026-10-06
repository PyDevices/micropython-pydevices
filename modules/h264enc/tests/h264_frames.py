# The test's source frames, computed the same way on a board (MicroPython)
# and on the PC (CPython, check_h264_ffmpeg.py): a fixed RGB565 gradient with
# a 96x96 box crossing it, 8 px a frame, in native byte order.
W = H = 720
BOX = 96


def base():
    """The background: red across, green down, blue on the diagonal."""
    buf = bytearray(W * H * 2)
    for y in range(H):
        row = bytearray(W * 2)
        g6 = y * 63 // (H - 1)
        for x in range(W):
            r5 = x * 31 // (W - 1)
            b5 = ((x + y) * 31) // (W + H - 2)
            v = (r5 << 11) | (g6 << 5) | b5
            row[2 * x] = v & 255
            row[2 * x + 1] = v >> 8
        buf[y * W * 2:(y + 1) * W * 2] = row
    return buf


def box_at(k):
    """The box's top-left corner in frame k."""
    x = (k * 8) % (W - BOX)
    y = (H - BOX) // 2 + ((k * 3) % 64) - 32
    return x, y


def frame(bg, k, out):
    """Frame k into out: the background with a white box at box_at(k)."""
    out[:] = bg
    x, y = box_at(k)
    white = b"\xff" * (BOX * 2)
    for r in range(y, y + BOX):
        out[(r * W + x) * 2:(r * W + x + BOX) * 2] = white
