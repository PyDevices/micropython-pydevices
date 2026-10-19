# The board half of the proof matrix's host leg (tools/matrix/run_host.py).
#
# Runs on the board whose native port is the USB host, with usbhost's check
# scripts (tools/host/*.py) beside it at the board's root and usb-core, the
# usb-host-* packages, midiports and adafruit_midi in /lib. Each check_*()
# returns a dict the runner reads as JSON; the runner decides pass or fail.
#
# PLANT names the checks whose planted fault is on, so the runner can show
# that each check fails when the thing it checks is broken.
import json
import time

import usb.core

PLANT = set()
BOARD_VID = 0x303A  # a board on the hub (Espressif's VID): the runner's peer, never touched without one


def _run(fn, *args):
    try:
        out = fn(*args)
    except Exception as e:  # a check that raises is a failed check, with the reason
        out = {"error": "%s: %s" % (type(e).__name__, e)}
    return json.dumps(out)


def _names(dev):
    try:
        return dev.manufacturer, dev.product
    except usb.core.USBError as e:
        return "!", str(e)  # a string read that failed, not a device without strings


def _classes(cfg):
    i, out = cfg[0], set()
    while i + 2 <= len(cfg) and cfg[i] >= 2:
        if cfg[i + 1] == 4:
            out.add(cfg[i + 5])
        i += cfg[i]
    return sorted(out)


# --- enumerate: every device on the port, its strings and its descriptor ---

def _enumerate():
    usb.core.start()
    # A slow drive can take more than the 1.5 s settle (Phase 3's ledger):
    # look until the count holds still for a second.
    last, still = -1, 0
    end = time.ticks_add(time.ticks_ms(), 8000)
    while time.ticks_diff(end, time.ticks_ms()) > 0 and still < 2:
        n = len(list(usb.core.find(find_all=True)))
        still = still + 1 if n == last else 0
        last = n
        time.sleep_ms(500)
    rows = []
    for dev in usb.core.find(find_all=True):
        cfg = bytes(dev.configuration_descriptor())
        classes = _classes(cfg)
        if "enumerate" in PLANT and 0x08 in classes:
            continue  # planted: the flash drive goes missing from the list
        rows.append({"id": "%04x:%04x" % (dev.idVendor, dev.idProduct), "speed": dev.speed,
                     "names": _names(dev), "classes": classes, "config": cfg.hex()})
    st = usb.core.stats()
    return {"devices": rows, "attaches": st["attaches"], "errors": st["errors"]}


def check_enumerate():
    return _run(_enumerate)


# --- msc: mount the flash drive, write, read back, remove ---

def check_msc():
    import msc_mount_check

    msc_mount_check.PLANT = "msc" in PLANT
    return _run(msc_mount_check.run)


# --- hid: a real keyboard, nobody typing ---

def _hid(reads=10):
    from usb.host.hid import Device, KEYBOARD

    usb.core.start()
    is_kbd = Device.match(KEYBOARD)
    dev = usb.core.find(custom_match=lambda d: d.idVendor != BOARD_VID and is_kbd(d))
    if dev is None:
        return {"skipped": "no keyboard on the port"}
    kbd = Device(dev, KEYBOARD)
    try:
        if "hid" in PLANT:
            kbd._class_out(0x0B, 1)  # planted: back to report protocol behind Device's back
        boot = kbd.boot_protocol()
        desc = len(kbd.report_descriptor())
        kbd.set_leds(0x02)  # Caps Lock on, then off: an OUT report each way
        kbd.set_leds(0x00)
        quiet = 0
        t0 = time.ticks_ms()
        for _ in range(reads):
            if kbd.read(100) is None:
                quiet += 1
        ms = time.ticks_diff(time.ticks_ms(), t0)
    finally:
        kbd.close()
    return {"device": "%s %s" % _names(dev), "interface": kbd.interface, "boot_protocol": boot,
            "report_descriptor": desc, "quiet_reads": quiet, "reads": reads, "ms": ms}


def check_hid():
    return _run(_hid)


# --- hid_typed: a peer board types through usb_hid, this board reads it ---

def _hid_typed(seconds):
    import kbd_host

    out = kbd_host.run(seconds=seconds, vid=BOARD_VID, boot=True)
    if "hid_typed" in PLANT and len(out["reports"]) > 2:
        del out["reports"][2]  # planted: one report lost on the way
    return out


def check_hid_typed(seconds=8):
    return _run(_hid_typed, seconds)


# --- cdc: type into a peer board's REPL over its USB serial port ---

def check_cdc():
    import cdc_check

    cdc_check.PLANT = "cdc" in PLANT
    return _run(cdc_check.run)


# --- midi: 200 note-ons through a peer board's MIDI echo, in order ---

def _midi(notes):
    import adafruit_midi
    from adafruit_midi.note_on import NoteOn
    from midiports import usb_host

    usb.core.start()
    dev = None
    for _ in range(40):
        dev = usb.core.find(idVendor=BOARD_VID, custom_match=usb_host.is_midi)
        if dev:
            break
        time.sleep_ms(250)
    if dev is None:
        return {"error": "no MIDI function on the peer"}
    midi_in, midi_out = usb_host.ports(dev)
    midi = adafruit_midi.MIDI(midi_in=midi_in, midi_out=midi_out, in_buf_size=256)
    while midi.receive() is not None:
        pass
    got = []
    t0 = time.ticks_ms()
    for i in range(notes):
        midi.send(NoteOn(i % 128, 1 + i // 128), channel=0)
        end = time.ticks_add(time.ticks_ms(), 100)
        while time.ticks_diff(end, time.ticks_ms()) > 0:
            msg = midi.receive()
            if isinstance(msg, NoteOn):
                got.append((msg.note, msg.velocity))
                break
    while True:  # anything late
        msg = midi.receive()
        if msg is None:
            break
        if isinstance(msg, NoteOn):
            got.append((msg.note, msg.velocity))
    ms = time.ticks_diff(time.ticks_ms(), t0)
    midi_in.close()
    midi_out.close()
    want = [(i % 128, 1 + i // 128) for i in range(notes)]
    return {"device": "%s %s" % _names(dev), "sent": notes, "received": len(got), "in_order": got == want,
            "ms": ms}


def check_midi(notes=200):
    return _run(_midi, notes)


# --- audio: a tone into a hosted speaker ---

def check_audio(seconds=3):
    import audio_check

    audio_check.PLANT = "audio" in PLANT
    return _run(audio_check.run, seconds)


# --- video: frames from a hosted camera, the last one decoded ---

def check_video(seconds=3):
    import video_check

    video_check.PLANT = "video" in PLANT
    return _run(video_check.run, seconds)
