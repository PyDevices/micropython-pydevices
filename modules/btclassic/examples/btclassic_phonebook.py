# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Read your phone's contacts and recent calls.

The board opens a paired phone's phone book (PBAP). It prints how many
contacts and calls the phone holds, the first few contacts with their
numbers, the latest missed calls, and the name the phone has for NUMBER.
The phone asks the first time: allow access to contacts on it.

Needs: a board with firmware built with the btclassic module (a classic
ESP32, or a Pico W or Pico 2 W), and a phone paired with the board, by
btclassic_pairing.py or any of the audio examples.

Run it:

    mpftp run btclassic_phonebook.py

Tested on:
"""

import btclassic
import network

# Change these to match your setup.
ADDRESS = None  # the phone's 6-byte address, or None for the first paired one
COUNT = 5  # how many contacts and calls to print
NUMBER = None  # a phone number to look up, or None

# Wi-Fi and Bluetooth share the radio. Turn Wi-Fi off for a steadier link.
network.WLAN(network.STA_IF).active(False)

# Open the phone book. This waits until the phone answers.
bt = btclassic.Bluetooth()
book = btclassic.PhoneBook(bt)
book.connect(ADDRESS)
print("Opened; the phone keeps", ", ".join(book.repositories))

# Count each book without pulling it: contacts, then received, dialed
# and missed calls.
for name in ("pb", "ich", "och", "mch"):
    print("%s: %s entries" % (name, book.size(name)))

# Pull the first few contacts, a page at a time as a small board should.
for contact in book.pull("pb", count=COUNT):
    numbers = ", ".join("%s (%s)" % pair for pair in contact["numbers"])
    print("Contact:", contact["name"], numbers)

# The latest missed calls, newest first, with the time of each.
for call in book.pull("mch", count=COUNT):
    print("Missed:", call["time"], call["name"] or call["numbers"])

# Ask the phone whose number this is, as a call screen would.
if NUMBER:
    print(NUMBER, "is", book.lookup(NUMBER))

book.deinit()
