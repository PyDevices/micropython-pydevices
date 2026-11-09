# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Run Bluetooth Classic and BLE at once on one board.

The board serves a Classic serial port that echoes what you send, while
MicroPython's own bluetooth module advertises the board over BLE and scans
for BLE devices nearby. Every ten seconds it prints how many BLE devices it
has heard, and whether a serial device is connected.

Needs: a board with firmware built with the btclassic module and the
bluetooth module (a classic ESP32, where BLE runs on Bluedroid beside
Classic, or a Pico W or Pico 2 W).

Run it:

    mpftp run btclassic_with_ble.py

A phone's BLE scanner app lists the board under its hostname; a serial
terminal on your computer or phone connects to the Classic port.

Tested on:
"""

import time

import bluetooth
import btclassic
import network

# Change these to match your setup.
ADVERTISE_US = 250_000  # BLE advertising interval, in microseconds
SCAN_MS = 10_000  # each BLE scan, in milliseconds

# Wi-Fi and Bluetooth share the radio. Turn Wi-Fi off for a steadier link.
network.WLAN(network.STA_IF).active(False)
name = network.hostname().encode()

# Start the Classic serial port.
bt = btclassic.Bluetooth()
port = btclassic.Serial(bt)

# Start BLE beside it. Scan results arrive through the IRQ handler.
ble = bluetooth.BLE()
ble.active(True)
heard = set()


def on_ble(event, data):
    # Event 5 is a scan result: (addr_type, addr, adv_type, rssi, adv_data).
    if event == 5:
        heard.add(bytes(data[1]))


ble.irq(on_ble)

# Advertise the name: a flags field, then the complete local name.
adv = bytes((2, 0x01, 0x06, len(name) + 1, 0x09)) + name
ble.gap_advertise(ADVERTISE_US, adv)
print("Classic serial port and BLE advertising as", name.decode())

while True:
    # Scan for BLE devices, echoing serial lines meanwhile.
    heard.clear()
    ble.gap_scan(SCAN_MS, 30_000, 30_000)
    end = time.ticks_add(time.ticks_ms(), SCAN_MS)
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        if port.changes() & btclassic.CONNECTION:
            print("Serial", "connected" if port.connected else "disconnected")
        line = port.readline()
        if line:
            port.write(line.upper())
        time.sleep_ms(20)

    # Report both sides.
    print("BLE devices heard: %d; serial connected: %s" % (len(heard), port.connected))
