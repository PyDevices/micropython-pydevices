# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Find and advertise services on the local network (mDNS and DNS-SD).

A board or a computer can announce what it offers (a web page, a speaker, a
remote control) and find what other devices offer, the way phones and laptops
do. This is CircuitPython's `mdns` module with the same classes, arguments,
defaults and errors, so one program runs on either.

It builds on the ESP32 (ESP-IDF's mDNS component), on the Raspberry Pi Pico W
and Pico 2 W (lwIP's mDNS), and on the unix and Windows ports. The browser
build has no `mdns`.
"""

__revision__: str
"""Which source this firmware was built from, from `git describe`: a release
tag, or the nearest one and the commit. "unknown" when the build had no git."""

class Server:
    """A device's presence on the network: its name, and the services it offers.

    Only one `Server` can be live at a time; making a second raises
    `RuntimeError("mDNS already initialized")` until the first is `deinit`ed.
    A `Server` offers up to 8 services at a time (one with lwIP on the Pico W).

    `hostname` and `instance_name` are read and write properties.
    """

    hostname: str
    """The name the device answers to as `<hostname>.local`. It starts as the
    board's `network.hostname()`, or the computer's name. Setting it renames
    the device for mDNS. On Windows it stays the computer's name, and setting
    another raises `NotImplementedError`."""
    instance_name: str
    """The human-readable name advertised services go by. It is the hostname
    until you set it."""

    def __init__(self, network_interface: object | str | None) -> None:
        """Start answering on the network.

        Args:
            network_interface: On a board, a `network.WLAN` (or `network.LAN`
                on the ESP32), or None for whichever has an address. On unix
                and Windows, None, or on a computer with more than one
                network the IPv4 address of the one to use, such as
                "192.168.1.20".

        Raises:
            ValueError: The interface is none of those.
            RuntimeError: Another `Server` is live.
        """
    def find(self, *, service_type: str, protocol: str, timeout: float = 1) -> tuple[RemoteService, ...]:
        """Look for services other devices offer.

        Ctrl-C ends the wait.

        Args:
            service_type: The kind of service, such as "_http".
            protocol: "_tcp" or "_udp".
            timeout: How many seconds to wait for answers.

        Returns:
            Every matching service that answered in time.
        """
    def advertise_service(
        self,
        *,
        service_type: str,
        protocol: str,
        port: int,
        txt_records: list[str] | tuple[str, ...] | None = None,
    ) -> None:
        """Answer for a service on this device.

        Calling it again for the same `service_type` and `protocol` changes
        the port and the records.

        Args:
            service_type: The kind of service, such as "_http".
            protocol: "_tcp" or "_udp".
            port: The port the service listens on, 0 to 65535.
            txt_records: Up to 32 strings, each "key=value" or a key alone.

        Raises:
            ValueError: The port is out of range, or there are too many
                records or one is not a string.
            RuntimeError: The `Server` is out of service slots.
        """
    def deinit(self) -> None:
        """Withdraw every service this `Server` advertised and free its place.

        A soft reset does this too. On the ESP32 the board keeps answering
        for its hostname.
        """

class RemoteService:
    """A service another device offers. Only `Server.find` makes these, and
    every attribute is read-only."""

    hostname: str
    """The device's name, without `.local`."""
    instance_name: str
    """The service's human-readable name."""
    service_type: str
    """The kind of service, such as "_http"."""
    protocol: str
    """Either "_tcp" or "_udp"."""
    port: int
    """The port the service listens on."""
    ipv4_address: str | None
    """The device's address as a dotted string, or None if none came back.
    (CircuitPython's is an `ipaddress.IPv4Address`; `str()` of either is the
    dotted address.)"""
