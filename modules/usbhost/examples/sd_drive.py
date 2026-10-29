# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Show the board's SD card to a computer as a USB drive.

Plug the board into a computer and run this: the card in its slot appears as
a removable drive, and files copy on and off with the file manager. Fonts and
graphics go in, sensor logs come out, with no special tool at either end.
``usb-device-msc`` serves any object that implements MicroPython's block
protocol, so the computer's reads and writes reach the card itself, whether
that is ``machine.SDCard``, mip's ``sdcard.SDCard`` over SPI, or a flash
partition. Success is the drive appearing on the computer.

Only one side may write at a time. While the computer has the card, don't
mount it on the board.

Needs: a board with USB device support, an SD card slot, and the
``usb-device-msc`` package. Install it with ``mip`` if your firmware doesn't
freeze it:

    mip.install("github:PyDevices/pydevices/usb-device-msc")

Run it:

    mpftp run examples/sd_drive.py

Another board can host this one: run ``usb_drive_read.py`` on it.

Tested on: an ESP32-P4 board with a 32 GB microSD card, on MicroPython
v1.30.0-preview.160.gb70ae1aca6 (built 2026-10-28), plugged into a Windows
computer.
"""

import time

import usb.device
from usb.device.msc import MSCInterface


def open_card():
    """Return a block device for the card, or raise with something useful.

    Native SDIO first: on boards that have it (the ESP32-P4 panels, for
    one) it is a hardware peripheral, needs no pin map, and is by far the
    quickest path. Falling back to the board config covers the boards whose
    card is on SPI, where the wiring is a per-board fact that `pydevices`
    already records rather than something this example should guess.
    """
    try:
        from machine import SDCard
    except ImportError:
        SDCard = None

    if SDCard is not None:
        try:
            return SDCard()
        except TypeError:
            return SDCard(slot=0)
        except Exception:
            pass        # no SDIO slot here; try the board config below

    try:
        import board_config  # noqa: F401  (populates the peripheral namespace)
        import boarddev

        return boarddev.sdcard()
    except Exception as exc:
        raise RuntimeError(
            "no SD card found: this board has no native SDIO slot and no "
            "board_config peripheral to describe its wiring. Build the block "
            "device yourself and pass it to MSCInterface(); see "
            "this file's header for what qualifies."
        ) from exc


def card_geometry(card):
    """Return ``(blocks, block_size)``, or raise if the card never answered.

    ``ioctl(4)`` and ``ioctl(5)`` are the block count and block size, and a
    card that is absent, unpowered or still timing out answers them with
    ``-1`` rather than raising. Serving that to a host is serving a drive of
    no blocks, which reads as the example being broken rather than the card.
    So refuse it here, while the reason is still known.
    """
    blocks = card.ioctl(4, 0)
    block_size = card.ioctl(5, 0)
    if not isinstance(blocks, int) or not isinstance(block_size, int) \
            or blocks <= 0 or block_size <= 0:
        raise RuntimeError(
            "the SD card did not answer (ioctl reported {} blocks of {} "
            "bytes): check that a card is seated in the slot, then reset "
            "the board, since a card left half-initialised by an earlier "
            "attempt does not recover on its own".format(blocks, block_size))
    return blocks, block_size


def main():
    card = open_card()
    blocks, block_size = card_geometry(card)
    print("card: {} blocks x {} bytes = {} MB".format(
        blocks, block_size, blocks * block_size // (1024 * 1024)))

    # The board's serial port stays up beside the drive, so the REPL is
    # reachable on the same cable while the computer has the card.
    msc = MSCInterface(card, product="SD card")
    usb.device.get().init(msc, builtin_driver=True)
    print("the card should now appear as a drive on the computer")

    # USB events arrive through MicroPython's scheduler on this port, so the
    # interpreter only has to stay responsive: a short sleep is enough, and a
    # long one starves enumeration.
    served = 0
    while True:
        time.sleep_ms(5)
        if msc.ejected:
            # "Safely remove" on the computer. Honouring it is what makes the
            # eject mean something, and it's the signal to wait for before
            # touching the card on the board again.
            print("the computer ejected the drive; the card is the board's again")
            return
        moved = msc.blocks_read + msc.blocks_written
        if moved != served:
            print("blocks read:", msc.blocks_read, "written:", msc.blocks_written,
                  "errors:", msc.errors)
            served = moved


if __name__ == "__main__":
    main()
