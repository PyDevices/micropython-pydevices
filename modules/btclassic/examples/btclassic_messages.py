# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Read your phone's text messages, and hear about new ones.

The board opens a paired phone's messages (MAP). It prints the folders,
how many messages the inbox holds and how many are unread, and the
newest few with their sender and start. Then it asks the phone to report
changes, and prints each new message as it arrives. The phone asks the
first time: allow access to messages on it.

Listing leaves messages as they are. Reading one in full with `get` may
mark it read, and `send`, `mark_read` and `delete` change the phone's
messages; this example uses none of them.

Needs: a classic ESP32 with firmware built with the btclassic module, and
a phone paired with the board, by btclassic_pairing.py or any of the audio
examples.

Run it:

    mpftp run btclassic_messages.py

Tested on:
"""

import time

import btclassic
import network

# Change these to match your setup.
ADDRESS = None  # the phone's 6-byte address, or None for the first paired one
COUNT = 3  # how many of the newest messages to print
PREVIEW = 30  # characters of each message to print

# Wi-Fi and Bluetooth share the radio. Turn Wi-Fi off for a steadier link.
network.WLAN(network.STA_IF).active(False)

# Open the phone's message service. This waits until the phone answers.
bt = btclassic.Bluetooth()
texts = btclassic.Messages(bt)
texts.connect(ADDRESS)
print("Opened; the phone carries", ", ".join(texts.types))

# The folders, and the size of the inbox.
print("Folders:", texts.folders())
print("Inbox: %s messages, %s unread" % (texts.size("inbox"), texts.size("inbox", unread=True)))

# The newest messages, with the start of each as its subject.
for message in texts.list("inbox", count=COUNT, subject_length=PREVIEW):
    print("%s from %s: %s" % (message["datetime"], message["sender_name"], message["subject"]))

# Ask the phone to report changes, then print each new message.
texts.notify()
print("Waiting for new messages...")
while True:
    if texts.changes() & btclassic.NOTIFY:
        event = texts.event()
        while event:
            print(event["type"], "in", event.get("folder"), event.get("subject", ""))
            event = texts.event()
    time.sleep_ms(200)
