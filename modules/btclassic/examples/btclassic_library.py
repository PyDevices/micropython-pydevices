# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""List the music on your phone, and play a track from it.

The board connects to a phone as a Bluetooth speaker does, then opens the
phone's media library (AVRCP browsing). It prints the phone's players,
browses the first one that allows it, and lists its top folder or the
results of a search. With PLAY set, it plays the first track listed.

Whether a player can be browsed is up to the phone and its apps. An
Android phone lists one player, "Media Player", whose folders are the
media apps that let Bluetooth browse them; with none, the board prints
the players and stops there. Some apps can be browsed only while they're
open on the phone.

Needs: a Pico W or Pico 2 W with firmware built with the btclassic module
(an ESP32's Bluetooth stack can't browse), and a phone with a music app.

Run it:

    mpftp run btclassic_library.py

Tested on: Pimoroni Pico Plus 2 W, MicroPython 1.30.0-preview with
btclassic, with a Galaxy S21, whose one player can't be browsed until a
music app that allows it is open.
"""

import time

import btclassic

# Change these to match your setup.
SEARCH = None  # text to search the player for, or None to list its folder
PLAY = False  # True plays the first track found

# Start the sink, and wait for the phone to connect.
bt = btclassic.Bluetooth()
music = btclassic.AudioSink(bt)
print("Pair with %s and open a music app." % bt.config("name"))
while not music.connected:
    time.sleep_ms(200)
print("Connected to", music.peer_name)

# Open the phone's library and list its players.
library = btclassic.Library(bt)
library.connect()
players = library.players()
for player in players:
    print("Player %d: %s, browsable %s" % (player["id"], player["name"], player["browsable"]))

# Browse the first player that allows it.
for player in players:
    try:
        print("Browsing", "/".join(library.browse(player["id"])) or player["name"])
        break
    except OSError as error:
        print("Can't browse %s: %s" % (player["name"], error))
else:
    print("No player on this phone can be browsed.")
    raise SystemExit

# List the top folder, or the results of a search.
if SEARCH:
    library.search(SEARCH)
    items = library.items(scope="search")
else:
    items = library.items()
for item in items:
    print("folder" if item["folder"] else "track ", item["name"])

# Play the first track listed.
tracks = [item for item in items if not item["folder"]]
if PLAY and tracks:
    library.play(tracks[0]["uid"], scope="search" if SEARCH else "folder")
    print("Playing", tracks[0]["name"])
library.deinit()
